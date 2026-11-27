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
    property real resizeW: 0          // grid: live size while the corner is dragged
    property real resizeH: 0
    property real dragX: 0
    property real dragY: 0

    // Size when expanded to the whole viewport (aspect preserved).
    readonly property real expW: viewport ? Math.min(viewport.width - 8, (viewport.height - 8) * player.aspect) : 0
    readonly property real expH: expW / player.aspect

    x: expanded ? viewport.contentX + (viewport.width - expW) / 2
       : dragging ? dragX : (freeMode ? player.posX : player.layoutX)
    y: expanded ? viewport.contentY + (viewport.height - expH) / 2
       : dragging ? dragY : (freeMode ? player.posY : player.layoutY)
    // Grid: every player takes a whole cell (video letterboxed inside).
    width: expanded ? expW : (gridMode ? (resizing ? resizeW : player.layoutWidth) : player.displayWidth)
    height: expanded ? expH : (gridMode ? (resizing ? resizeH : player.layoutHeight) : player.displayHeight)
    z: expanded ? 6000 : (dragging || resizing ? 3000 : (freeMode ? player.zOrder : 0))

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

    function bringToFront() { if (freeMode) win.raise(player) }
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

    // Two surfaces: the shown one and the one a new stream is prepared on
    // (switching without a black frame). Only the shown one is visible.
    property var surfaceItems: [null, null]
    property var surfaces: [null, null]          // objects passed to the player
    readonly property var surfaceItem: surfaceItems[player.surfaceIndex]
    readonly property var surface: surfaces[player.surfaceIndex]
    function destroySurfaces() {
        for (let i = 0; i < 2; ++i) {
            const it = surfaceItems[i]
            if (!it)
                continue
            if (surfaces[i])
                player.detachSink(surfaces[i])
            // GstGLQt6VideoItem crashes if it is destroyed while still
            // parented (its windowChanged handler runs during destruction):
            // unparent it first, while it is fully alive.
            it.parent = null
            it.destroy()
        }
        surfaceItems = [null, null]
        surfaces = [null, null]
    }
    function createSurface() {
        destroySurfaces()
        const items = [null, null]
        const objs = [null, null]
        let gl = App.glSink && !player.useQtSink
        for (let i = 0; i < 2; ++i) {
            if (gl) {
                try {
                    const item = Qt.createQmlObject(
                        'import org.freedesktop.gstreamer.Qt6GLVideoItem 1.0; GstGLQt6VideoItem { anchors.fill: parent }',
                        surfaceHolder, "GlVideoSurface" + i)
                    // The GL item accepts hover/mouse events itself (navigation
                    // events); disable that so the tile's handlers get them.
                    item.enabled = false
                    items[i] = item
                    objs[i] = item
                    continue
                } catch (e) {
                    console.warn("GstGLQt6VideoItem unavailable, using VideoOutput:", e)
                    gl = false
                }
            }
            const vo = qtSurfaceComponent.createObject(surfaceHolder)
            vo.enabled = false
            items[i] = vo
            objs[i] = vo.videoSink
        }
        for (let i = 0; i < 2; ++i) {
            const idx = i
            items[i].visible = Qt.binding(() => tile.player.surfaceIndex === idx)
        }
        surfaceItems = items
        surfaces = objs
        for (let i = 0; i < 2; ++i) {
            if (gl)
                player.attachGlItem(items[i], i)
            else
                player.attachVideoSink(objs[i], i)
        }
    }
    Component.onCompleted: {
        createSurface()
        pushViewSize()
    }
    // The player chooses the camera stream that suits its size on the screen.
    function pushViewSize() { player.setViewSize(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio) }
    onWidthChanged: pushViewSize()
    onHeightChanged: pushViewSize()
    // Automatic fallback (GL upload too slow) switches to Qt Multimedia.
    Connections {
        target: tile.player
        function onRenderPathChanged() {
            const wantQt = !App.glSink || tile.player.useQtSink
            const isQt = tile.surfaceItems[0] && tile.surfaces[0] !== tile.surfaceItems[0]
            if (wantQt !== isQt)
                tile.createSurface()
        }
    }
    Component.onDestruction: {
        if (player) {
            for (let i = 0; i < 2; ++i)
                if (surfaces[i])
                    player.detachSink(surfaces[i])
        }
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
                tile.bringToFront()
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
    // Shown only when there is nothing current to look at: the last frame
    // stays visible while a stream (re)connects or switches, the panel
    // appears after the "waiting for the stream" time. Its text shrinks to
    // fit, so it never leaves the tile.
    Rectangle {
        id: statusPanel
        visible: tile.player.overlayVisible
        readonly property bool busy: tile.player.status === Player.Connecting
                                     || tile.player.status === Player.Reconnecting || tile.player.switching
        readonly property bool showBusy: busy && tile.height >= 150 && tile.width >= 150
        readonly property bool showStart: tile.player.status === Player.Stopped && tile.player.url.length > 0
                                          && tile.height >= 90
        readonly property real pad: Math.max(3, Math.min(8, tile.height / 20))
        readonly property real extras: (showBusy ? busyInd.height + 4 : 0) + (showStart ? startBtn.height + 4 : 0)
        anchors.centerIn: parent
        width: Math.max(10, Math.min(tile.width - 8, 440))
        height: Math.max(10, Math.min(tile.height - 8, statusMetrics.height + extras + 2 * pad))
        radius: Math.min(6, height / 4)
        color: tile.player.status === Player.Playing ? "#b3111316" : "#d9111316"
        border.color: tile.player.status === Player.Reconnecting ? "#7c2d12" : "#374151"
        border.width: 1
        clip: true
        z: 5

        // Height the text needs at the normal size (the panel grows up to the tile).
        Text {
            id: statusMetrics
            visible: false
            width: statusPanel.width - 2 * statusPanel.pad
            text: statusText.text
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
        Column {
            anchors.fill: parent
            anchors.margins: statusPanel.pad
            spacing: 4
            BusyIndicator {
                id: busyInd
                anchors.horizontalCenter: parent.horizontalCenter
                visible: statusPanel.showBusy
                running: visible
                width: 32; height: 32
            }
            Text {
                id: statusText
                width: parent.width
                height: Math.max(6, parent.height - statusPanel.extras)
                text: tile.player.overlayText
                color: tile.player.status === Player.Reconnecting ? "#fdba74" : "#d1d5db"
                font.pixelSize: 13
                fontSizeMode: Text.Fit
                minimumPixelSize: 6
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
                elide: Text.ElideRight
            }
            Button {
                id: startBtn
                anchors.horizontalCenter: parent.horizontalCenter
                visible: statusPanel.showStart
                text: "Запустить"
                onClicked: tile.player.start()
            }
        }
    }

    // State of the source reported by the device (e.g. CAMAI: no signal on
    // the input, the server shows its splash screen).
    Rectangle {
        id: sourceNote
        visible: tile.player.sourceNote.length > 0 && !tile.player.overlayVisible && tile.height >= 80
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: tile.width >= 200 ? 26 : 4
        width: Math.min(tile.width - 12, noteText.implicitWidth + noteIcon.width + 22)
        height: noteText.implicitHeight + 6
        radius: height / 2
        color: "#cc422006"
        border.color: "#f59e0b"
        z: 6
        Icon {
            id: noteIcon
            name: "warning"
            color: "#fde68a"
            width: noteText.font.pixelSize + 2; height: width
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            id: noteText
            anchors.left: noteIcon.right
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - noteIcon.width - 18
            text: tile.player.sourceNote
            color: "#fde68a"
            font.pixelSize: 11
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
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
    readonly property real refFps: player.refFps
    Rectangle {
        id: healthPill
        // The status panel carries the same information when it is shown.
        visible: tile.player.health !== Player.HealthNone && !tile.player.overlayVisible
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
                        // What the operator really sees: limited by the window repaint
                        // rate and never above the stream's own rate.
                        const shown = Math.round(p.displayFps)
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
            tile.bringToFront()
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
            if (tile.flowMode || tile.gridMode)
                tile.view.showDropIndicator(tile.player, p.x, p.y)
        }
        onReleased: (m) => {
            if (!tile.dragging)
                return
            const p = mapToItem(tile.parent, m.x, m.y)
            if (tile.flowMode || tile.gridMode) {
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
                    s += "  " + p.displayFps.toFixed(0) + " к/с"
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
                iconName: tile.player.active ? "stop" : "play"
                tip: tile.player.active ? "Остановить" : "Запустить"
                onClicked: tile.player.active ? tile.player.stop() : tile.player.start()
            }
            ToolIconButton {
                iconName: "reload"
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
                iconName: "gear"
                tip: "Настройки плеера"
                onClicked: tile.view.openPlayerSettings(tile.player)
            }
            ToolIconButton {
                iconName: "close"
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
        visible: !tile.expanded
        cursorShape: Qt.SizeFDiagCursor
        preventStealing: true
        hoverEnabled: true
        property point startPos
        property real startWidth
        onPressed: (m) => {
            startPos = mapToItem(tile.parent, m.x, m.y)
            startWidth = tile.player.displayWidth
            tile.resizeW = tile.width
            tile.resizeH = tile.height
            tile.resizing = true
            tile.bringToFront()
        }
        onPositionChanged: (m) => {
            if (!pressed)
                return
            const p = mapToItem(tile.parent, m.x, m.y)
            if (tile.gridMode) {
                // Any rectangle of whole cells: the outline shows the snapped size.
                tile.resizeW = Math.max(tile.win.cellWidth * 0.5, tile.player.layoutWidth + p.x - startPos.x)
                tile.resizeH = Math.max(tile.win.cellHeight * 0.5, tile.player.layoutHeight + p.y - startPos.y)
                const sp = tile.win.snapSpan(tile.resizeW, tile.resizeH)
                tile.view.showRect(Qt.rect(tile.x, tile.y,
                                           sp.width * tile.win.cellWidth + (sp.width - 1) * 6,
                                           sp.height * tile.win.cellHeight + (sp.height - 1) * 6))
                return
            }
            const dx = p.x - startPos.x
            const dy = (p.y - startPos.y) * tile.player.aspect
            // The dominant direction defines the new width; height follows
            // from the aspect ratio of the stream.
            tile.player.displayWidth = tile.clampWidth(startWidth + (Math.abs(dx) > Math.abs(dy) ? dx : dy))
        }
        onReleased: {
            if (tile.gridMode) {
                const sp = tile.win.snapSpan(tile.resizeW, tile.resizeH)
                tile.view.hideDropIndicator()
                tile.resizing = false
                // The others are packed again around the new size.
                tile.win.setSpan(tile.player, sp.width, sp.height)
                return
            }
            tile.resizing = false
        }
        onCanceled: { tile.resizing = false; tile.view.hideDropIndicator() }

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
        ToolTip.text: tile.gridMode ? "Растянуть на несколько ячеек сетки (остальные плееры перестроятся)"
                                    : "Изменить размер (пропорции сохраняются). Ctrl+колесо — масштаб"
        ToolTip.delay: 800
    }

    // ---------------------------------------------------------------- sound
    // Speaker in the bottom-right corner when the stream (or another stream of
    // the camera) has sound: crossed out = off (default), waves = playing.
    // Only one player plays sound; switching it on here switches it off elsewhere.
    Rectangle {
        id: speaker
        visible: tile.player.audioAvailable && tile.width >= 110 && tile.height >= 70
        width: 28; height: 28; radius: 14
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 26          // the resize corner stays usable
        anchors.bottomMargin: 4
        color: speakerMouse.containsMouse ? "#cc1f2937" : "#99000000"
        border.color: tile.player.audioOn ? tile.view.accent : "#66ffffff"
        border.width: tile.player.audioOn ? 2 : 1
        z: 20
        Canvas {
            id: speakerIcon
            anchors.fill: parent
            anchors.margins: 6
            readonly property bool on: tile.player.audioOn
            onOnChanged: requestPaint()
            onPaint: {
                const c = getContext("2d")
                const w = width, h = height
                c.clearRect(0, 0, w, h)
                c.fillStyle = on ? "#ffffff" : "#d1d5db"
                c.strokeStyle = c.fillStyle
                c.lineWidth = 1.6
                // speaker body
                c.beginPath()
                c.moveTo(0, h * 0.35); c.lineTo(w * 0.25, h * 0.35); c.lineTo(w * 0.52, h * 0.1)
                c.lineTo(w * 0.52, h * 0.9); c.lineTo(w * 0.25, h * 0.65); c.lineTo(0, h * 0.65)
                c.closePath(); c.fill()
                c.beginPath()
                if (on) {
                    c.arc(w * 0.5, h * 0.5, w * 0.25, -0.8, 0.8)
                    c.moveTo(w * 0.5 + w * 0.42 * Math.cos(-0.85), h * 0.5 + w * 0.42 * Math.sin(-0.85))
                    c.arc(w * 0.5, h * 0.5, w * 0.42, -0.85, 0.85)
                } else {
                    c.strokeStyle = "#f87171"
                    c.moveTo(w * 0.66, h * 0.3); c.lineTo(w * 0.98, h * 0.7)
                    c.moveTo(w * 0.98, h * 0.3); c.lineTo(w * 0.66, h * 0.7)
                }
                c.stroke()
            }
        }
        MouseArea {
            id: speakerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: tile.player.setAudioEnabled(!tile.player.audioOn)
        }
        ToolTip.visible: speakerMouse.containsMouse
        ToolTip.delay: 400
        ToolTip.text: (tile.player.audioOn ? "Выключить звук" : "Включить звук (у других плееров звук выключится)")
                      + (tile.player.audioNote ? "\n" + tile.player.audioNote : "")
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
        MenuItem {
            text: "Одна ячейка сетки (сейчас " + tile.player.colSpan + "×" + tile.player.rowSpan + ")"
            visible: tile.gridMode
            height: visible ? implicitHeight : 0
            enabled: tile.player.colSpan > 1 || tile.player.rowSpan > 1
            onTriggered: tile.win.setSpan(tile.player, 1, 1)
        }
        MenuItem { text: tile.expanded ? "Свернуть" : "Развернуть в окне"; onTriggered: tile.view.expandedTile = tile.expanded ? null : tile }
        MenuItem { text: "Увеличить (+25%)"; enabled: !tile.gridMode; onTriggered: tile.player.displayWidth = tile.clampWidth(tile.player.displayWidth * 1.25) }
        MenuItem { text: "Уменьшить (-20%)"; enabled: !tile.gridMode; onTriggered: tile.player.displayWidth = tile.clampWidth(tile.player.displayWidth * 0.8) }
        MenuSeparator {}
        MenuItem { text: "Переместить в начало"; onTriggered: tile.win.movePlayer(tile.player, 0) }
        MenuItem { text: "Переместить в конец"; onTriggered: tile.win.movePlayer(tile.player, tile.win.playerCount) }
        MenuItem { text: "На передний план"; enabled: !tile.flowMode; onTriggered: tile.bringToFront() }
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
