import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HmiCore

// Add / edit a player (video stream).
// Sizes are derived from the font (em), so the dialog stays intact with
// enlarged system fonts; fields shrink, long texts wrap, content scrolls.
Dialog {
    id: dlg
    property VmsWindow win
    property Player player: null

    FontMetrics { id: fm; font: dlg.font }
    readonly property real em: fm.height

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(em * 36, parent ? parent.width - 24 : em * 36)
    height: Math.min(implicitHeight, parent ? parent.height - 24 : implicitHeight)
    modal: true
    focus: true
    title: player ? "Настройки плеера" : "Новый плеер"

    function openFor(p) {
        player = p
        const c = p ? p.config() : {}
        nameField.text = c.name || ""
        urlField.text = c.url || "rtsp://"
        userField.text = c.username || ""
        passField.text = c.password || ""
        transportBox.currentIndex = c.transport !== undefined ? c.transport : 0
        latencySpin.value = c.latency !== undefined ? c.latency : 200
        decoderBox.currentIndex = c.decoder !== undefined ? c.decoder : 0
        lowLatencyCheck.checked = c.lowLatency !== undefined ? c.lowLatency : true
        snapshotSpin.value = c.snapshotInterval !== undefined ? c.snapshotInterval : 1000
        webField.text = c.webUrl || ""
        rotationBox.currentIndex = c.rotation !== undefined ? c.rotation / 90 : 0
        flipHCheck.checked = !!c.flipH
        flipVCheck.checked = !!c.flipV
        scroll.ScrollBar.vertical.position = 0
        open()
        urlField.forceActiveFocus()
        urlField.selectAll()
    }

    function accept_() {
        const cfg = {
            name: nameField.text,
            url: urlField.text.trim(),
            username: userField.text,
            password: passField.text,
            transport: transportBox.currentIndex,
            latency: latencySpin.value,
            decoder: decoderBox.currentIndex,
            lowLatency: lowLatencyCheck.checked,
            snapshotInterval: snapshotSpin.value,
            webUrl: webField.text.trim(),
            rotation: rotationBox.currentIndex * 90,
            flipH: flipHCheck.checked,
            flipV: flipVCheck.checked
        }
        if (player)
            player.applyConfig(cfg)
        else
            win.addPlayer(cfg)
        close()
    }

    readonly property string url_: urlField.text.trim()
    readonly property bool urlValid: /^(rtsps?|rtsp[tuh]|https?|wss?|m4[sf]s?|test|file):\/\/.+/i.test(url_)
    readonly property bool isRtsp: /^rtsp/i.test(url_)
    readonly property bool isSnapshot: /^https?:\/\/[^?#]+\.jpe?g([?#].*)?$/i.test(url_)
    readonly property string streamHost: {
        const m = url_.match(/^[a-z0-9+.-]+:\/\/(?:[^@\/]*@)?(\[[^\]]+\]|[^:\/?#]+)/i)
        return m ? m[1] : ""
    }
    // Protocol shown under the address field.
    readonly property string protocol: {
        const u = url_.toLowerCase().split(/[?#]/)[0]
        if (/^rtsp/.test(u)) return "RTSP"
        if (/^m4s/.test(u)) return "M4S → MSE-LD"
        if (/^m4f/.test(u)) return "M4F → MPEG-TS"
        if (/^wss?:/.test(u)) return "MSE-LD (WebSocket)"
        if (isSnapshot) return "JPEG-снимки"
        if (/\.m3u8$/.test(u)) return "HLS"
        if (/\.mpd$/.test(u)) return "DASH"
        if (/\.ism/.test(u)) return "MSS"
        if (/mpegts$|\.ts$/.test(u)) return "MPEG-TS по HTTP"
        if (/^https?:/.test(u)) return "HTTP (автоопределение)"
        if (/^test:/.test(u)) return "тестовый сигнал"
        return ""
    }

    // Common sizing for the value column: grows with the dialog, never
    // dictates its width.
    component Field: TextField {
        Layout.fillWidth: true
        Layout.preferredWidth: dlg.em * 4
        Layout.minimumWidth: dlg.em * 3
    }
    component Hint: Label {
        Layout.fillWidth: true
        Layout.preferredWidth: dlg.em * 4
        wrapMode: Text.Wrap
        opacity: 0.65
        font.pixelSize: Math.max(10, Math.round(dlg.em * 0.68))
    }
    component Section: Label {
        Layout.columnSpan: 2
        Layout.topMargin: dlg.em * 0.4
        font.bold: true
        opacity: 0.85
    }

    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: form.implicitHeight

        GridLayout {
            id: form
            width: scroll.availableWidth
            columns: 2
            columnSpacing: Math.round(dlg.em * 0.7)
            rowSpacing: Math.round(dlg.em * 0.4)

            Section { text: "Поток"; Layout.topMargin: 0 }

            Label { text: "Название" }
            Field {
                id: nameField
                placeholderText: "по умолчанию — адрес камеры"
            }

            Label { text: "Адрес потока" }
            Field {
                id: urlField
                placeholderText: "rtsp://192.168.1.64:554/Streaming/Channels/101"
                onAccepted: if (dlg.urlValid) dlg.accept_()
            }

            Item { implicitWidth: 1; implicitHeight: 1 }
            Hint {
                textFormat: Text.StyledText
                text: (dlg.protocol ? "Протокол: <b>" + dlg.protocol + "</b><br>" : "")
                      + "RTSP; HLS (.m3u8), DASH (.mpd), MSS (.isml/manifest), MPEG-TS и MJPEG по HTTP; "
                      + "JPEG-снимки (.jpg); MSE-LD (ws://…/mse_ld); m4s:// и m4f:// Flussonic; test://ball."
            }

            Label { text: "Логин" }
            Field { id: userField; placeholderText: "необязательно" }

            Label { text: "Пароль" }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                Field {
                    id: passField
                    echoMode: showPass.checked ? TextInput.Normal : TextInput.Password
                }
                CheckBox {
                    id: showPass
                    text: "показать"
                    focusPolicy: Qt.NoFocus
                }
            }

            Label { text: "Транспорт RTSP"; visible: dlg.isRtsp }
            ComboBox {
                id: transportBox
                visible: dlg.isRtsp
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["Авто (UDP, при неудаче TCP)", "TCP (надёжно через NAT/VPN)", "UDP (минимальная задержка)"]
            }

            Label { text: "Буфер, мс"; visible: dlg.isRtsp }
            SpinBox {
                id: latencySpin
                visible: dlg.isRtsp
                from: 0; to: 5000; stepSize: 50
                editable: true
            }
            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.isRtsp }
            Hint {
                visible: dlg.isRtsp
                text: "Джиттер-буфер RTP: меньше — ниже задержка, больше — устойчивее к потерям пакетов."
            }

            Label { text: "Обновление снимка, мс"; visible: dlg.isSnapshot }
            SpinBox {
                id: snapshotSpin
                visible: dlg.isSnapshot
                from: 100; to: 60000; stepSize: 100
                editable: true
            }

            Label { text: "Декодер" }
            ComboBox {
                id: decoderBox
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["Авто (аппаратный, при ошибке — программный)", "Только аппаратный (VPU)", "Только программный (CPU)"]
            }

            Item { implicitWidth: 1; implicitHeight: 1 }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                CheckBox { id: lowLatencyCheck }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: "Минимальная задержка (выводить кадры сразу, без синхронизации по времени)"
                    MouseArea { anchors.fill: parent; onClicked: lowLatencyCheck.toggle() }
                }
            }

            Section { text: "Изображение" }

            Label { text: "Поворот" }
            ComboBox {
                id: rotationBox
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["0°", "90° по часовой", "180°", "270° (90° против часовой)"]
            }

            Label { text: "Отражение" }
            Flow {
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                spacing: dlg.em * 0.6
                CheckBox { id: flipHCheck; text: "по горизонтали" }
                CheckBox { id: flipVCheck; text: "по вертикали" }
            }

            Section { text: "Камера" }

            Label { text: "Веб-интерфейс" }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                Field {
                    id: webField
                    placeholderText: dlg.streamHost ? "например " + dlg.streamHost : "IP-адрес или http://…"
                }
                Button {
                    text: "Из адреса потока"
                    enabled: dlg.streamHost.length > 0
                    onClicked: webField.text = dlg.streamHost
                }
            }
            Item { implicitWidth: 1; implicitHeight: 1 }
            Hint {
                text: "Адрес страницы настроек камеры (IP или URL). Открывается из контекстного меню плеера "
                      + "пунктом «Веб-интерфейс камеры»; если адрес пуст — пункт неактивен."
            }
        }
    }

    footer: DialogButtonBox {
        Button {
            text: dlg.player ? "Сохранить" : "Добавить"
            enabled: dlg.urlValid
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: "Отмена"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        onAccepted: dlg.accept_()
        onRejected: dlg.close()
    }
}
