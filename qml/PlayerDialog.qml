import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HmiCore

// Add / edit a player (video stream).
Dialog {
    id: dlg
    property VmsWindow win
    property Player player: null

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(620, parent ? parent.width - 40 : 620)
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
            snapshotInterval: snapshotSpin.value
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
    // Protocol shown next to the address field.
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

    contentItem: GridLayout {
        columns: 2
        columnSpacing: 12
        rowSpacing: 8

        Label { text: "Название" }
        TextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: "например, «Въезд, камера 1» (по умолчанию — адрес камеры)"
        }

        Label { text: "Адрес потока" }
        TextField {
            id: urlField
            Layout.fillWidth: true
            placeholderText: "rtsp://192.168.1.64:554/Streaming/Channels/101"
            onAccepted: if (dlg.urlValid) dlg.accept_()
        }

        Item { width: 1; height: 1 }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.65
            font.pixelSize: 11
            text: (dlg.protocol ? "Протокол: <b>" + dlg.protocol + "</b><br>" : "")
                  + "RTSP; HLS (.m3u8), DASH (.mpd), MSS (.isml/manifest), MPEG-TS и MJPEG по HTTP; "
                  + "JPEG-снимки (.jpg); MSE-LD (ws://…/mse_ld); m4s:// и m4f:// Flussonic; test://ball."
            textFormat: Text.StyledText
        }

        Label { text: "Логин" }
        TextField { id: userField; Layout.fillWidth: true; placeholderText: "необязательно" }

        Label { text: "Пароль" }
        TextField {
            id: passField
            Layout.fillWidth: true
            echoMode: showPass.checked ? TextInput.Normal : TextInput.Password
            rightPadding: showPass.width + 4
            CheckBox {
                id: showPass
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "👁"
                focusPolicy: Qt.NoFocus
            }
        }

        Label { text: "Транспорт RTSP"; visible: dlg.isRtsp }
        ComboBox {
            id: transportBox
            visible: dlg.isRtsp
            Layout.fillWidth: true
            model: ["Авто (UDP, при неудаче TCP)", "TCP (надёжно через NAT/VPN)", "UDP (минимальная задержка)"]
        }

        Label { text: "Обновление снимка, мс"; visible: dlg.isSnapshot }
        SpinBox {
            id: snapshotSpin
            visible: dlg.isSnapshot
            from: 100; to: 60000; stepSize: 100
            editable: true
        }

        Label { text: "Буфер, мс"; visible: dlg.isRtsp }
        RowLayout {
            visible: dlg.isRtsp
            SpinBox {
                id: latencySpin
                from: 0; to: 5000; stepSize: 50
                editable: true
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.65
                font.pixelSize: 11
                text: "Джиттер-буфер RTP. Меньше — ниже задержка, больше — устойчивее к потерям."
            }
        }

        Label { text: "Декодер" }
        ComboBox {
            id: decoderBox
            Layout.fillWidth: true
            model: ["Авто (аппаратный, при ошибке — программный)", "Только аппаратный (VPU)", "Только программный (CPU)"]
        }

        Item { width: 1; height: 1 }
        CheckBox {
            id: lowLatencyCheck
            text: "Минимальная задержка (выводить кадры сразу, без синхронизации по времени)"
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
