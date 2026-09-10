import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root
    property string title: ""
    property string description: ""
    default property alias fields: body.data
    UiTheme { id: ui }
    padding: 20
    background: Rectangle { radius: 12; color: ui.surface; border.color: ui.line }
    contentItem: ColumnLayout {
        spacing: 14
        ColumnLayout {
            spacing: 5
            Layout.fillWidth: true
            Label { text: root.title; color: ui.text; font.pixelSize: 15; font.weight: Font.DemiBold }
            Label {
                visible: root.description.length > 0
                text: root.description
                color: ui.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
        ColumnLayout { id: body; spacing: 10; Layout.fillWidth: true }
    }
}
