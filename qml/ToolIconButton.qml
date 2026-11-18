import QtQuick
import QtQuick.Controls

// Compact flat button used on the player overlay.
AbstractButton {
    id: control
    property string tip: ""
    property color fg: "white"
    implicitWidth: Math.max(26, label.implicitWidth + 10)
    implicitHeight: 24
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    contentItem: Text {
        id: label
        text: control.text
        color: control.enabled ? control.fg : "#777"
        font.pixelSize: 14
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: 3
        color: control.pressed ? "#60ffffff" : control.hovered ? "#35ffffff" : "transparent"
    }
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.text: tip
    ToolTip.delay: 600
}
