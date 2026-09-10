pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

ComboBox {
    id: root
    UiTheme { id: ui }
    implicitHeight: 38
    leftPadding: 12
    rightPadding: 32
    font.pixelSize: 13
    palette.text: ui.text
    palette.buttonText: ui.text
    palette.base: ui.inset
    palette.button: ui.inset
    palette.highlight: ui.accent
    palette.highlightedText: ui.accentText
    selectTextByMouse: true
    background: Rectangle {
        radius: 7
        color: root.hovered ? ui.hover : ui.inset
        border.color: root.activeFocus ? ui.accent : ui.line
        border.width: root.activeFocus ? 2 : 1
    }
    indicator: UiIcon {
        x: root.width - width - 10
        y: (root.height - height) / 2
        name: "down"
        color: ui.muted
        implicitWidth: 16
        implicitHeight: 16
    }
    delegate: ItemDelegate {
        id: option
        required property int index
        width: ListView.view ? ListView.view.width : root.width
        implicitHeight: 36
        highlighted: root.highlightedIndex === index
        contentItem: Text {
            text: root.textAt(option.index)
            color: option.highlighted ? ui.accent : ui.text
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle { radius: 5; color: option.highlighted ? ui.accentSoft : "transparent" }
    }
    popup: Popup {
        y: root.height + 4
        width: root.width
        padding: 5
        implicitHeight: Math.min(contentItem.implicitHeight + 10, 260)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle { color: ui.surface; radius: 8; border.color: ui.line }
    }
}
