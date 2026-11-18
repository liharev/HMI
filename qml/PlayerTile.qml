import QtQuick
import QtQuick.Controls
import QtMultimedia
import HmiCore

// A video player tile. Size: player.displayWidth x displayWidth / aspect,
// i.e. the aspect ratio of the stream is always preserved.
Item {
    id: tile
    required property Player player
    property VmsWindow win
    property var view          // MonitorWindow
    property Flickable viewport

    readonly property bool flowMode: win.layoutMode === VmsWindow.Flow
    readonly property bool gridMode: win.layoutMode === VmsWindow.Grid
    readonly property bool freeMode: win.layoutMode === VmsWindow.Free
    readonly property bool expanded: view && view.expandedTile === tile
    property bool dragging: false
    property bool resizing: false
    property real dragX: 0
    property real dragY: 0
    property int zOrder: 0
    property var surface: null       // QObject passed to the player (sink key)

    // Size when expanded to the whole viewport (aspect preserved).
    readonly property real expW: viewport ? Math.min(viewport.width - 8, (viewport.height - 8) * player.aspect) : 0
    readonly property real expH: expW / player.aspect

    x: expanded ? viewport.contentX + (viewport.width - expW) / 2
       : dragging ? dragX : (freeMode ? player.posX : player.layoutX)
    y: expanded ? viewport.contentY + (viewport.height - expH) / 2
       : dragging ? dragY : (freeMode ? player.posY : player.layoutY)
    // Grid: every player takes a whole cell (video letterboxed inside).
    width: expanded ? expW : (gridMode ? player.layoutWidth : player.displayWidth)
    height: expanded ? expH : (gridMode ? player.layoutHeight : player.displayHeight)
    z: expanded ? 6000 : (dragging ? 3000 : zOrder)

    Behavior on x { enabled: !tile.freeMode && !tile.dragging && !tile.resizing; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Behavior on y { enabled: !tile.freeMode && !tile.dragging && !tile.resizing; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    readonly property color statusColor: {
        switch (player.status) {
        case Player.Playing: return "#22c55e"
        case Player.Connecting: return "#eab308"
        case Player.Reconnecting: return "#f97316"
        case Player.Suspended: return "#64748b"
        default: return "#6b7280"
        }
    }

    function clampWidth(w) { return Math.max(160, Math.min(7680, w)) }

    // The control bar hides with a delay: HoverHandler briefly reports
    // "not hovered" while a button is pressed, which must not hide the button.
    readonly property bool wantBar: hover.hovered || dragging || barMouse.pressed || contextMenu.visible
    property bool barShown: false
    onWantBarChanged: {
        if (wantBar) {
            barShown = true
            barHideTimer.stop()
        } else {
            barHideTimer.restart()
        }
    }
    Timer { id: barHideTimer; interval: 700; onTriggered: tile.barShown = false }

    // ---------------------------------------------------------------- video
    Rectangle { anchors.fill: parent; color: "black" }
    // Orientation: mirroring in screen coordinates (outer item), rotation
    // inside; both are GPU transforms of the video surface (no CPU cost).
    Item {
        id: flipper
        anchors.fill: parent
        clip: true
        transform: Scale {
            origin.x: flipper.width / 2
            origin.y: flipper.height / 2
            xScale: tile.player.flipH ? -1 : 1
            yScale: tile.player.flipV ? -1 : 1
        }
        Item {
            id: surfaceHolder
            readonly property bool swap: tile.player.rotation % 180 === 90
            anchors.centerIn: parent
            width: swap ? flipper.height : flipper.width
            height: swap ? flipper.width : flipper.height
            rotation: tile.player.rotation
        }
    }

    Component {
        id: qtSurfaceComponent
        VideoOutput { anchors.fill: parent; fillMode: VideoOutput.PreserveAspectFit }
    }

    Component.onCompleted: {
        if (App.glSink) {
            try {
                const item = Qt.createQmlObject(
                    'import org.freedesktop.gstreamer.Qt6GLVideoItem 1.0; GstGLQt6VideoItem { anchors.fill: parent }',
                    surfaceHolder, "GlVideoSurface")
                // The GL item accepts hover/mouse events itself (navigation
                // events); disable that so the tile's handlers get them.
                item.enabled = false
                surface = item
                player.attachGlItem(item)
                return
            } catch (e) {
                console.warn("GstGLQt6VideoItem unavailable, using VideoOutput:", e)
            }
        }
        const vo = qtSurfaceComponent.createObject(surfaceHolder)
        vo.enabled = false
        surface = vo.videoSink
        player.attachVideoSink(vo.videoSink)
    }
    Component.onDestruction: {
        if (player && surface)
            player.detachSink(surface)
    }

    // ---------------------------------------------------------------- mouse
    HoverHandler { id: hover }

    WheelHandler {
        target: null
        acceptedModifiers: Qt.ControlModifier
        enabled: !tile.gridMode
        onWheel: (ev) => {
            const f = ev.angleDelta.y > 0 ? 1.1 : 1 / 1.1
            tile.player.displayWidth = tile.clampWidth(tile.player.displayWidth * f)
        }
    }

    MouseArea {
        id: videoMouse
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (m) => {
            if (!tile.expanded)
                tile.zOrder = tile.view.nextZ()
            barTimer.restart()      // touch screens: show the controls on tap
        }
        onDoubleClicked: (m) => {
            if (m.button === Qt.LeftButton)
                tile.view.expandedTile = tile.expanded ? null : tile
        }
        onClicked: (m) => {
            if (m.button === Qt.RightButton)
                contextMenu.popup()
        }
    }

    // ---------------------------------------------------------------- status
    Rectangle {
        anchors.fill: parent
        color: "#cc111316"
        visible: tile.player.status !== Player.Playing
        Column {
            anchors.centerIn: parent
            width: parent.width - 20
            spacing: 6
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: visible
                visible: (tile.player.status === Player.Connecting || tile.player.status === Player.Reconnecting)
                         && tile.height > 120
                width: 36; height: 36
            }
            Text {
                width: parent.width
                text: tile.player.statusText
                color: tile.player.status === Player.Reconnecting ? "#fdba74" : "#d1d5db"
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: tile.player.status === Player.Stopped && tile.player.url.length > 0
                text: "▶ Запустить"
                onClicked: tile.player.start()
            }
        }
    }

    // Camera name (always visible).
    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 6
        width: parent.width - 30
        visible: !bar.visible && tile.width >= 200
        text: tile.player.name
        color: "white"
        style: Text.Outline
        styleColor: "black"
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    // Traffic light: green - OK, yellow - problems, red - no video / unusable.
    readonly property color healthColor: player.health === Player.HealthGood ? "#22c55e"
                                       : player.health === Player.HealthWarning ? "#eab308"
                                       : player.health === Player.HealthBad ? "#ef4444" : "#6b7280"
    readonly property real refFps: player.nominalFps > 0 ? player.nominalFps : Math.max(player.inFps, player.fps)
    Rectangle {
        id: healthPill
        visible: tile.player.health !== Player.HealthNone
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 6
        anchors.topMargin: bar.visible ? bar.height + 4 : 6
        height: 20
        radius: 10
        width: healthRow.implicitWidth + 12
        color: "#a6000000"
        border.color: Qt.rgba(tile.healthColor.r, tile.healthColor.g, tile.healthColor.b, 0.7)
        border.width: 1
        z: 10
        Row {
            id: healthRow
            anchors.centerIn: parent
            spacing: 5
            Rectangle {
                width: 10; height: 10; radius: 5
                anchors.verticalCenter: parent.verticalCenter
                color: tile.healthColor
            }
            Text {
                visible: tile.width >= 220
                anchors.verticalCenter: parent.verticalCenter
                color: "white"
                font.pixelSize: 11
                text: {
                    const p = tile.player
                    if (p.status === Player.Playing) {
                        // What the operator really sees: limited by the window repaint rate.
                        const r = tile.win.renderFps
                        const shown = Math.round(r >= 0 ? Math.min(p.shownFps, r) : p.shownFps)
                        return tile.refFps > 0 ? shown + "/" + Math.round(tile.refFps) + " к/с" : shown + " к/с"
                    }
                    if (p.status === Player.Connecting) return "подключение"
                    return "нет видео"
                }
            }
        }
        HoverHandler { id: healthHover }
        ToolTip.visible: healthHover.hovered
        ToolTip.delay: 300
        ToolTip.text: tile.player.healthText
                      + (tile.player.status === Player.Playing
                         ? "\nприём " + tile.player.inFps.toFixed(1) + " · декодирование " + tile.player.fps.toFixed(1)
                           + " · показ " + tile.player.shownFps.toFixed(1)
                           + " · перерисовка окна " + Math.max(0, tile.win.renderFps).toFixed(1) + " к/с"
                           + (tile.player.nominalFps > 0 ? " (номинал " + tile.player.nominalFps.toFixed(1) + ")" : "")
                         : "")
    }

    // Frame: status colour, highlighted while hovered.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: hover.hovered || tile.dragging ? 2 : 1
        border.color: hover.hovered || tile.dragging ? tile.view.accent : Qt.darker(tile.statusColor, 1.6)
    }

    // ---------------------------------------------------------------- top bar
    // Drag handle (always active, also when the bar is hidden): moves the
    // tile; in flow mode this is the manual sorting.
    MouseArea {
        id: barMouse
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 28
        preventStealing: true
        cursorShape: tile.expanded ? Qt.ArrowCursor : (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
        enabled: !tile.expanded
        property point pressPos
        property point offset
        onPressed: (m) => {
            const p = mapToItem(tile.parent, m.x, m.y)
            pressPos = p
            offset = Qt.point(p.x - tile.x, p.y - tile.y)
            tile.zOrder = tile.view.nextZ()
            barTimer.restart()
        }
        onPositionChanged: (m) => {
            const p = mapToItem(tile.parent, m.x, m.y)
            if (!tile.dragging) {
                if (Math.abs(p.x - pressPos.x) + Math.abs(p.y - pressPos.y) < 8)
                    return
                tile.dragX = tile.x
                tile.dragY = tile.y
                tile.dragging = true
            }
            tile.dragX = Math.max(0, p.x - offset.x)
            tile.dragY = Math.max(0, p.y - offset.y)
            if (tile.flowMode)
                tile.view.showDropIndicator(tile.player, p.x, p.y)
        }
        onReleased: (m) => {
            if (!tile.dragging)
                return
            const p = mapToItem(tile.parent, m.x, m.y)
            if (tile.flowMode) {
                tile.view.hideDropIndicator()
                tile.win.movePlayer(tile.player, tile.win.dropIndex(tile.player, p.x, p.y))
            } else {
                tile.player.posX = tile.dragX
                tile.player.posY = tile.dragY
            }
            tile.dragging = false
        }
        onCanceled: {
            tile.view.hideDropIndicator()
            tile.dragging = false
        }
        onDoubleClicked: tile.view.expandedTile = tile.expanded ? null : tile
    }

    Rectangle {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 28
        color: "#b0000000"
        visible: tile.barShown || barTimer.running

        Timer { id: barTimer; interval: 4000 }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            width: parent.width - buttons.width - 16
            clip: true
            Rectangle {
                width: 9; height: 9; radius: 5
                anchors.verticalCenter: parent.verticalCenter
                color: tile.statusColor
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: tile.player.name
                color: "white"
                font.bold: true
                font.pixelSize: 12
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: tile.player.status === Player.Playing
                color: "#cbd5e1"
                font.pixelSize: 11
                text: {
                    const p = tile.player
                    let s = (p.sourceType ? p.sourceType + " · " : "") + p.codec
                    if (p.videoWidth > 0)
                        s += "  " + p.videoWidth + "×" + p.videoHeight
                    s += "  " + p.shownFps.toFixed(0) + " к/с"
                    if (p.decoderName)
                        s += "  " + (p.hardwareDecoding ? "HW " : "SW ") + p.decoderName
                    return s
                }
            }
        }

        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1
            ToolIconButton {
                text: tile.player.active ? "■" : "▶"
                tip: tile.player.active ? "Остановить" : "Запустить"
                onClicked: tile.player.active ? tile.player.stop() : tile.player.start()
            }
            ToolIconButton {
                text: "↻"
                tip: "Переподключить"
                onClicked: tile.player.restart()
            }
            ToolIconButton {
                text: "1:1"
                enabled: !tile.gridMode
                font.pixelSize: 11
                tip: "Оригинальный размер" + (tile.player.videoWidth > 0
                     ? " (" + tile.player.videoWidth + "×" + tile.player.videoHeight + ")" : "")
                onClicked: { tile.view.expandedTile = null; tile.player.resetSize() }
            }
            ToolIconButton {
                text: "⚙"
                tip: "Настройки плеера"
                onClicked: tile.view.openPlayerSettings(tile.player)
            }
            ToolIconButton {
                text: "✕"
                tip: "Удалить плеер"
                fg: "#fca5a5"
                onClicked: removeDialog.open()
            }
        }
    }

    // ---------------------------------------------------------------- resize
    MouseArea {
        id: resizer
        width: 22; height: 22
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: !tile.expanded && !tile.gridMode
        cursorShape: Qt.SizeFDiagCursor
        preventStealing: true
        hoverEnabled: true
        property point startPos
        property real startWidth
        onPressed: (m) => {
            startPos = mapToItem(tile.parent, m.x, m.y)
            startWidth = tile.player.displayWidth
            tile.resizing = true
            tile.zOrder = tile.view.nextZ()
        }
        onPositionChanged: (m) => {
            if (!pressed)
                return
            const p = mapToItem(tile.parent, m.x, m.y)
            const dx = p.x - startPos.x
            const dy = (p.y - startPos.y) * tile.player.aspect
            // The dominant direction defines the new width; height follows
            // from the aspect ratio of the stream.
            tile.player.displayWidth = tile.clampWidth(startWidth + (Math.abs(dx) > Math.abs(dy) ? dx : dy))
        }
        onReleased: tile.resizing = false
        onCanceled: tile.resizing = false

        Canvas {
            anchors.fill: parent
            visible: hover.hovered || resizer.pressed
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = "#e5e7eb"
                ctx.lineWidth = 1.5
                for (let i = 0; i < 3; ++i) {
                    const o = 6 + i * 5
                    ctx.beginPath()
                    ctx.moveTo(width - 3, height - o)
                    ctx.lineTo(width - o, height - 3)
                    ctx.stroke()
                }
            }
        }
        ToolTip.visible: containsMouse && !pressed
        ToolTip.text: "Изменить размер (пропорции сохраняются). Ctrl+колесо — масштаб"
        ToolTip.delay: 800
    }

    // ---------------------------------------------------------------- menus
    Menu {
        id: contextMenu
        width: tile.view.em * 17
        MenuItem {
            text: tile.player.active ? "Остановить" : "Запустить"
            onTriggered: tile.player.active ? tile.player.stop() : tile.player.start()
        }
        MenuItem { text: "Переподключить"; onTriggered: tile.player.restart() }
        MenuItem {
            text: "Веб-интерфейс камеры…"
            // Inactive (dimmed) until the address is set in the player settings.
            enabled: tile.player.webUrl.length > 0
            onTriggered: Qt.openUrlExternally(tile.player.webPageUrl())
        }
        MenuSeparator {}
        Menu {
            title: "Поворот и отражение"
            width: tile.view.em * 17
            MenuItem { text: "Повернуть по часовой (90°)"; onTriggered: tile.player.rotateBy(90) }
            MenuItem { text: "Повернуть против часовой (90°)"; onTriggered: tile.player.rotateBy(-90) }
            MenuItem { text: "Повернуть на 180°"; onTriggered: tile.player.rotateBy(180) }
            MenuSeparator {}
            MenuItem {
                text: "Отразить по горизонтали"
                checkable: true
                checked: tile.player.flipH
                onTriggered: tile.player.flipH = checked
            }
            MenuItem {
                text: "Отразить по вертикали"
                checkable: true
                checked: tile.player.flipV
                onTriggered: tile.player.flipV = checked
            }
            MenuSeparator {}
            MenuItem {
                text: "Сбросить (" + tile.player.rotation + "°)"
                enabled: tile.player.rotation !== 0 || tile.player.flipH || tile.player.flipV
                onTriggered: tile.player.resetOrientation()
            }
        }
        MenuSeparator {}
        MenuItem { text: "Оригинальный размер"; enabled: !tile.gridMode; onTriggered: { tile.view.expandedTile = null; tile.player.resetSize() } }
        MenuItem { text: tile.expanded ? "Свернуть" : "Развернуть в окне"; onTriggered: tile.view.expandedTile = tile.expanded ? null : tile }
        MenuItem { text: "Увеличить (+25%)"; enabled: !tile.gridMode; onTriggered: tile.player.displayWidth = tile.clampWidth(tile.player.displayWidth * 1.25) }
        MenuItem { text: "Уменьшить (−20%)"; enabled: !tile.gridMode; onTriggered: tile.player.displayWidth = tile.clampWidth(tile.player.displayWidth * 0.8) }
        MenuSeparator {}
        MenuItem { text: "Переместить в начало"; onTriggered: tile.win.movePlayer(tile.player, 0) }
        MenuItem { text: "Переместить в конец"; onTriggered: tile.win.movePlayer(tile.player, tile.win.playerCount) }
        MenuItem { text: "На передний план"; enabled: !tile.flowMode; onTriggered: tile.zOrder = tile.view.nextZ() }
        MenuSeparator {}
        MenuItem { text: "Настройки…"; onTriggered: tile.view.openPlayerSettings(tile.player) }
        MenuItem { text: "Удалить…"; onTriggered: removeDialog.open() }
    }

    ConfirmDialog {
        id: removeDialog
        title: "Удалить плеер «" + tile.player.name + "»?"
        text: "Плеер будет удалён из окна, поток будет остановлен."
        onAccepted: {
            if (tile.view.expandedTile === tile)
                tile.view.expandedTile = null
            tile.win.removePlayer(tile.player)
        }
    }
}
