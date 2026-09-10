import QtQuick

// Small, theme-aware line icons. Coordinates use a 24 px view box.
Canvas {
    id: root
    property string name: "language"
    property color color: "#687183"
    implicitWidth: 20
    implicitHeight: 20
    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.scale(width / 24, height / 24)
        c.strokeStyle = color
        c.lineWidth = 1.7
        c.lineCap = "round"
        c.lineJoin = "round"
        c.beginPath()
        switch (name) {
        case "copy":
            c.roundedRect(8, 8, 12, 13, 2, 2)
            c.moveTo(15, 4); c.lineTo(5, 4); c.lineTo(5, 16)
            break
        case "refresh":
            c.arc(12, 12, 8, 0.6, 5.7)
            c.moveTo(19, 3); c.lineTo(19, 8); c.lineTo(14, 8)
            break
        case "arrow":
            c.moveTo(5, 12); c.lineTo(19, 12)
            c.moveTo(14, 7); c.lineTo(19, 12); c.lineTo(14, 17)
            break
        case "down":
            c.moveTo(7, 10); c.lineTo(12, 15); c.lineTo(17, 10)
            break
        case "close":
            c.moveTo(6, 6); c.lineTo(18, 18)
            c.moveTo(18, 6); c.lineTo(6, 18)
            break
        case "check":
            c.moveTo(5, 12); c.lineTo(10, 17); c.lineTo(19, 7)
            break
        case "key":
            c.arc(8, 9, 4, 0, Math.PI * 2)
            c.moveTo(11, 12); c.lineTo(20, 21)
            c.moveTo(16, 17); c.lineTo(19, 14)
            break
        case "window":
            c.roundedRect(3, 4, 18, 16, 2, 2)
            c.moveTo(3, 9); c.lineTo(21, 9)
            c.moveTo(8, 9); c.lineTo(8, 20)
            break
        case "service":
            c.roundedRect(4, 3, 16, 7, 2, 2)
            c.roundedRect(4, 14, 16, 7, 2, 2)
            c.moveTo(8, 6); c.lineTo(8, 7)
            c.moveTo(8, 17); c.lineTo(8, 18)
            c.moveTo(12, 6.5); c.lineTo(16, 6.5)
            c.moveTo(12, 17.5); c.lineTo(16, 17.5)
            break
        case "settings":
            c.moveTo(4, 6); c.lineTo(20, 6)
            c.moveTo(4, 12); c.lineTo(20, 12)
            c.moveTo(4, 18); c.lineTo(20, 18)
            c.moveTo(9, 3); c.lineTo(9, 9)
            c.moveTo(16, 9); c.lineTo(16, 15)
            c.moveTo(8, 15); c.lineTo(8, 21)
            break
        case "info":
            c.arc(12, 12, 9, 0, Math.PI * 2)
            c.moveTo(12, 11); c.lineTo(12, 17)
            c.moveTo(12, 7); c.lineTo(12, 7.2)
            break
        default:
            c.moveTo(3, 5); c.lineTo(14, 5)
            c.moveTo(8, 3); c.lineTo(8, 5)
            c.moveTo(12, 5); c.quadraticCurveTo(11, 12, 4, 16)
            c.moveTo(5, 8); c.quadraticCurveTo(7, 12, 11, 14)
            c.moveTo(12, 21); c.lineTo(17, 9); c.lineTo(22, 21)
            c.moveTo(14, 17); c.lineTo(20, 17)
        }
        c.stroke()
    }
}
