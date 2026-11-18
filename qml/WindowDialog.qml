import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import HmiCore

// Window title, accent colour and wallpaper.
Dialog {
    id: dlg
    property VmsWindow win
    property color chosen: "#2563eb"
    property string wallpaper: ""
    property real dimLevel: 0

    FontMetrics { id: fm; font: dlg.font }
    readonly property real em: fm.height

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(em * 34, parent ? parent.width - 24 : em * 34)
    height: Math.min(implicitHeight, parent ? parent.height - 24 : implicitHeight)
    modal: true
    title: "Окно: название, цвет и обои"

    readonly property var palette_: ["#2563eb", "#0891b2", "#0d9488", "#16a34a", "#65a30d", "#ca8a04",
                                     "#d97706", "#ea580c", "#dc2626", "#db2777", "#9333ea", "#7c3aed",
                                     "#4b5563", "#1f2937", "#78716c", "#e5e7eb"]

    // Preview the wallpaper live; restored on cancel.
    property string origWallpaper: ""
    property real origDim: 0

    onAboutToShow: {
        titleField.text = win.title
        chosen = win.color
        hexField.text = chosen.toString()
        origWallpaper = win.wallpaper
        origDim = win.wallpaperDim
        wallpaper = win.wallpaper
        dimLevel = win.wallpaperDim
        titleField.forceActiveFocus()
        titleField.selectAll()
    }
    onWallpaperChanged: if (visible) win.wallpaper = wallpaper
    onDimLevelChanged: if (visible) win.wallpaperDim = dimLevel

    function isBuiltin(url) { return url.indexOf("qrc:/wallpapers/") === 0 }
    function fileName(url) {
        const s = decodeURIComponent(url.toString())
        return s.substring(s.lastIndexOf("/") + 1)
    }

    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: col.implicitHeight

        ColumnLayout {
            id: col
            width: scroll.availableWidth
            spacing: Math.round(dlg.em * 0.5)

            Label { text: "Название"; font.bold: true }
            TextField {
                id: titleField
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                onAccepted: dlg.accept()
            }

            Label { text: "Цвет выделения"; font.bold: true }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: dlg.palette_
                    delegate: Rectangle {
                        required property var modelData
                        width: Math.round(dlg.em * 2.1); height: Math.round(dlg.em * 1.6); radius: 4
                        color: modelData
                        border.width: Qt.colorEqual(dlg.chosen, modelData) ? 3 : 1
                        border.color: Qt.colorEqual(dlg.chosen, modelData) ? "white" : "#55000000"
                        MouseArea {
                            anchors.fill: parent
                            onClicked: { dlg.chosen = parent.modelData; hexField.text = dlg.chosen.toString() }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    width: Math.round(dlg.em * 2.1); height: Math.round(dlg.em * 1.6); radius: 4
                    color: dlg.chosen
                    border.color: "#888"
                }
                TextField {
                    id: hexField
                    Layout.preferredWidth: dlg.em * 6
                    placeholderText: "#RRGGBB"
                    validator: RegularExpressionValidator { regularExpression: /#?[0-9a-fA-F]{6}/ }
                    onTextEdited: {
                        const t = text.startsWith("#") ? text : "#" + text
                        if (t.length === 7)
                            dlg.chosen = t
                    }
                }
                Button {
                    text: "Другой цвет…"
                    onClicked: {
                        colorDialog.selectedColor = dlg.chosen
                        colorDialog.open()
                    }
                }
                Item { Layout.fillWidth: true }
            }

            Label { text: "Обои"; font.bold: true; Layout.topMargin: dlg.em * 0.3 }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                // "No wallpaper" + built-in CAMAI wallpapers + current user file.
                Repeater {
                    model: [{ url: "", name: "Без обоев" }].concat(App.builtinWallpapers)
                           .concat(dlg.wallpaper && !dlg.isBuiltin(dlg.wallpaper)
                                   ? [{ url: dlg.wallpaper, name: dlg.fileName(dlg.wallpaper) }] : [])
                    delegate: Column {
                        id: thumb
                        required property var modelData
                        readonly property bool selected: dlg.wallpaper === modelData.url
                        spacing: 2
                        Rectangle {
                            width: Math.round(dlg.em * 7); height: Math.round(width * 9 / 16)
                            color: "#0e1013"
                            border.width: thumb.selected ? 3 : 1
                            border.color: thumb.selected ? dlg.chosen : "#44ffffff"
                            Image {
                                anchors.fill: parent
                                anchors.margins: parent.border.width
                                source: thumb.modelData.url
                                sourceSize.width: 240
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: thumb.modelData.url !== ""
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: dlg.wallpaper = thumb.modelData.url
                            }
                        }
                        Label {
                            width: Math.round(dlg.em * 7)
                            text: thumb.modelData.name
                            elide: Text.ElideMiddle
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: Math.max(10, Math.round(dlg.em * 0.68))
                            opacity: 0.8
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: "Выбрать файл…"
                    onClicked: fileDialog.open()
                }
                Label {
                    text: "Затемнение"
                    Layout.leftMargin: dlg.em * 0.6
                }
                Slider {
                    id: dimSlider
                    Layout.fillWidth: true
                    Layout.preferredWidth: dlg.em * 4
                    from: 0; to: 0.9; stepSize: 0.05
                    value: dlg.dimLevel
                    onMoved: dlg.dimLevel = value
                    enabled: dlg.wallpaper.length > 0
                }
                Label {
                    text: Math.round(dlg.dimLevel * 100) + "%"
                    Layout.preferredWidth: dlg.em * 2.2
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                wrapMode: Text.Wrap
                opacity: 0.65
                font.pixelSize: Math.max(10, Math.round(dlg.em * 0.68))
                text: "Для своих изображений рекомендуется затемнение 50–70%, чтобы обои не отвлекали от видео."
            }
        }
    }

    ColorDialog {
        id: colorDialog
        title: "Цвет окна"
        onAccepted: {
            dlg.chosen = selectedColor
            hexField.text = dlg.chosen.toString()
        }
    }

    FileDialog {
        id: fileDialog
        title: "Изображение для обоев"
        nameFilters: ["Изображения (*.jpg *.jpeg *.png *.webp *.bmp)", "Все файлы (*)"]
        onAccepted: {
            dlg.wallpaper = selectedFile.toString()
            if (dlg.dimLevel < 0.5)
                dlg.dimLevel = 0.6     // user images are usually too bright
        }
    }

    footer: DialogButtonBox {
        Button { text: "Применить"; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        Button { text: "Отмена"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: {
        win.title = titleField.text
        win.color = dlg.chosen
        win.wallpaper = dlg.wallpaper
        win.wallpaperDim = dlg.dimLevel
    }
    onRejected: {
        win.wallpaper = origWallpaper
        win.wallpaperDim = origDim
    }
}
