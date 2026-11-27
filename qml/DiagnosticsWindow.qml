import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import HmiCore

// Diagnostics: every important subsystem as a card with a traffic light,
// infographics (ring gauge, bars, stacked bar) and advice.
ApplicationWindow {
    id: dw
    title: "Диагностика — " + App.productName
    color: "#0e1013"
    width: Math.round(em * 64)
    height: Math.round(em * 44)
    minimumWidth: 520
    minimumHeight: 360

    FontMetrics { id: fm; font: dw.font }
    readonly property real em: fm.height
    readonly property var diag: App.diag

    palette.window: "#1d2024"
    palette.windowText: "#e6e6e6"
    palette.base: "#15171a"
    palette.text: "#e6e6e6"
    palette.button: "#2a2e34"
    palette.buttonText: "#e6e6e6"
    palette.highlight: "#2563eb"
    palette.highlightedText: "#ffffff"
    palette.toolTipBase: "#2a2e34"
    palette.toolTipText: "#e6e6e6"

    function statusColor(s) {
        return s === 2 ? "#ef4444" : s === 1 ? "#eab308" : s === 0 ? "#22c55e" : "#6b7280"
    }
    function statusName(s) {
        return s === 2 ? "серьёзные проблемы" : s === 1 ? "замечания" : s === 0 ? "норма" : "нет данных"
    }

    // ------------------------------------------------------------- components
    component RingGauge: Canvas {
        id: ring
        property real fraction: 0
        property color color: "#22c55e"
        property string centerText: ""
        property string label: ""
        width: Math.round(dw.em * 5.6)
        height: width
        onFractionChanged: requestPaint()
        onColorChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const r = width / 2 - 6
            const start = Math.PI * 0.75
            const span = Math.PI * 1.5
            ctx.lineWidth = Math.max(6, width * 0.09)
            ctx.lineCap = "round"
            ctx.strokeStyle = "#2a2f36"
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, r, start, start + span)
            ctx.stroke()
            if (fraction > 0.001) {
                ctx.strokeStyle = ring.color
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, r, start, start + span * Math.min(1, fraction))
                ctx.stroke()
            }
        }
        Column {
            anchors.centerIn: parent
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: ring.centerText
                font.pixelSize: Math.round(dw.em * 1.05)
                font.bold: true
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                width: ring.width * 0.7
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: ring.label
                opacity: 0.6
                font.pixelSize: Math.max(9, Math.round(dw.em * 0.55))
            }
        }
    }

    component StatusPill: Rectangle {
        property int status: 0
        height: Math.round(dw.em * 1.25)
        width: pillRow.implicitWidth + dw.em * 0.8
        radius: height / 2
        color: Qt.rgba(0, 0, 0, 0.35)
        border.color: dw.statusColor(status)
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 5
            Rectangle {
                width: dw.em * 0.55; height: width; radius: width / 2
                anchors.verticalCenter: parent.verticalCenter
                color: dw.statusColor(parent.parent.status)
            }
            Label {
                text: dw.statusName(parent.parent.status)
                font.pixelSize: Math.max(10, Math.round(dw.em * 0.65))
            }
        }
    }

    // ------------------------------------------------------------- header
    header: Rectangle {
        height: headerRow.implicitHeight + dw.em * 1.2
        color: Qt.rgba(dw.statusColor(dw.diag.status).r, dw.statusColor(dw.diag.status).g,
                       dw.statusColor(dw.diag.status).b, 0.18)
        Rectangle {
            anchors.bottom: parent.bottom; width: parent.width; height: 2
            color: dw.statusColor(dw.diag.status)
        }
        RowLayout {
            id: headerRow
            anchors.fill: parent
            anchors.margins: dw.em * 0.6
            spacing: dw.em * 0.8
            Rectangle {
                id: bigDot
                Layout.alignment: Qt.AlignVCenter
                width: dw.em * 2.2; height: width; radius: width / 2
                color: dw.statusColor(dw.diag.status)
                SequentialAnimation on opacity {
                    running: dw.diag.status === 2
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.45; duration: 700 }
                    NumberAnimation { to: 1.0; duration: 700 }
                    onRunningChanged: if (!running) bigDot.opacity = 1
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: dw.diag.status === 2 ? "Есть серьёзные проблемы"
                          : dw.diag.status === 1 ? "Есть незначительные проблемы" : "Система работает нормально"
                    font.bold: true
                    font.pixelSize: Math.round(dw.em * 1.05)
                }
                Label {
                    Layout.fillWidth: true
                    text: dw.diag.summary
                    wrapMode: Text.Wrap
                    opacity: 0.85
                }
            }
            ColumnLayout {
                spacing: 4
                Label {
                    Layout.alignment: Qt.AlignRight
                    text: "обновлено " + dw.diag.updatedAt
                    opacity: 0.6
                    font.pixelSize: Math.max(10, Math.round(dw.em * 0.65))
                }
                RowLayout {
                    Button { text: "Обновить"; onClicked: dw.diag.refresh() }
                    Button {
                        id: copyBtn
                        text: "Скопировать отчёт"
                        onClicked: {
                            dw.diag.copyReport()
                            copiedTip.visible = true
                            copiedTimer.restart()
                        }
                        ToolTip { id: copiedTip; text: "Отчёт скопирован в буфер обмена" }
                        Timer { id: copiedTimer; interval: 2000; onTriggered: copiedTip.visible = false }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------- cards
    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: dw.em * 0.6
        contentWidth: availableWidth
        clip: true

        Flow {
            id: flow
            width: scroll.availableWidth
            spacing: dw.em * 0.6
            readonly property int cols: Math.max(1, Math.floor((width + spacing) / (dw.em * 25 + spacing)))
            readonly property real cardWidth: (width - (cols - 1) * spacing) / cols

            Repeater {
                model: dw.diag.sections
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property int st: modelData.status
                    width: flow.cardWidth
                    height: cardCol.implicitHeight + dw.em * 1.2
                    radius: 8
                    color: "#161a1f"
                    border.width: 1
                    border.color: Qt.rgba(dw.statusColor(st).r, dw.statusColor(st).g, dw.statusColor(st).b,
                                          st > 0 ? 0.8 : 0.3)

                    ColumnLayout {
                        id: cardCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: dw.em * 0.6
                        spacing: dw.em * 0.45

                        // Title row
                        RowLayout {
                            Layout.fillWidth: true
                            Icon { name: card.modelData.icon; color: palette.windowText; opacity: 0.8; implicitWidth: Math.round(dw.em * 1.0); implicitHeight: implicitWidth }
                            Label {
                                text: card.modelData.title
                                font.bold: true
                                font.pixelSize: Math.round(dw.em * 0.85)
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            StatusPill { status: card.st }
                        }

                        // Gauge + headline + details
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: dw.em * 0.7
                            RingGauge {
                                visible: card.modelData.fraction !== undefined && card.modelData.fraction >= 0
                                fraction: Math.max(0, card.modelData.fraction || 0)
                                color: dw.statusColor(card.st < 0 ? 0 : card.st)
                                centerText: card.modelData.gaugeText || ""
                                label: card.modelData.gaugeLabel || ""
                            }
                            // Column (not ColumnLayout): wrapped lines get their real height.
                            Column {
                                id: infoCol
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                spacing: 3
                                Label {
                                    width: infoCol.width
                                    text: card.modelData.headline
                                    font.pixelSize: Math.round(dw.em * 1.0)
                                    font.bold: true
                                    wrapMode: Text.Wrap
                                    color: card.st > 0 ? dw.statusColor(card.st) : palette.windowText
                                }
                                Repeater {
                                    model: card.modelData.details || []
                                    delegate: Label {
                                        required property var modelData
                                        width: infoCol.width
                                        wrapMode: Text.Wrap
                                        textFormat: Text.StyledText
                                        font.pixelSize: Math.max(10, Math.round(dw.em * 0.68))
                                        text: "<span style='color:#9ca3af'>" + modelData.k + ":</span> " + modelData.v
                                    }
                                }
                            }
                        }

                        // Stacked bar (streams, decoders)
                        ColumnLayout {
                            id: segBox
                            Layout.fillWidth: true
                            visible: (card.modelData.segments || []).length > 0
                            spacing: 3
                            readonly property var segs: card.modelData.segments || []
                            readonly property int total: {
                                let t = 0
                                for (let i = 0; i < segs.length; ++i) t += segs[i].count
                                return t
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: Math.round(dw.em * 0.75)
                                radius: height / 2
                                color: "#2a2f36"
                                clip: true
                                Row {
                                    id: segRow
                                    anchors.fill: parent
                                    Repeater {
                                        model: segBox.segs
                                        delegate: Rectangle {
                                            required property var modelData
                                            height: segRow.height
                                            width: segBox.total > 0 ? segRow.width * modelData.count / segBox.total : 0
                                            color: modelData.color
                                        }
                                    }
                                }
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: dw.em * 0.7
                                Repeater {
                                    model: segBox.segs
                                    delegate: Row {
                                        required property var modelData
                                        spacing: 4
                                        visible: modelData.count > 0
                                        Rectangle {
                                            width: dw.em * 0.55; height: width; radius: 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: modelData.color
                                        }
                                        Label {
                                            text: modelData.label + ": " + modelData.count
                                            font.pixelSize: Math.max(10, Math.round(dw.em * 0.65))
                                        }
                                    }
                                }
                            }
                        }

                        // Horizontal bars (cores, sensors, interfaces, windows)
                        GridLayout {
                            Layout.fillWidth: true
                            visible: (card.modelData.bars || []).length > 0
                            columns: 3
                            columnSpacing: dw.em * 0.5
                            rowSpacing: 3
                            Repeater {
                                model: card.modelData.bars || []
                                delegate: Label {
                                    required property var modelData
                                    text: modelData.label
                                    Layout.row: index
                                    Layout.column: 0
                                    Layout.maximumWidth: dw.em * 8
                                    elide: Text.ElideRight
                                    font.pixelSize: Math.max(10, Math.round(dw.em * 0.65))
                                    opacity: 0.8
                                    required property int index
                                }
                            }
                            Repeater {
                                model: card.modelData.bars || []
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    Layout.row: index
                                    Layout.column: 1
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: dw.em * 3
                                    height: Math.round(dw.em * 0.55)
                                    radius: height / 2
                                    color: "#2a2f36"
                                    Rectangle {
                                        height: parent.height
                                        radius: parent.radius
                                        width: parent.width * modelData.fraction
                                        color: dw.statusColor(modelData.status)
                                    }
                                }
                            }
                            Repeater {
                                model: card.modelData.bars || []
                                delegate: Label {
                                    required property var modelData
                                    required property int index
                                    Layout.row: index
                                    Layout.column: 2
                                    text: modelData.text
                                    font.pixelSize: Math.max(10, Math.round(dw.em * 0.65))
                                }
                            }
                        }

                        // Problem players (streams card)
                        Column {
                            id: probCol
                            Layout.fillWidth: true
                            visible: (card.modelData.problems || []).length > 0
                            spacing: 2
                            Repeater {
                            model: card.modelData.problems || []
                            delegate: Label {
                                required property var modelData
                                width: probCol.width
                                wrapMode: Text.Wrap
                                textFormat: Text.StyledText
                                font.pixelSize: Math.max(10, Math.round(dw.em * 0.68))
                                text: "<b>" + modelData.k + "</b> — " + modelData.v
                            }
                            }
                        }

                        // Advice
                        Rectangle {
                            Layout.fillWidth: true
                            visible: (card.modelData.advice || []).length > 0
                            implicitHeight: adviceCol.implicitHeight + dw.em * 0.8
                            radius: 6
                            color: Qt.rgba(dw.statusColor(Math.max(card.st, 1)).r, dw.statusColor(Math.max(card.st, 1)).g,
                                           dw.statusColor(Math.max(card.st, 1)).b, 0.10)
                            Rectangle {
                                width: 3; height: parent.height; radius: 1.5
                                color: dw.statusColor(Math.max(card.st, 1))
                            }
                            Column {
                                id: adviceCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: dw.em * 0.4
                                anchors.leftMargin: dw.em * 0.7
                                spacing: 3
                                Label {
                                    text: card.st > 0 ? "Что сделать" : "Примечание"
                                    font.bold: true
                                    font.pixelSize: Math.max(10, Math.round(dw.em * 0.7))
                                }
                                Repeater {
                                    model: card.modelData.advice || []
                                    delegate: Label {
                                        required property var modelData
                                        width: adviceCol.width
                                        wrapMode: Text.Wrap
                                        font.pixelSize: Math.max(10, Math.round(dw.em * 0.68))
                                        text: "- " + modelData
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
