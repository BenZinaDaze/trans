import QtQuick
import QtQuick.Controls

TextArea {
    id: root
    UiTheme { id: ui }
    padding: 12
    font.pixelSize: 13
    color: ui.text
    placeholderTextColor: ui.muted
    selectionColor: ui.accent
    selectedTextColor: ui.accentText
    textFormat: TextEdit.PlainText
    selectByMouse: true
    wrapMode: TextEdit.Wrap
    background: Rectangle {
        radius: 7
        color: ui.inset
        border.color: root.activeFocus ? ui.accent : ui.line
    }
}
