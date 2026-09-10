import QtQuick
import QtQuick.Controls

SpinBox {
    id: root
    UiTheme { id: ui }
    implicitWidth: 142
    implicitHeight: 36
    leftPadding: 30
    rightPadding: 30
    font.pixelSize: 13
    palette.text: ui.text
    palette.base: ui.inset
    palette.highlight: ui.accent
    palette.highlightedText: ui.accentText
    background: Rectangle {
        radius: 7
        color: ui.inset
        border.color: root.activeFocus ? ui.accent : ui.line
        opacity: root.enabled ? 1 : 0.45
    }
    up.indicator: Rectangle {
        x: root.width - width
        width: 28
        height: root.height
        color: "transparent"
        Text { anchors.centerIn: parent; text: "+"; color: root.enabled ? ui.text : ui.muted; font.pixelSize: 18 }
    }
    down.indicator: Rectangle {
        width: 28
        height: root.height
        color: "transparent"
        Text { anchors.centerIn: parent; text: "−"; color: root.enabled ? ui.text : ui.muted; font.pixelSize: 18 }
    }
}
