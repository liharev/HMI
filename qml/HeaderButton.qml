import QtQuick
import QtQuick.Controls

// Flat tool-bar button that stays readable on any accent colour.
// iconName: drawn icon before the text; menu: drop-down arrow after it.
AbstractButton {
    id: control
    property color fg: "white"
    property string tip: ""
    property string iconName: ""
    property bool menu: false
    padding: 9
    topPadding: 0
    bottomPadding: 0
    implicitWidth: row.implicitWidth + leftPadding + rightPadding
    implicitHeight: Math.max(30, label.implicitHeight + 10)
    font.bold: checked
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    readonly property real iconSize: Math.round(label.font.pixelSize > 0 ? label.font.pixelSize * 1.05
                                                                        : fm.height * 0.8)
    FontMetrics { id: fm; font: control.font }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: Math.round(control.iconSize * 0.35)
            Icon {
                visible: control.iconName.length > 0
                name: control.iconName
                color: control.fg
                width: control.iconSize; height: width
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: label
                visible: text.length > 0
                text: control.text
                color: control.fg
                font: control.font
                verticalAlignment: Text.AlignVCenter
                anchors.verticalCenter: parent.verticalCenter
            }
            Icon {
                visible: control.menu
                name: "chevron"
                color: control.fg
                width: Math.round(control.iconSize * 0.8); height: width
                anchors.verticalCenter: parent.verticalCenter
            }
        }
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
