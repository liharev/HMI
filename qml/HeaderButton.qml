import QtQuick
import QtQuick.Controls

// Flat tool-bar button that stays readable on any accent colour.
AbstractButton {
    id: control
    property color fg: "white"
    property string tip: ""
    implicitWidth: label.implicitWidth + 18
    implicitHeight: Math.max(30, label.implicitHeight + 10)
    font.bold: checked
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    contentItem: Text {
        id: label
        text: control.text
        color: control.fg
        font: control.font
        // bold when checked is set via font below
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: 4
        color: control.pressed || control.checked ? Qt.rgba(0, 0, 0, 0.28)
             : control.hovered ? Qt.rgba(1, 1, 1, 0.18) : "transparent"
        border.width: control.checked ? 1 : 0
        border.color: Qt.rgba(control.fg.r, control.fg.g, control.fg.b, 0.5)
    }
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.text: tip
    ToolTip.delay: 600
}
