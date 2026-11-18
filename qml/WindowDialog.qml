import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import HmiCore

// Window title and accent colour.
Dialog {
    id: dlg
    property VmsWindow win
    property color chosen: "#2563eb"

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(460, parent ? parent.width - 40 : 460)
    modal: true
    title: "Окно: название и цвет"

    readonly property var palette_: ["#2563eb", "#0891b2", "#0d9488", "#16a34a", "#65a30d", "#ca8a04",
                                     "#d97706", "#ea580c", "#dc2626", "#db2777", "#9333ea", "#7c3aed",
                                     "#4b5563", "#1f2937", "#78716c", "#e5e7eb"]

    onAboutToShow: {
        titleField.text = win.title
        chosen = win.color
        hexField.text = chosen.toString()
        titleField.forceActiveFocus()
        titleField.selectAll()
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label { text: "Название" }
        TextField {
            id: titleField
            Layout.fillWidth: true
            onAccepted: dlg.accept()
        }
        Label { text: "Цвет выделения" }
        GridLayout {
            columns: 8
            rowSpacing: 6
            columnSpacing: 6
            Repeater {
                model: dlg.palette_
                delegate: Rectangle {
                    required property var modelData
                    width: 36; height: 28; radius: 4
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
            Rectangle {
                width: 36; height: 28; radius: 4
                color: dlg.chosen
                border.color: "#888"
            }
            TextField {
                id: hexField
                Layout.preferredWidth: 110
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

    footer: DialogButtonBox {
        Button { text: "Применить"; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        Button { text: "Отмена"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: {
        win.title = titleField.text
        win.color = dlg.chosen
    }
}
