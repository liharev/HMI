import QtQuick
import QtQuick.Controls

// Yes / No confirmation with Russian button captions.
Dialog {
    id: dlg
    property string text: ""
    property string acceptText: "Удалить"

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(460, parent ? parent.width - 40 : 460)
    modal: true

    contentItem: Label {
        text: dlg.text
        wrapMode: Text.Wrap
    }
    footer: DialogButtonBox {
        Button { text: dlg.acceptText; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        Button { text: "Отмена"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
}
