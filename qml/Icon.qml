import QtQuick

// Vector icons drawn by the application itself: they do not depend on the
// fonts installed in the system (symbol characters may be missing and show
// up as empty rectangles).
Canvas {
    id: icon
    property string name: ""
    property color color: "white"
    property real lineWidth: Math.max(1.4, width / 12)
    implicitWidth: 16
    implicitHeight: 16
    antialiasing: true

    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const c = getContext("2d")
        const w = width, h = height
        c.reset()
        c.clearRect(0, 0, w, h)
        c.strokeStyle = icon.color
        c.fillStyle = icon.color
        c.lineWidth = icon.lineWidth
        c.lineCap = "round"
        c.lineJoin = "round"
        const s = Math.min(w, h)
        const x0 = (w - s) / 2, y0 = (h - s) / 2
        const X = (f) => x0 + f * s
        const Y = (f) => y0 + f * s
        const line = (a, b, d, e) => { c.beginPath(); c.moveTo(X(a), Y(b)); c.lineTo(X(d), Y(e)); c.stroke() }
        const circle = (cx, cy, r, fill) => {
            c.beginPath(); c.arc(X(cx), Y(cy), r * s, 0, Math.PI * 2)
            if (fill) c.fill(); else c.stroke()
        }
        const rect = (a, b, rw, rh, fill) => {
            if (fill) c.fillRect(X(a), Y(b), rw * s, rh * s); else c.strokeRect(X(a), Y(b), rw * s, rh * s)
        }

        switch (icon.name) {
        case "plus":
            line(0.5, 0.15, 0.5, 0.85); line(0.15, 0.5, 0.85, 0.5); break
        case "close":
            line(0.22, 0.22, 0.78, 0.78); line(0.78, 0.22, 0.22, 0.78); break
        case "chevron":                    // drop-down arrow
            c.beginPath(); c.moveTo(X(0.22), Y(0.38)); c.lineTo(X(0.5), Y(0.66)); c.lineTo(X(0.78), Y(0.38)); c.stroke(); break
        case "more":                       // vertical dots
            circle(0.5, 0.2, 0.08, true); circle(0.5, 0.5, 0.08, true); circle(0.5, 0.8, 0.08, true); break
        case "play":
            c.beginPath(); c.moveTo(X(0.3), Y(0.2)); c.lineTo(X(0.8), Y(0.5)); c.lineTo(X(0.3), Y(0.8)); c.closePath(); c.fill(); break
        case "stop":
            rect(0.25, 0.25, 0.5, 0.5, true); break
        case "reload": {                   // circular arrow, gap at the top left
            const r = 0.3, a0 = -Math.PI * 0.35, a1 = Math.PI * 1.15
            c.beginPath(); c.arc(X(0.5), Y(0.5), r * s, a0, a1); c.stroke()
            const ex = 0.5 + r * Math.cos(a1), ey = 0.5 + r * Math.sin(a1)
            const dx = -Math.sin(a1), dy = Math.cos(a1)       // direction of the arc at its end
            const hw = 0.17, hl = 0.24
            c.beginPath()
            c.moveTo(X(ex + dx * hl * 0.6), Y(ey + dy * hl * 0.6))
            c.lineTo(X(ex - dx * hl * 0.4 + dy * hw), Y(ey - dy * hl * 0.4 - dx * hw))
            c.lineTo(X(ex - dx * hl * 0.4 - dy * hw), Y(ey - dy * hl * 0.4 + dx * hw))
            c.closePath(); c.fill(); break
        }
        case "gear": {
            const teeth = 6
            c.save(); c.lineWidth = icon.lineWidth * 2.2; c.lineCap = "butt"
            for (let i = 0; i < teeth; ++i) {
                const a = i * Math.PI * 2 / teeth
                line(0.5 + Math.cos(a) * 0.26, 0.5 + Math.sin(a) * 0.26, 0.5 + Math.cos(a) * 0.44, 0.5 + Math.sin(a) * 0.44)
            }
            c.restore()
            circle(0.5, 0.5, 0.26, false); circle(0.5, 0.5, 0.1, false); break
        }
        case "warning":
            c.beginPath(); c.moveTo(X(0.5), Y(0.1)); c.lineTo(X(0.92), Y(0.86)); c.lineTo(X(0.08), Y(0.86)); c.closePath(); c.stroke()
            line(0.5, 0.38, 0.5, 0.6); circle(0.5, 0.73, 0.04, true); break
        case "check":
            c.beginPath(); c.moveTo(X(0.18), Y(0.52)); c.lineTo(X(0.42), Y(0.75)); c.lineTo(X(0.82), Y(0.28)); c.stroke(); break
        case "streams":                    // 2x2 video tiles
            rect(0.12, 0.18, 0.34, 0.28, true); rect(0.54, 0.18, 0.34, 0.28, true)
            rect(0.12, 0.54, 0.34, 0.28, true); rect(0.54, 0.54, 0.34, 0.28, true); break
        case "cpu":
            rect(0.25, 0.25, 0.5, 0.5, false); rect(0.4, 0.4, 0.2, 0.2, true)
            for (const f of [0.35, 0.5, 0.65]) { line(f, 0.08, f, 0.25); line(f, 0.75, f, 0.92); line(0.08, f, 0.25, f); line(0.75, f, 0.92, f) }
            break
        case "temperature":
            c.beginPath(); c.moveTo(X(0.42), Y(0.62)); c.lineTo(X(0.42), Y(0.18))
            c.arc(X(0.5), Y(0.18), 0.08 * s, Math.PI, 0); c.lineTo(X(0.58), Y(0.62)); c.stroke()
            circle(0.5, 0.74, 0.14, true); line(0.5, 0.3, 0.5, 0.66); break
        case "memory":
            rect(0.1, 0.3, 0.8, 0.4, false)
            for (const f of [0.25, 0.42, 0.58, 0.75]) rect(f - 0.05, 0.38, 0.1, 0.24, true)
            for (const f of [0.2, 0.35, 0.5, 0.65, 0.8]) line(f, 0.7, f, 0.82)
            break
        case "network":                    // up / down arrows
            line(0.35, 0.15, 0.35, 0.85); c.beginPath(); c.moveTo(X(0.2), Y(0.32)); c.lineTo(X(0.35), Y(0.15)); c.lineTo(X(0.5), Y(0.32)); c.stroke()
            line(0.65, 0.15, 0.65, 0.85); c.beginPath(); c.moveTo(X(0.5), Y(0.68)); c.lineTo(X(0.65), Y(0.85)); c.lineTo(X(0.8), Y(0.68)); c.stroke()
            break
        case "decoder":                    // film frame with play
            rect(0.1, 0.2, 0.8, 0.6, false)
            c.beginPath(); c.moveTo(X(0.42), Y(0.36)); c.lineTo(X(0.64), Y(0.5)); c.lineTo(X(0.42), Y(0.64)); c.closePath(); c.fill(); break
        case "gpu":                        // monitor
            rect(0.1, 0.18, 0.8, 0.52, false); line(0.5, 0.7, 0.5, 0.84); line(0.3, 0.86, 0.7, 0.86); break
        case "dot":
            circle(0.5, 0.5, 0.25, true); break
        default:
            break
        }
    }
}
