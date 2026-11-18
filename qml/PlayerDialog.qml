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
            lowLatency: lowLatencyCheck.checked
        }
        if (player)
            player.applyConfig(cfg)
        else
            win.addPlayer(cfg)
        close()
    }

    readonly property bool urlValid: /^(rtsps?|rtsp[tuh]|https?|test|file):\/\/.+/i.test(urlField.text.trim())

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
            text: "RTSP: H.264, H.265, MJPEG. Также http(s):// — MJPEG по HTTP, test://ball — тестовый сигнал."
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

        Label { text: "Транспорт RTSP" }
        ComboBox {
            id: transportBox
            Layout.fillWidth: true
            model: ["Авто (UDP, при неудаче TCP)", "TCP (надёжно через NAT/VPN)", "UDP (минимальная задержка)"]
        }

        Label { text: "Буфер, мс" }
        RowLayout {
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
