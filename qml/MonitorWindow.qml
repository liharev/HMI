import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import HmiCore

// One monitor window. Created from C++ (AppController::createView).
ApplicationWindow {
    id: root
    required property VmsWindow win

    readonly property color accent: win ? win.color : "#2563eb"
    readonly property color accentText: (0.299 * accent.r + 0.587 * accent.g + 0.114 * accent.b) > 0.62
                                        ? "#111111" : "#ffffff"
    readonly property bool flowMode: win && win.layoutMode === VmsWindow.Flow
    readonly property bool gridMode: win && win.layoutMode === VmsWindow.Grid
    readonly property bool freeMode: win && win.layoutMode === VmsWindow.Free
    // Size unit derived from the system font: dialogs and menus scale with it.
    readonly property real em: fontMetrics.height
    FontMetrics { id: fontMetrics; font: root.font }
    property bool renderWarningClosed: false
    property int zCounter: 1
    property var expandedTile: null

    title: win ? win.title : App.productName
    color: "#0e1013"

    // Dark theme for all controls, menus and dialogs of this window.
    palette.window: "#1d2024"
    palette.windowText: "#e6e6e6"
    palette.base: "#15171a"
    palette.alternateBase: "#24272c"
    palette.text: "#e6e6e6"
    palette.button: "#2a2e34"
    palette.buttonText: "#e6e6e6"
    palette.highlight: "#2563eb"
    palette.highlightedText: "#ffffff"
    palette.light: "#3a3f46"
    palette.midlight: "#32363c"
    palette.mid: "#25282d"
    palette.dark: "#121417"
    palette.shadow: "#000000"
    palette.toolTipBase: "#2a2e34"
    palette.toolTipText: "#e6e6e6"
    palette.placeholderText: "#80868e"
    palette.disabled.text: "#6b7078"
    palette.disabled.windowText: "#6b7078"
    palette.disabled.buttonText: "#6b7078"
    palette.disabled.highlightedText: "#9ca3af"
    minimumWidth: 480
    minimumHeight: 320

    onClosing: (close) => {
        close.accepted = false
        App.closeWindow(win)
    }

    function nextZ() { zCounter += 1; return zCounter }
    function openPlayerSettings(p) { playerDialog.openFor(p) }

    function showDropIndicator(player, x, y) {
        const r = win.dropIndicator(player, x, y)
        dropBar.x = r.x; dropBar.y = r.y; dropBar.width = r.width; dropBar.height = r.height
        dropBar.visible = true
    }
    function hideDropIndicator() { dropBar.visible = false }

    function sortLabel(key) {
        switch (key) {
        case "name": return "по имени"
        case "url": return "по адресу"
        case "size": return "по размеру"
        case "resolution": return "по разрешению"
        case "status": return "по состоянию"
        case "created": return "по времени добавления"
        }
        return ""
    }

    Shortcut { sequence: "Ctrl+N"; onActivated: App.newWindow() }
    Shortcut { sequences: ["Insert", "Ctrl+I"]; onActivated: playerDialog.openFor(null) }
    Shortcut { sequence: "F11"; onActivated: App.toggleFullScreen(root.win) }
    Shortcut { sequence: "Ctrl+Q"; onActivated: App.quit() }
    Shortcut { sequence: "Ctrl+G"; onActivated: root.win.fitAll() }
    Shortcut {
        sequence: "Esc"
        enabled: root.expandedTile !== null
        onActivated: root.expandedTile = null
    }

    // ------------------------------------------------------------------ header
    header: ToolBar {
        id: toolbar
        height: Math.max(40, Math.round(root.em * 2.3))
        palette.buttonText: root.accentText
        palette.windowText: root.accentText
        background: Rectangle {
            color: root.accent
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.darker(root.accent, 1.4) }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 2

            ToolButton {
                id: swatch
                Layout.preferredWidth: 30
                focusPolicy: Qt.NoFocus
                contentItem: Rectangle {
                    implicitWidth: 18; implicitHeight: 18
                    radius: 9
                    color: root.accent
                    border.color: root.accentText
                    border.width: 2
                }
                ToolTip.visible: hovered
                ToolTip.text: "Название и цвет окна"
                onClicked: windowDialog.open()
            }
            Label {
                text: root.win ? root.win.title : ""
                font.pixelSize: Math.round(root.em * 0.95)
                font.bold: true
                elide: Text.ElideRight
                Layout.maximumWidth: root.em * 20
                MouseArea {
                    anchors.fill: parent
                    onDoubleClicked: windowDialog.open()
                }
            }
            Label {
                text: root.win ? "· камер: " + root.win.playerCount : ""
                opacity: 0.8
                Layout.leftMargin: 6
            }
            Item { Layout.fillWidth: true }

            HeaderButton {
                fg: root.accentText
                text: "＋ Камера"
                onClicked: playerDialog.openFor(null)
                tip: "Добавить плеер (Insert)"
            }
            Rectangle { width: 1; height: 20; color: root.accentText; opacity: 0.35; Layout.leftMargin: 4; Layout.rightMargin: 4 }
            HeaderButton {
                fg: root.accentText
                text: "Поток"
                checked: root.flowMode
                onClicked: root.win.layoutMode = VmsWindow.Flow
                tip: "Плееры выстраиваются по порядку с переносом строк"
            }
            HeaderButton {
                fg: root.accentText
                text: "Свободно"
                checked: root.freeMode
                onClicked: root.win.layoutMode = VmsWindow.Free
                tip: "Плееры размещаются в любом месте окна"
            }
            HeaderButton {
                id: gridButton
                fg: root.accentText
                text: "Сетка " + root.win.gridRows + "×" + root.win.gridCols + " ▾"
                checked: root.gridMode
                onClicked: gridPopup.open()
                tip: "Все плееры одного размера в сетке N строк × M столбцов на всё окно"
            }
            Rectangle { width: 1; height: 20; color: root.accentText; opacity: 0.35; Layout.leftMargin: 4; Layout.rightMargin: 4 }
            HeaderButton {
                fg: root.accentText
                text: "Сортировка ▾"
                onClicked: sortMenu.popup(this, 0, height)
            }
            HeaderButton {
                fg: root.accentText
                text: "Окна ▾"
                onClicked: windowsMenu.popup(this, 0, height)
            }
            HeaderButton {
                fg: root.accentText
                text: "⋮"
                font.pixelSize: 18
                onClicked: mainMenu.popup(this, 0, height)
            }
        }
    }

    // ------------------------------------------------------------------ menus
    Popup {
        id: gridPopup
        parent: gridButton
        y: gridButton.height + 2
        x: Math.min(0, root.width - width - gridButton.mapToItem(null, 0, 0).x - 8)
        padding: Math.round(root.em * 0.6)
        modal: false
        focus: true
        onAboutToShow: {
            rowsSpin.value = root.win.gridRows
            colsSpin.value = root.win.gridCols
        }
        contentItem: ColumnLayout {
            spacing: Math.round(root.em * 0.4)
            Label { text: "Сетка: строк × столбцов"; font.bold: true }
            GridLayout {
                columns: 4
                rowSpacing: 4
                columnSpacing: 4
                Repeater {
                    model: [[1, 1], [1, 2], [2, 2], [2, 3], [3, 3], [3, 4], [4, 4], [4, 5], [5, 5], [4, 6], [6, 6], [8, 8]]
                    delegate: Button {
                        required property var modelData
                        text: modelData[0] + " × " + modelData[1]
                        Layout.fillWidth: true
                        highlighted: root.gridMode && root.win.gridRows === modelData[0] && root.win.gridCols === modelData[1]
                        onClicked: {
                            root.win.setGrid(modelData[0], modelData[1])
                            gridPopup.close()
                        }
                    }
                }
            }
            RowLayout {
                spacing: 6
                Label { text: "Строк" }
                SpinBox { id: rowsSpin; from: 1; to: 16; editable: true }
                Label { text: "Столбцов" }
                SpinBox { id: colsSpin; from: 1; to: 16; editable: true }
            }
            Button {
                text: "Применить " + rowsSpin.value + " × " + colsSpin.value
                Layout.alignment: Qt.AlignRight
                onClicked: {
                    root.win.setGrid(rowsSpin.value, colsSpin.value)
                    gridPopup.close()
                }
            }
            Label {
                Layout.maximumWidth: root.em * 22
                wrapMode: Text.Wrap
                opacity: 0.65
                font.pixelSize: Math.round(root.em * 0.68)
                text: "Плееры заполняют ячейки по порядку (порядок меняется перетаскиванием и сортировкой); "
                      + "если плееров больше, чем ячеек, они продолжаются ниже с прокруткой."
            }
        }
    }

    Menu {
        id: sortMenu
        width: root.em * 24
        MenuItem { text: "По имени (А → Я)"; onTriggered: root.win.sortBy("name", true) }
        MenuItem { text: "По имени (Я → А)"; onTriggered: root.win.sortBy("name", false) }
        MenuItem { text: "По адресу потока"; onTriggered: root.win.sortBy("url", true) }
        MenuItem { text: "По размеру плеера (больше → меньше)"; onTriggered: root.win.sortBy("size", false) }
        MenuItem { text: "По разрешению видео (больше → меньше)"; onTriggered: root.win.sortBy("resolution", false) }
        MenuItem { text: "По состоянию (проблемные первыми)"; onTriggered: root.win.sortBy("status", false) }
        MenuItem { text: "По времени добавления"; onTriggered: root.win.sortBy("created", true) }
        MenuSeparator {}
        MenuItem {
            text: "Поддерживать сортировку автоматически"
                  + (root.win.sortKey ? " (" + root.sortLabel(root.win.sortKey) + ")" : "")
            checkable: true
            checked: root.win.autoSort
            onTriggered: root.win.autoSort = checked
        }
        MenuSeparator {}
        MenuItem { text: "Уместить все в окне\tCtrl+G"; enabled: !root.gridMode; onTriggered: root.win.fitAll() }
        MenuItem { text: "Оригинальный размер для всех"; onTriggered: root.win.originalSizeAll() }
        MenuItem {
            text: "Выстроить по порядку"
            enabled: root.freeMode
            onTriggered: root.win.arrange()
        }
        MenuSeparator {}
        MenuItem {
            enabled: false
            text: "Вручную: перетащите плеер за верхнюю панель"
        }
    }

    Menu {
        id: windowsMenu
        width: root.em * 20
        MenuItem { text: "Новое окно\tCtrl+N"; onTriggered: App.newWindow() }
        MenuSeparator {}
        Instantiator {
            model: App.windows
            delegate: MenuItem {
                required property var modelData
                text: (modelData === root.win ? "● " : (modelData.open ? "○ " : "    "))
                      + modelData.title + (modelData.open ? "" : "  (закрыто)")
                onTriggered: {
                    // The list is rebuilt when a window opens: act after the menu closed.
                    const w = modelData
                    windowsMenu.close()
                    Qt.callLater(() => App.openWindow(w))
                }
            }
            onObjectAdded: (index, object) => windowsMenu.insertItem(2 + index, object)
            onObjectRemoved: (index, object) => windowsMenu.removeItem(object)
        }
        MenuSeparator {}
        Menu {
            id: screenMenu
            width: root.em * 22
            title: "Переместить на экран"
            Instantiator {
                model: App.screens
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.label
                    checkable: true
                    checked: root.screen && root.screen.name === modelData.name
                    onTriggered: App.moveToScreen(root.win, modelData.name)
                }
                onObjectAdded: (index, object) => screenMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => screenMenu.removeItem(object)
            }
        }
        MenuItem {
            text: "Во весь экран\tF11"
            checkable: true
            checked: root.visibility === Window.FullScreen
            onTriggered: App.toggleFullScreen(root.win)
        }
        MenuSeparator {}
        MenuItem { text: "Название, цвет и обои…"; onTriggered: windowDialog.open() }
        MenuItem { text: "Удалить окно…"; onTriggered: deleteDialog.open() }
    }

    Menu {
        id: mainMenu
        width: root.em * 16
        MenuItem { text: "Запустить все плееры"; onTriggered: root.win.startAll() }
        MenuItem { text: "Остановить все плееры"; onTriggered: root.win.stopAll() }
        MenuSeparator {}
        MenuItem { text: "О программе…"; onTriggered: aboutDialog.open() }
        MenuItem { text: "Выход\tCtrl+Q"; onTriggered: App.quit() }
    }

    // ------------------------------------------------------------------ content
    // Wallpaper (built-in CAMAI or a user image) with optional dimming.
    Image {
        id: wallpaperImage
        anchors.fill: parent
        source: root.win.wallpaper
        visible: root.win.wallpaper.length > 0 && status === Image.Ready
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        // Decode once at screen size: no reload while the window is resized.
        sourceSize.width: root.screen ? root.screen.width : 1920
    }
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: root.win.wallpaperDim
        visible: wallpaperImage.visible && opacity > 0
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 3
        anchors.topMargin: renderWarning.visible ? renderWarning.height + 3 : 3
        clip: true
        contentWidth: Math.max(width, root.win.contentWidth)
        contentHeight: Math.max(height, root.win.contentHeight)
        boundsBehavior: Flickable.StopAtBounds
        interactive: root.expandedTile === null
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

        onWidthChanged: root.win.viewportWidth = width
        onHeightChanged: root.win.viewportHeight = height
        Component.onCompleted: {
            root.win.viewportWidth = width
            root.win.viewportHeight = height
        }

        Item {
            id: canvas
            width: flick.contentWidth
            height: flick.contentHeight

            // Dim background behind an expanded player.
            Rectangle {
                x: flick.contentX; y: flick.contentY
                width: flick.width; height: flick.height
                color: "#e0000000"
                visible: root.expandedTile !== null
                z: 5000
                MouseArea { anchors.fill: parent; onClicked: root.expandedTile = null }
            }

            // Free cells of the grid: double click adds a camera.
            Repeater {
                model: root.gridMode ? Math.max(0, root.win.gridRows * root.win.gridCols - root.win.playerCount) : 0
                delegate: Rectangle {
                    required property int index
                    readonly property point pos: {
                        root.win.cellWidth; root.win.cellHeight; root.win.gridCols  // re-evaluate on change
                        return root.win.cellPos(root.win.playerCount + index)
                    }
                    x: pos.x
                    y: pos.y
                    width: root.win.cellWidth
                    height: root.win.cellHeight
                    color: "#0affffff"
                    border.color: "#22ffffff"
                    radius: 2
                    Label {
                        anchors.centerIn: parent
                        text: "＋"
                        opacity: 0.25
                        font.pixelSize: Math.min(parent.width, parent.height) * 0.18
                    }
                    MouseArea {
                        anchors.fill: parent
                        onDoubleClicked: playerDialog.openFor(null)
                    }
                }
            }

            Repeater {
                model: root.win.players
                delegate: PlayerTile {
                    view: root
                    win: root.win
                    viewport: flick
                }
            }

            Rectangle {
                id: dropBar
                visible: false
                color: root.accent
                radius: 2
                z: 4000
            }
        }
    }

    // The UI is rendered without a GPU driver (llvmpipe): video will lag.
    Rectangle {
        id: renderWarning
        visible: App.softwareRendering && !root.renderWarningClosed
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 3
        height: warnText.implicitHeight + 14
        color: "#e07f1d1d"
        z: 20
        Label {
            id: warnText
            anchors.left: parent.left
            anchors.right: warnClose.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 8
            wrapMode: Text.Wrap
            color: "white"
            text: "Графика работает без аппаратного ускорения (" + App.glRenderer + "). "
                  + "Видео будет отображаться с пропусками кадров. Установите/включите драйвер GPU Mali "
                  + "(Panfrost/Panthor или libmali) — см. раздел «Диагностика» в README."
        }
        ToolButton {
            id: warnClose
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: "✕"
            onClicked: root.renderWarningClosed = true
        }
    }

    // Accent frame around the window content.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: root.accent
        border.width: 3
        z: 10
    }

    Column {
        anchors.centerIn: parent
        spacing: 12
        visible: root.win.playerCount === 0
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "В окне нет плееров"
            font.pixelSize: 20
            opacity: 0.7
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Добавить камеру"
            onClicked: playerDialog.openFor(null)
        }
    }

    // ------------------------------------------------------------------ dialogs
    PlayerDialog { id: playerDialog; win: root.win }
    WindowDialog { id: windowDialog; win: root.win }

    ConfirmDialog {
        id: deleteDialog
        title: "Удалить окно «" + root.win.title + "»?"
        text: "Окно и все его плееры (" + root.win.playerCount + ") будут удалены из конфигурации."
        onAccepted: App.deleteWindow(root.win)
    }

    Dialog {
        id: aboutDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: App.productName + " " + App.version
        width: Math.min(root.em * 38, root.width - 40)
        footer: DialogButtonBox {
            Button { text: "Закрыть"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        }
        contentItem: GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 6
            Label {
                Layout.columnSpan: 2
                text: App.productName + " — рабочее место администратора видеонаблюдения"
                font.bold: true
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label { text: "Версия:"; opacity: 0.7 }
            Label { text: App.version; Layout.fillWidth: true }
            Label { text: "Вывод видео:"; opacity: 0.7 }
            Label { text: App.renderBackend; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredWidth: 1 }
            Label { text: "Графика (GPU):"; opacity: 0.7 }
            Label {
                text: (App.glRenderer || "—") + (App.softwareRendering ? "  ⚠ без аппаратного ускорения" : "")
                color: App.softwareRendering ? "#fca5a5" : palette.windowText
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.preferredWidth: 1
            }
            Label { text: "Декодирование:"; opacity: 0.7 }
            Label { text: App.decoderBalance || "—"; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredWidth: 1 }
            Label { text: "Аппаратные декодеры:"; opacity: 0.7 }
            Label { text: App.hardwareDecoders; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.preferredWidth: 1 }
            Label { text: "Конфигурация:"; opacity: 0.7 }
            Label { text: App.configPath; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; Layout.preferredWidth: 1 }
            Label { text: "Экран окна:"; opacity: 0.7 }
            Label {
                text: root.screen ? root.screen.name + " (" + root.screen.width + "×" + root.screen.height + ")" : ""
                Layout.fillWidth: true
            }
        }
    }
}
