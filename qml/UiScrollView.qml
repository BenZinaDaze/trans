import QtQuick
import QtQuick.Controls

ScrollView {
    id: root
    UiTheme { id: ui }
    clip: true
    contentWidth: availableWidth
    rightPadding: 14
    // Handle wheels after child controls, including the scrollbar. At a boundary,
    // leave the event for an enclosing scroll view instead of swallowing it.
    wheelEnabled: false
    MouseArea {
        parent: root
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.NoButton
        onWheel: function(wheel) {
            const flickable = root.contentItem as Flickable
            const delta = wheel.pixelDelta.y || wheel.angleDelta.y / 120 * Application.styleHints.wheelScrollLines * 24
            const minimum = flickable.originY
            const maximum = minimum + Math.max(0, flickable.contentHeight - flickable.height)
            const next = Math.max(minimum, Math.min(maximum, flickable.contentY - delta))
            wheel.accepted = delta !== 0 && next !== flickable.contentY
            if (wheel.accepted) {
                flickable.cancelFlick()
                flickable.contentY = next
            }
        }
    }
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical: ScrollBar {
        parent: root
        x: root.width - width
        y: root.topPadding
        height: root.availableHeight
        orientation: Qt.Vertical
        implicitWidth: 12
        minimumSize: Math.min(1, 24 / Math.max(1, height))
        padding: 0
        leftPadding: 3.5
        rightPadding: 3.5
        interactive: true
        visible: size < 1
        active: visible
        contentItem: Rectangle {
            implicitWidth: 5
            implicitHeight: 20
            radius: 2.5
            color: ui.muted
            opacity: 0.5
        }
        background: null
    }
}
