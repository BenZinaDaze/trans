import QtQuick
import QtQuick.Controls

CheckBox {
    id: root
    UiTheme { id: ui }
    implicitHeight: 36
    spacing: 10
    font.pixelSize: 13
    indicator: Rectangle {
        implicitWidth: 32
        implicitHeight: 18
        x: root.leftPadding
        y: (root.height - height) / 2
        radius: 9
        color: root.checked ? ui.accent : ui.hover
        border.color: root.activeFocus ? ui.accent : (root.checked ? "transparent" : ui.line)
        border.width: root.activeFocus ? 2 : 1
        Rectangle {
            x: root.checked ? 16 : 3
            y: 3
            width: 12
            height: 12
            radius: 6
            color: root.checked ? ui.accentText : ui.muted
            Behavior on x { NumberAnimation { duration: 100 } }
        }
    }
    contentItem: Text {
        text: root.text
        font: root.font
        color: root.enabled ? ui.text : ui.muted
        leftPadding: root.indicator.width + root.spacing
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
    }
}
