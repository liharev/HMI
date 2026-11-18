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
    property int zCounter: 1
    property var expandedTile: null

    title: win ? win.title : "HMI VMS"
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
        height: 40
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
                font.pixelSize: 16
                font.bold: true
                elide: Text.ElideRight
                Layout.maximumWidth: 360
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
                checked: !root.flowMode
                onClicked: root.win.layoutMode = VmsWindow.Free
                tip: "Плееры размещаются в любом месте окна"
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
    Menu {
        id: sortMenu
        width: 400
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
        MenuItem { text: "Уместить все в окне (сетка)\tCtrl+G"; onTriggered: root.win.fitAll() }
        MenuItem { text: "Оригинальный размер для всех"; onTriggered: root.win.originalSizeAll() }
        MenuItem {
            text: "Выстроить по порядку"
            enabled: !root.flowMode
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
        width: 340
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
            width: 360
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
        MenuItem { text: "Название и цвет…"; onTriggered: windowDialog.open() }
        MenuItem { text: "Удалить окно…"; onTriggered: deleteDialog.open() }
    }

    Menu {
        id: mainMenu
        width: 260
        MenuItem { text: "Запустить все плееры"; onTriggered: root.win.startAll() }
        MenuItem { text: "Остановить все плееры"; onTriggered: root.win.stopAll() }
        MenuSeparator {}
        MenuItem { text: "О программе…"; onTriggered: aboutDialog.open() }
        MenuItem { text: "Выход\tCtrl+Q"; onTriggered: App.quit() }
    }

    // ------------------------------------------------------------------ content
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 3
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
        title: "HMI VMS " + App.version
        width: Math.min(620, root.width - 40)
        footer: DialogButtonBox {
            Button { text: "Закрыть"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        }
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 6
            Label { text: "Вывод видео:"; opacity: 0.7 }
            Label { text: App.renderBackend }
            Label { text: "Аппаратные декодеры:"; opacity: 0.7 }
            Label { text: App.hardwareDecoders; wrapMode: Text.Wrap; Layout.maximumWidth: 420 }
            Label { text: "Конфигурация:"; opacity: 0.7 }
            Label { text: App.configPath; wrapMode: Text.WrapAnywhere; Layout.maximumWidth: 420 }
            Label { text: "Экран окна:"; opacity: 0.7 }
            Label { text: root.screen ? root.screen.name + " (" + root.screen.width + "×" + root.screen.height + ")" : "" }
        }
    }
}
