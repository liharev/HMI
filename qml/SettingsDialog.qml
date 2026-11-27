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

    onAboutToShow: {
        waitBox.value = Math.round(App.streamWaitMs / 100)
        autoCheck.checked = App.autoQuality
        fpsBox.value = Math.round(App.minTileFps)
        smoothCheck.checked = App.smoothPlayback
        bufferBox.value = App.smoothBufferMs
        App.refreshAudioDevices()
        audioBox.currentIndex = Math.max(0, audioBox.model.indexOf(App.audioDevice))
    }

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
            Label {
                text: "Качество видео"
                font.bold: true
                Layout.topMargin: Math.round(dlg.em * 0.5)
            }
            CheckBox {
                id: autoCheck
                text: "Выбирать поток камеры по размеру плеера"
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#9ca3af"
                font.pixelSize: Math.round(dlg.em * 0.8)
                text: "Для камер с несколькими потоками (CAMAI, ONVIF, Flussonic, список в настройках плеера): "
                      + "маленький плеер получает лёгкий поток, большой — подробный. Это снижает нагрузку "
                      + "на декодер, процессор и сеть."
            }
            RowLayout {
                enabled: autoCheck.checked
                spacing: Math.round(dlg.em * 0.5)
                Label { text: "Минимальная частота кадров" }
                SpinBox {
                    id: fpsBox
                    from: 1
                    to: 30
                    editable: true
                    Layout.preferredWidth: dlg.em * 6
                }
                Label { text: "к/с" }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#9ca3af"
                font.pixelSize: Math.round(dlg.em * 0.8)
                text: "Потоки с меньшей частотой выбираются, только если других нет. По умолчанию — 10 к/с "
                      + "(например, у CAMAI превью 360p идёт с 5 к/с, 480p — с 10 к/с)."
            }
            Label {
                text: "Плавность"
                font.bold: true
                Layout.topMargin: Math.round(dlg.em * 0.5)
            }
            CheckBox {
                id: smoothCheck
                text: "Плавное воспроизведение (кадры по их времени)"
            }
            RowLayout {
                enabled: smoothCheck.checked
                spacing: Math.round(dlg.em * 0.5)
                Label { text: "Буфер" }
                SpinBox {
                    id: bufferBox
                    from: 100
                    to: 3000
                    stepSize: 100
                    editable: true
                    Layout.preferredWidth: dlg.em * 7
                }
                Label { text: "мс" }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#9ca3af"
                font.pixelSize: Math.round(dlg.em * 0.8)
                text: "Видео показывается с задержкой на размер буфера, зато неравномерная доставка по сети и "
                      + "рывки декодера не видны: объекты движутся равномерно. Больше буфер — плавнее при плохой "
                      + "сети, но больше задержка. По умолчанию — включено, 500 мс. У плеера можно выбрать иначе."
            }
            Label {
                text: "Звук"
                font.bold: true
                Layout.topMargin: Math.round(dlg.em * 0.5)
            }
            RowLayout {
                spacing: Math.round(dlg.em * 0.5)
                Layout.fillWidth: true
                Label { text: "Устройство вывода" }
                ComboBox {
                    id: audioBox
                    Layout.fillWidth: true
                    model: ["Системное по умолчанию"].concat(App.audioDevices)
                }
                Button {
                    text: "Обновить"
                    onClicked: {
                        const cur = audioBox.currentText
                        App.refreshAudioDevices()
                        audioBox.currentIndex = Math.max(0, audioBox.model.indexOf(cur))
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Обновить список устройств"
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#9ca3af"
                font.pixelSize: Math.round(dlg.em * 0.8)
                text: "Звук включается значком динамика на плеере (по умолчанию выключен, одновременно — только у одного плеера). "
                      + "Если выбранное устройство пропадёт, звук пойдёт на системное."
            }
            Button {
                text: "По умолчанию"
                onClicked: {
                    waitBox.value = 30
                    autoCheck.checked = true
                    fpsBox.value = 10
                    audioBox.currentIndex = 0
                    smoothCheck.checked = true
                    bufferBox.value = 500
                }
            }
        }
    }

    footer: DialogButtonBox {
        Button { text: "Применить"; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        Button { text: "Отмена"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: {
        App.streamWaitMs = waitBox.value * 100
        App.autoQuality = autoCheck.checked
        App.minTileFps = fpsBox.value
        App.audioDevice = audioBox.currentIndex > 0 ? audioBox.currentText : ""
        App.smoothBufferMs = bufferBox.value
        App.smoothPlayback = smoothCheck.checked
    }
}
