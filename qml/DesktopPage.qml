import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trans.Core

UiScrollView {
    id: root
    required property DesktopBridge desktop
    required property AppSettings appSettings
    property bool recording: false
    property bool recordingScreenshot: false
    property string screenshotShortcut: appSettings.defaults().screenshotShortcut
    property string shortcut: appSettings.defaults().shortcut
    contentWidth: availableWidth
    clip: true
    function load(values) {
        shortcut = values.shortcut
        screenshotShortcut = values.screenshotShortcut
        recordingScreenshot = false
        recording = false
        popupPosition.currentIndex = values.popupPosition === "cursor" ? 1 : 0
        fontSize.value = values.fontSize
        stayOnTop.checked = values.stayOnTop
        restoreFocus.checked = values.restoreFocus
    }
    function write(values) {
        values.shortcut = shortcut
        values.screenshotShortcut = screenshotShortcut
        values.popupPosition = popupPosition.currentIndex === 1 ? "cursor" : "screen"
        values.fontSize = fontSize.value
        values.stayOnTop = stayOnTop.checked
        values.restoreFocus = restoreFocus.checked
    }
    UiTheme { id: ui }
    ColumnLayout {
        width: root.availableWidth
        spacing: 16
        UiSection {
            Layout.fillWidth: true
            title: "选区翻译快捷键"
            description: root.desktop.capabilities.selection.available
                ? "选中一个词或一句话，随时呼出翻译窗口。"
                : root.desktop.capabilities.selection.reason
            RowLayout {
                enabled: root.desktop.capabilities.shortcuts.available
                Layout.fillWidth: true
                spacing: 8
                UiField {
                    id: shortcutField
                    objectName: "shortcutField"
                    Layout.fillWidth: true
                    text: root.recording ? "请按下快捷键…" : root.shortcut
                    placeholderText: "已禁用全局快捷键"
                    readOnly: true
                    Keys.onPressed: function(event) {
                        if (!root.recording)
                            return
                        event.accepted = true
                        if (event.key === Qt.Key_Escape) {
                            root.recording = false
                        } else {
                            const sequence = root.desktop.shortcutForKey(event.key, event.modifiers)
                            if (sequence.length > 0) {
                                root.shortcut = sequence
                                root.recording = false
                            }
                        }
                    }
                    onActiveFocusChanged: { if (!activeFocus) root.recording = false }
                }
                UiButton {
                    text: root.recording ? "取消" : "录制"
                    highlighted: root.recording
                    objectName: "recordShortcutButton"
                    onClicked: {
                        root.recording = !root.recording
                        if (root.recording)
                            shortcutField.forceActiveFocus()
                    }
                }
                UiButton { text: "清空"; quiet: true; onClicked: { root.shortcut = ""; root.recording = false } }
            }
            Label { text: "组合键需包含 Ctrl、Alt 或 Meta。按 Esc 取消录制。"; font.pixelSize: 11; color: ui.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
            UiButton { text: "恢复默认快捷键"; symbol: "refresh"; quiet: true; enabled: root.desktop.capabilities.shortcuts.available; onClicked: root.shortcut = root.appSettings.defaults().shortcut }
            Label { text: root.desktop.shortcutError; color: ui.danger; font.pixelSize: 12; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
        UiSection {
            Layout.fillWidth: true
            title: "截图翻译快捷键"
            description: root.desktop.capabilities.screenshots.available
                ? "鼠标拖动框选文字，松开后自动识别并翻译。"
                : root.desktop.capabilities.screenshots.reason
            RowLayout {
                enabled: root.desktop.capabilities.shortcuts.available
                Layout.fillWidth: true
                spacing: 8
                UiField {
                    id: screenshotShortcutField
                    objectName: "screenshotShortcutField"
                    Layout.fillWidth: true
                    text: root.recordingScreenshot ? "请按下快捷键…" : root.screenshotShortcut
                    placeholderText: "已禁用截图快捷键"
                    readOnly: true
                    Keys.onPressed: function(event) {
                        if (!root.recordingScreenshot) return
                        event.accepted = true
                        if (event.key === Qt.Key_Escape) {
                            root.recordingScreenshot = false
                        } else {
                            const sequence = root.desktop.shortcutForKey(event.key, event.modifiers)
                            if (sequence.length > 0) {
                                root.screenshotShortcut = sequence
                                root.recordingScreenshot = false
                            }
                        }
                    }
                    onActiveFocusChanged: { if (!activeFocus) root.recordingScreenshot = false }
                }
                UiButton {
                    text: root.recordingScreenshot ? "取消" : "录制"
                    highlighted: root.recordingScreenshot
                    objectName: "recordScreenshotShortcutButton"
                    onClicked: {
                        root.recordingScreenshot = !root.recordingScreenshot
                        if (root.recordingScreenshot) screenshotShortcutField.forceActiveFocus()
                    }
                }
                UiButton { text: "清空"; quiet: true; onClicked: { root.screenshotShortcut = ""; root.recordingScreenshot = false } }
            }
            UiButton { text: "恢复默认快捷键"; symbol: "refresh"; quiet: true; enabled: root.desktop.capabilities.shortcuts.available; onClicked: root.screenshotShortcut = root.appSettings.defaults().screenshotShortcut }
        }
        UiSection {
            Layout.fillWidth: true
            title: "翻译窗口"
            description: "窗口随原文和译文自动调整大小，长文本可滚动阅读。"
            Label { text: "弹出位置"; color: ui.muted; font.pixelSize: 12 }
            UiCombo { id: popupPosition; objectName: "positionField"; Layout.fillWidth: true; model: ["鼠标所在屏幕中央", "鼠标附近"] }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "译文字号"; color: ui.text; Layout.fillWidth: true }
                UiSpin { id: fontSize; objectName: "fontSizeField"; from: 10; to: 32; editable: true }
            }
        }
        UiSection {
            Layout.fillWidth: true
            title: "窗口行为"
            UiCheck { id: stayOnTop; objectName: "stayOnTopField"; text: "翻译窗口保持置顶"; Layout.fillWidth: true }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ui.line }
            UiCheck { id: restoreFocus; objectName: "restoreFocusField"; text: "关闭后恢复原应用焦点"; enabled: root.desktop.capabilities.focusRestoration.available; Layout.fillWidth: true }
            Label { text: root.desktop.capabilities.focusRestoration.reason; visible: text.length > 0; color: ui.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
    }
}
