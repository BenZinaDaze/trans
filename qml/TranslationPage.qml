import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Tran.Core

UiScrollView {
    id: root
    required property AppSettings appSettings
    contentWidth: availableWidth
    clip: true
    property var sourceLanguages: [{id: "auto", name: "自动检测"}].concat(appSettings.languages)
    function load(values) {
        source.currentIndex = Math.max(0, sourceLanguages.findIndex(function(item) { return item.id === values.sourceLanguage }))
        target.currentIndex = Math.max(0, appSettings.languages.findIndex(function(item) { return item.id === values.targetLanguage }))
        prompt.text = values.systemPrompt
        timeout.value = values.timeoutSeconds
        maxInput.value = values.maxInputChars
        maxResponse.value = values.maxResponseKiB
    }
    function write(values) {
        values.sourceLanguage = source.currentValue
        values.targetLanguage = target.currentValue
        values.systemPrompt = prompt.text
        values.timeoutSeconds = timeout.value
        values.maxInputChars = maxInput.value
        values.maxResponseKiB = maxResponse.value
    }
    UiTheme { id: ui }
    ColumnLayout {
        width: root.availableWidth
        spacing: 16
        UiSection {
            Layout.fillWidth: true
            title: "翻译语言"
            description: "默认自动识别原文，将译文转换为你选择的语言。"
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "源语言"; color: ui.muted; font.pixelSize: 12 }
                    UiCombo { id: source; objectName: "sourceLanguageField"; Layout.fillWidth: true; model: root.sourceLanguages; textRole: "name"; valueRole: "id" }
                }
                UiIcon { name: "arrow"; color: ui.muted; Layout.topMargin: 24 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: "目标语言"; color: ui.muted; font.pixelSize: 12 }
                    UiCombo { id: target; objectName: "targetLanguageField"; Layout.fillWidth: true; model: root.appSettings.languages; textRole: "name"; valueRole: "id" }
                }
            }
        }
        UiSection {
            Layout.fillWidth: true
            title: "翻译提示词"
            description: "告诉模型你希望采用的语气、风格和翻译要求。"
            UiScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 166
                clip: true
                UiTextArea { id: prompt; objectName: "promptField" }
            }
            Label { text: "{{sourceLanguage}} 与 {{targetLanguage}} 会替换为所选语言。请保留目标语言占位符。"; Layout.fillWidth: true; wrapMode: Text.Wrap; color: ui.muted; font.pixelSize: 11 }
            UiButton { text: "恢复默认提示词"; symbol: "refresh"; quiet: true; onClicked: prompt.text = root.appSettings.defaults().systemPrompt }
        }
        UiSection {
            Layout.fillWidth: true
            title: "请求限制"
            description: "根据文本长度和网络情况调整。"
            RowLayout {
                Layout.fillWidth: true
                Label { text: "请求超时（秒）"; color: ui.text; Layout.fillWidth: true; wrapMode: Text.Wrap }
                UiSpin { id: timeout; objectName: "timeoutField"; from: 1; to: 600; editable: true }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ui.line }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "选中文本长度上限"; color: ui.text; Layout.fillWidth: true; wrapMode: Text.Wrap }
                UiSpin { id: maxInput; objectName: "maxInputField"; from: 1; to: 200000; stepSize: 1000; editable: true }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ui.line }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "响应大小上限（KiB）"; color: ui.text; Layout.fillWidth: true; wrapMode: Text.Wrap }
                UiSpin { id: maxResponse; objectName: "maxResponseField"; from: 16; to: 16384; stepSize: 256; editable: true }
            }
        }
    }
}
