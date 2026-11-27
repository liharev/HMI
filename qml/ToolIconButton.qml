import QtQuick
import QtQuick.Controls

// Compact flat button used on the player overlay: a drawn icon (independent
// of the fonts) or a short text such as "1:1".
AbstractButton {
    id: control
    property string tip: ""
    property color fg: "white"
    property string iconName: ""
    implicitWidth: iconName.length > 0 ? 26 : Math.max(26, label.implicitWidth + 10)
    implicitHeight: 24
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            visible: control.iconName.length > 0
            name: control.iconName
            width: 14; height: 14
            color: control.enabled ? control.fg : "#777"
        }
        Text {
            id: label
            anchors.fill: parent
            visible: control.iconName.length === 0
            text: control.text
            color: control.enabled ? control.fg : "#777"
            font.pixelSize: 14
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
    background: Rectangle {
        radius: 3
        color: control.pressed ? "#60ffffff" : control.hovered ? "#35ffffff" : "transparent"
    }
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.text: tip
    ToolTip.delay: 600
}
