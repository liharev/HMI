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

    // Streams of the camera (profiles) being edited: list of maps.
    property var profs: []
    property string pin: ""
    property var probing: ({})           // probe id -> url
    property int probesRunning: 0
    // Source: 0 - stream address, 1 - CAMAI device (channels from its catalogue).
    property int mode: 0
    property var camaiList: []
    property var camaiChecked: ({})
    property string camaiError: ""
    property bool camaiBusy: false
    property int camaiReq: -1
    property int discoverReq: -1
    property string discoverMsg: ""

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
        playModeBox.currentIndex = c.playMode !== undefined ? c.playMode : 0
        snapshotSpin.value = c.snapshotInterval !== undefined ? c.snapshotInterval : 1000
        webField.text = c.webUrl || ""
        rotationBox.currentIndex = c.rotation !== undefined ? c.rotation / 90 : 0
        flipHCheck.checked = !!c.flipH
        flipVCheck.checked = !!c.flipV
        outputBox.currentIndex = c.output !== undefined ? c.output : 0
        profs = (c.profiles || []).map(x => Object.assign({}, x))
        pin = c.profilePin || ""
        probing = ({})
        probesRunning = 0
        newProfileField.text = ""
        onvifField.text = ""
        discoverMsg = ""
        discoverReq = -1
        mode = c.camaiService ? 1 : 0
        camaiField.text = c.camaiService || ""
        camaiHostField.text = c.camaiRtspHost || ""
        camaiList = []
        camaiError = ""
        camaiBusy = false
        const chk = {}
        if (c.camaiService)
            chk[c.camaiChannel] = true
        camaiChecked = chk
        if (mode === 1)
            loadCamai()
        scroll.ScrollBar.vertical.position = 0
        open()
        urlField.forceActiveFocus()
        urlField.selectAll()
    }

    function camaiSelected() { return camaiList.filter(ch => !!camaiChecked[ch.id]) }
    function loadCamai() {
        if (!camaiField.text.trim().length)
            return
        camaiBusy = true
        camaiError = ""
        camaiReq = App.camaiChannels(camaiField.text, userField.text, passField.text)
    }
    function toggleChannel(id, on) {
        // Editing a player: one channel; adding: any number (one player each).
        const chk = player ? {} : Object.assign({}, camaiChecked)
        chk[id] = on
        camaiChecked = chk
    }
    function channelSummary(ch) {
        const streams = ch.profiles.filter(p => p.kind !== "preview").map(p => p.width + "×" + p.height
                                                                            + (p.codec ? " " + p.codec.toUpperCase() : ""))
        const previews = ch.profiles.filter(p => p.kind === "preview").map(p => p.label || (p.height + "p"))
        let t = (ch.signal ? "" : (ch.splash ? "нет сигнала (заставка) · " : "нет сигнала · "))
        t += "потоки: " + (streams.length ? streams.join(", ") : "—")
        if (previews.length)
            t += " · превью: " + previews.join(", ")
        return t
    }
    Connections {
        target: App
        function onCamaiChannelsReady(id, list, error) {
            if (id !== dlg.camaiReq)
                return
            dlg.camaiBusy = false
            dlg.camaiError = error
            dlg.camaiList = list
            if (!dlg.player && list.length && Object.keys(dlg.camaiChecked).length === 0) {
                const chk = {}
                for (const ch of list)
                    chk[ch.id] = true          // adding: all channels by default
                dlg.camaiChecked = chk
            }
        }
    }

    function acceptCamai(base) {
        const svc = App.camaiNormalize(camaiField.text)
        const host = svc.replace(/^[a-z]+:\/\//i, "").replace(/:\d+$/, "")
        const sel = camaiSelected()
        if (player && sel.length === 0) {
            // Catalogue not loaded: keep the channel, save the other settings.
            player.applyConfig(base)
            return
        }
        for (const ch of sel) {
            const profs = ch.profiles
            const main = profs.find(x => x.kind === "stream") || profs[0]
            const own = sel.length === 1 && nameField.text.trim().length > 0
            const cfg = Object.assign({}, base, {
                url: main ? main.url : "",
                name: own ? nameField.text.trim() : "CAMAI " + host + " · канал " + ch.id,
                camaiService: svc,
                camaiChannel: ch.id,
                camaiRtspHost: camaiHostField.text.trim(),
                profiles: profs,
                profilePin: player && player.camaiChannel === ch.id ? base.profilePin : ""
            })
            if (player)
                player.applyConfig(cfg)
            else
                win.addPlayer(cfg)
        }
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
            playMode: playModeBox.currentIndex,
            snapshotInterval: snapshotSpin.value,
            webUrl: webField.text.trim(),
            rotation: rotationBox.currentIndex * 90,
            flipH: flipHCheck.checked,
            flipV: flipVCheck.checked,
            output: outputBox.currentIndex,
            profiles: profs,
            profilePin: pinBox.currentIndex > 0 ? profs[pinBox.currentIndex - 1].url : ""
        }
        if (mode === 1) {
            delete cfg.url
            delete cfg.profiles
            acceptCamai(cfg)
            close()
            return
        }
        cfg.camaiService = ""
        cfg.camaiChannel = -1
        if (player)
            player.applyConfig(cfg)
        else
            win.addPlayer(cfg)
        close()
    }
    readonly property bool canAccept: mode === 0 ? urlValid
                                                 : (camaiSelected().length > 0 || (!!player && camaiField.text.trim().length > 0))

    function profileIndex(u) {
        for (let i = 0; i < profs.length; ++i)
            if (profs[i].url === u)
                return i
        return -1
    }
    // What the player already knows about the stream it plays.
    function learned(u) {
        const p = player
        if (!p || p.streamUrl !== u || p.videoWidth <= 0)
            return { url: u }
        const codec = p.codec === "H.264" ? "h264" : p.codec === "H.265" ? "h265" : p.codec === "MJPEG" ? "jpeg" : ""
        return { url: u, width: p.videoWidth, height: p.videoHeight, fps: p.nominalFps, codec: codec,
                 description: p.videoWidth + "×" + p.videoHeight + (p.nominalFps > 0 ? " · " + Math.round(p.nominalFps) + " к/с" : "")
                              + (p.codec ? " · " + p.codec : "") }
    }
    function addProfile(u) {
        u = u.trim()
        if (!u.length)
            return
        const list = profs.slice()
        // The main address is one of the streams too.
        if (list.length === 0 && url_.length && url_ !== u)
            list.push(Object.assign({ label: "основной" }, learned(url_)))
        if (profileIndex(u) < 0 && !list.some(x => x.url === u))
            list.push(learned(u))
        profs = list
        probeAll()
    }
    function removeProfile(i) {
        const list = profs.slice()
        if (pinBox.currentIndex - 1 === i)
            pin = ""
        list.splice(i, 1)
        profs = list
    }
    function probeAll() {
        for (let i = 0; i < profs.length; ++i) {
            if (profs[i].width > 0 && profs[i].codec)
                continue
            const id = App.probeStream(profs[i].url, userField.text, passField.text)
            const m = Object.assign({}, probing)
            m[id] = profs[i].url
            probing = m
            ++probesRunning
        }
    }
    // Streams offered by the camera (ONVIF) or the Flussonic server.
    function discover(kind) {
        discoverMsg = kind === "onvif" ? "Запрос профилей ONVIF…" : "Запрос media_info.json…"
        const addr = kind === "onvif" ? (onvifField.text.trim() || streamHost) : url_
        discoverReq = App.discoverStreams(kind, addr, userField.text, passField.text)
    }
    Connections {
        target: App
        function onDiscoverFinished(id, list, error) {
            if (id !== dlg.discoverReq)
                return
            dlg.discoverReq = -1
            if (error) {
                dlg.discoverMsg = "Ошибка: " + error
                return
            }
            const merged = dlg.profs.slice()
            const mainKnown = list.some(x => x.url === dlg.url_)
            if (merged.length === 0 && dlg.urlValid && !mainKnown && !/\.m3u8|mpegts|\.mpd/i.test(dlg.url_))
                merged.push(Object.assign({ label: "основной" }, dlg.learned(dlg.url_)))
            let added = 0
            for (const p of list) {
                if (merged.some(x => x.url === p.url))
                    continue
                merged.push(p)
                ++added
            }
            dlg.profs = merged
            if (!dlg.urlValid && list.length)
                urlField.text = list[0].url
            dlg.discoverMsg = "Найдено потоков: " + list.length + (added < list.length ? " (новых " + added + ")" : "")
        }
    }
    function forceProbeAll() {
        const list = profs.map(x => ({ url: x.url, label: x.label || "", kind: x.kind || "", source: x.source || "" }))
        profs = list
        probeAll()
    }
    Connections {
        target: App
        function onProbeFinished(id, profile, error) {
            const u = dlg.probing[id]
            if (u === undefined)
                return
            const m = Object.assign({}, dlg.probing)
            delete m[id]
            dlg.probing = m
            dlg.probesRunning = Math.max(0, dlg.probesRunning - 1)
            const i = dlg.profileIndex(u)
            if (i < 0)
                return
            const list = dlg.profs.slice()
            const old = list[i]
            list[i] = error ? Object.assign({}, old, { error: error })
                            : Object.assign({}, profile, { label: old.label || "", kind: old.kind || "",
                                                           source: old.source || "probe" })
            dlg.profs = list
        }
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

            Section { text: "Источник"; Layout.topMargin: 0 }
            TabBar {
                id: modeBar
                Layout.columnSpan: 2
                Layout.fillWidth: true
                currentIndex: dlg.mode
                onCurrentIndexChanged: {
                    if (currentIndex === dlg.mode)
                        return
                    dlg.mode = currentIndex
                    if (dlg.mode === 1 && !dlg.player)
                        transportBox.currentIndex = 1      // CAMAI: RTSP over TCP
                }
                TabButton { text: "Адрес потока" }
                TabButton { text: "Устройство CAMAI" }
            }

            Section { text: "Поток" }

            Label { text: "Название" }
            Field {
                id: nameField
                placeholderText: "по умолчанию — адрес камеры"
            }

            Label { text: "Адрес потока"; visible: dlg.mode === 0 }
            Field {
                id: urlField
                visible: dlg.mode === 0
                placeholderText: "rtsp://192.168.1.64:554/Streaming/Channels/101"
                onAccepted: if (dlg.urlValid) dlg.accept_()
            }

            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.mode === 0 }
            Hint {
                visible: dlg.mode === 0
                textFormat: Text.StyledText
                text: (dlg.protocol ? "Протокол: <b>" + dlg.protocol + "</b><br>" : "")
                      + "RTSP; HLS (.m3u8), DASH (.mpd), MSS (.isml/manifest), MPEG-TS и MJPEG по HTTP; "
                      + "JPEG-снимки (.jpg); MSE-LD (ws://…/mse_ld); m4s:// и m4f:// Flussonic; test://ball."
            }

            // ---- CAMAI device
            Label { text: "Адрес устройства"; visible: dlg.mode === 1 }
            RowLayout {
                visible: dlg.mode === 1
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                Field {
                    id: camaiField
                    placeholderText: "192.168.53.168 или http://192.168.53.168:8080"
                    onAccepted: dlg.loadCamai()
                }
                Button {
                    text: dlg.camaiBusy ? "Запрос…" : "Получить каналы"
                    enabled: !dlg.camaiBusy && camaiField.text.trim().length > 0
                    onClicked: dlg.loadCamai()
                }
            }
            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.mode === 1 }
            Hint {
                visible: dlg.mode === 1
                color: dlg.camaiError ? "#fca5a5" : palette.windowText
                text: dlg.camaiError ? "Ошибка: " + dlg.camaiError
                      : dlg.camaiList.length ? (dlg.player ? "Выберите канал этого плеера."
                                                           : "Отметьте каналы: для каждого будет добавлен плеер. "
                                                             + "Поток подбирается автоматически по размеру плеера.")
                      : "Сервис превью CAMAI (порт 8080). Логин и пароль ниже — если на устройстве включена авторизация."
            }
            Repeater {
                model: dlg.mode === 1 ? dlg.camaiList : []
                delegate: CheckBox {
                    required property var modelData
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    // The text keeps the indicator on the left (Fusion centres it otherwise).
                    text: "Канал " + modelData.id
                    checked: !!dlg.camaiChecked[modelData.id]
                    onToggled: dlg.toggleChannel(modelData.id, checked)
                    contentItem: Column {
                        leftPadding: parent.indicator.width + parent.spacing
                        Label { text: "Канал " + modelData.id; font.bold: true }
                        Label {
                            width: dlg.availableWidth - dlg.em * 3
                            wrapMode: Text.Wrap
                            opacity: 0.7
                            font.pixelSize: Math.max(10, Math.round(dlg.em * 0.68))
                            color: modelData.signal ? palette.windowText : "#fbbf24"
                            text: dlg.channelSummary(modelData)
                        }
                    }
                }
            }
            Label { text: "RTSP через прокси"; visible: dlg.mode === 1 }
            Field {
                id: camaiHostField
                visible: dlg.mode === 1
                placeholderText: "адрес:порт — если устройство видно под другим адресом"
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

            Label { text: "Транспорт RTSP"; visible: dlg.isRtsp || dlg.mode === 1 }
            ComboBox {
                id: transportBox
                visible: dlg.isRtsp || dlg.mode === 1
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["Авто (UDP, при неудаче TCP)", "TCP (надёжно через NAT/VPN)", "UDP (минимальная задержка)"]
            }

            Label { text: "Буфер, мс"; visible: dlg.isRtsp || dlg.mode === 1 }
            SpinBox {
                id: latencySpin
                visible: dlg.isRtsp || dlg.mode === 1
                from: 0; to: 5000; stepSize: 50
                editable: true
            }
            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.isRtsp || dlg.mode === 1 }
            Hint {
                visible: dlg.isRtsp || dlg.mode === 1
                text: "Джиттер-буфер RTP: меньше — ниже задержка, больше — устойчивее к потерям пакетов."
            }

            Label { text: "Обновление снимка, мс"; visible: dlg.isSnapshot && dlg.mode === 0 }
            SpinBox {
                id: snapshotSpin
                visible: dlg.isSnapshot && dlg.mode === 0
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

            Label { text: "Воспроизведение" }
            ComboBox {
                id: playModeBox
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["Как в настройках программы (" + (App.smoothPlayback ? "плавно" : "минимальная задержка") + ")",
                         "Минимальная задержка", "Плавно"]
            }
            Item { implicitWidth: 1; implicitHeight: 1 }
            Hint {
                text: "Плавно — кадры показываются по их времени после буфера (" + App.smoothBufferMs
                      + " мс), движение равномерное. Минимальная задержка — кадр выводится сразу после "
                      + "декодирования; при неравномерной доставке по сети движение рывками."
            }

            Label { text: "Вывод видео" }
            ComboBox {
                id: outputBox
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["Авто (подбирается по скорости)", "GPU без копирования (zero-copy)",
                        "GPU с копированием кадра", "Qt Multimedia"]
            }
            Item { implicitWidth: 1; implicitHeight: 1 }
            Hint {
                text: (dlg.player ? "Сейчас: " + dlg.player.renderPath + ". " : "")
                      + "В режиме «Авто» плеер сам переходит на другой способ, если кадры не успевают "
                      + "попадать на экран, и запоминает его."
            }

            Section { text: "Качество: потоки камеры"; visible: dlg.mode === 0 || !!dlg.player }

            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.mode === 0 }
            Hint {
                visible: dlg.mode === 0
                text: "Если камера отдаёт несколько потоков (например, 4K, 1080p и 360p), перечислите их: "
                      + "плеер сам выберет самый лёгкий поток, которого достаточно для его размера на экране, "
                      + "и переключится без чёрного экрана при изменении размера. Список CAMAI, ONVIF и "
                      + "Flussonic заполняется автоматически."
            }

            Repeater {
                model: dlg.mode === 0 || dlg.player ? dlg.profs : []
                delegate: RowLayout {
                    required property var modelData
                    required property int index
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    spacing: Math.round(dlg.em * 0.4)
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            text: (modelData.label ? modelData.label + ": " : "") + modelData.url
                            font.bold: !!(dlg.player && dlg.player.streamUrl === modelData.url)
                        }
                        Hint {
                            Layout.fillWidth: true
                            color: modelData.error ? "#fca5a5" : palette.windowText
                            text: {
                                const probingNow = Object.values(dlg.probing).indexOf(modelData.url) >= 0
                                if (probingNow) return "определение параметров…"
                                if (modelData.error) return "ошибка: " + modelData.error
                                let t = modelData.description || "параметры неизвестны — нажмите «Определить параметры»"
                                if (modelData.kind === "preview") t += " · превью по запросу"
                                if (dlg.player && dlg.player.streamUrl === modelData.url) t += " · воспроизводится"
                                return t
                            }
                        }
                    }
                    ToolButton {
                        text: "✕"
                        onClicked: dlg.removeProfile(index)
                        ToolTip.visible: hovered
                        ToolTip.text: "Убрать поток из списка"
                    }
                }
            }

            Label { text: "Добавить поток"; visible: dlg.mode === 0 }
            RowLayout {
                visible: dlg.mode === 0
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                Field {
                    id: newProfileField
                    placeholderText: "адрес другого потока камеры"
                    onAccepted: { dlg.addProfile(text); text = "" }
                }
                Button {
                    text: "Добавить"
                    enabled: newProfileField.text.trim().length > 0
                    onClicked: { dlg.addProfile(newProfileField.text); newProfileField.text = "" }
                }
            }

            Label { text: "Найти потоки"; visible: dlg.mode === 0 }
            RowLayout {
                visible: dlg.mode === 0
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                Field {
                    id: onvifField
                    placeholderText: "адрес ONVIF" + (dlg.streamHost ? " (" + dlg.streamHost + ")" : "")
                }
                Button {
                    text: "ONVIF"
                    enabled: dlg.discoverReq < 0 && (onvifField.text.trim().length > 0 || dlg.streamHost.length > 0)
                    onClicked: dlg.discover("onvif")
                    ToolTip.visible: hovered
                    ToolTip.text: "Профили камеры по ONVIF (GetProfiles / GetStreamUri) с логином и паролем выше"
                }
                Button {
                    text: "Flussonic"
                    enabled: dlg.discoverReq < 0 && /^(https?|m4[sf]s?|wss?|rtsp):/i.test(dlg.url_)
                    onClicked: dlg.discover("flussonic")
                    ToolTip.visible: hovered
                    ToolTip.text: "Дорожки разного качества потока Flussonic (media_info.json; нужен транскодер)"
                }
            }
            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.mode === 0 && dlg.discoverMsg.length > 0 }
            Hint {
                visible: dlg.mode === 0 && dlg.discoverMsg.length > 0
                color: dlg.discoverMsg.indexOf("Ошибка") === 0 ? "#fca5a5" : palette.windowText
                text: dlg.discoverMsg
            }

            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.profs.length > 0 && dlg.mode === 0 }
            Button {
                visible: dlg.profs.length > 0 && dlg.mode === 0
                text: dlg.probesRunning > 0 ? "Определение… (" + dlg.probesRunning + ")" : "Определить параметры"
                enabled: dlg.probesRunning === 0
                onClicked: dlg.forceProbeAll()
            }

            Label { text: "Выбор потока"; visible: dlg.profs.length > 0 }
            ComboBox {
                id: pinBox
                visible: dlg.profs.length > 0
                Layout.fillWidth: true
                Layout.preferredWidth: dlg.em * 4
                model: ["Автоматически по размеру плеера"].concat(
                           dlg.profs.map(x => (x.label ? x.label + ": " : "") + (x.description || x.url)))
                currentIndex: Math.max(0, dlg.profileIndex(dlg.pin) + 1)
                onActivated: (i) => dlg.pin = i > 0 ? dlg.profs[i - 1].url : ""
            }
            Item { implicitWidth: 1; implicitHeight: 1; visible: dlg.profs.length > 0 && dlg.player }
            Hint {
                visible: dlg.profs.length > 0 && !!dlg.player
                text: dlg.player ? (dlg.player.profileText || "") : ""
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
            text: dlg.player ? "Сохранить"
                  : dlg.mode === 1 && dlg.camaiSelected().length > 1 ? "Добавить (" + dlg.camaiSelected().length + ")"
                  : "Добавить"
            enabled: dlg.canAccept
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
