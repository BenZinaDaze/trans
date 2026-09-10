import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: root
    property string symbol: ""
    property string hint: ""
    property bool quiet: false
    UiTheme { id: ui }
    implicitHeight: 36
    implicitWidth: Math.max(36, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: text.length ? 12 : 8
    rightPadding: leftPadding
    font.pixelSize: 13
    hoverEnabled: true
    Accessible.name: text || hint
    ToolTip.text: hint
    ToolTip.visible: hovered && hint.length > 0
    ToolTip.delay: 600
    background: Rectangle {
        radius: 7
        color: root.highlighted ? ui.accent : (root.down || root.hovered ? ui.hover : (root.quiet ? "transparent" : ui.surface))
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? ui.accent : (root.highlighted || root.quiet ? "transparent" : ui.line)
        opacity: root.enabled ? 1 : 0.45
    }
    contentItem: RowLayout {
        spacing: root.text.length && root.symbol.length ? 7 : 0
        opacity: root.enabled ? 1 : 0.45
        UiIcon {
            visible: root.symbol.length > 0
            name: root.symbol
            color: root.highlighted ? ui.accentText : (root.checked ? ui.accent : ui.text)
            implicitWidth: 18
            implicitHeight: 18
            Layout.alignment: Qt.AlignHCenter
        }
        Text {
            visible: root.text.length > 0
            text: root.text
            font: root.font
            color: root.highlighted ? ui.accentText : ui.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            Layout.fillWidth: true
        }
    }
}
