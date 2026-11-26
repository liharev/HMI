import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HmiCore

// Application settings (common for all windows and players).
Dialog {
    id: dlg

    FontMetrics { id: fm; font: dlg.font }
    readonly property real em: fm.height

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(em * 32, parent ? parent.width - 24 : em * 32)
    height: Math.min(implicitHeight, parent ? parent.height - 24 : implicitHeight)
    modal: true
    title: "Настройки программы"

    onAboutToShow: waitBox.value = Math.round(App.streamWaitMs / 100)

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        implicitHeight: col.implicitHeight

        ColumnLayout {
            id: col
            width: parent.width
            spacing: Math.round(dlg.em * 0.5)

            Label {
                text: "Ожидание видеопотока"
                font.bold: true
            }
            RowLayout {
                spacing: Math.round(dlg.em * 0.5)
                SpinBox {
                    id: waitBox
                    // Tenths of a second: 0.5 .. 30 s.
                    from: 5
                    to: 300
                    stepSize: 5
                    editable: true
                    Layout.preferredWidth: dlg.em * 8
                    textFromValue: (v, locale) => Number(v / 10).toLocaleString(locale, 'f', 1)
                    valueFromText: (t, locale) => Math.round(Number.fromLocaleString(locale, t) * 10)
                    validator: DoubleValidator { bottom: 0.5; top: 30; decimals: 1 }
                }
                Label { text: "с" }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#9ca3af"
                font.pixelSize: Math.round(dlg.em * 0.8)
                text: "При переключении потока или переподключении на экране остаётся последний кадр. "
                      + "Если за это время новый кадр не пришёл, поверх плеера показывается состояние "
                      + "обмена с камерой. По умолчанию — 3 с."
            }
            Button {
                text: "По умолчанию"
                onClicked: waitBox.value = 30
            }
        }
    }

    footer: DialogButtonBox {
        Button { text: "Применить"; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        Button { text: "Отмена"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: App.streamWaitMs = waitBox.value * 100
}
