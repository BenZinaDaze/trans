import QtQuick
import QtQuick.Controls

TextField {
    id: root
    UiTheme { id: ui }
    implicitHeight: 38
    leftPadding: 12
    rightPadding: 12
    font.pixelSize: 13
    color: ui.text
    placeholderTextColor: ui.muted
    selectionColor: ui.accent
    selectedTextColor: ui.accentText
    selectByMouse: true
    background: Rectangle {
        radius: 7
        color: ui.inset
        border.color: root.activeFocus ? ui.accent : ui.line
        border.width: root.activeFocus ? 2 : 1
    }
}
