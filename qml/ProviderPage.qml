import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Tran.Core

UiScrollView {
    id: root
    required property AppSettings appSettings
    required property ProviderTools providerTools
    property var configs: ({})
    property var descriptor: ({})
    property string editingProvider: ""
    property var modelOptions: []
    signal fetchRequested(string id)
    signal testRequested(string id)
    contentWidth: availableWidth
    clip: true

    function forget() {
        apiKey.text = ""
        customHeaders.text = "{}"
        extraOptions.text = "{}"
        configs = ({})
        editingProvider = ""
        revealKey.checked = false
    }
    function load(values, selected) {
        editingProvider = ""
        configs = values
        let index = 0
        for (let i = 0; i < appSettings.providers.length; ++i) {
            if (appSettings.providers[i].id === selected)
                index = i
        }
        provider.currentIndex = index
        selectProvider(index)
    }
    function flush() {
        if (!editingProvider)
            return
        configs[editingProvider] = {
            endpoint: endpoint.text, model: modelName.editText, apiKey: apiKey.text,
            apiMode: editingProvider === "openai" && apiMode.currentIndex === 0 ? "responses" : "chat",
            temperatureEnabled: enableTemperature.checked, temperature: temperature.value / 100,
            maxOutputTokens: outputTokens.value,
            reasoning: reasoning.currentValue || "default",
            headersJson: customHeaders.text, optionsJson: extraOptions.text
        }
    }
    function selectProvider(index) {
        flush()
        providerTools.clear()
        descriptor = appSettings.providers[index]
        editingProvider = descriptor.id
        const config = configs[editingProvider]
        endpoint.text = config.endpoint
        modelOptions = descriptor.models
        modelName.editText = config.model
        apiKey.text = config.apiKey
        revealKey.checked = false
        apiMode.currentIndex = config.apiMode === "responses" ? 0 : 1
        enableTemperature.checked = config.temperatureEnabled
        temperature.value = Math.round(config.temperature * 100)
        outputTokens.value = config.maxOutputTokens
        reasoning.currentIndex = Math.max(0, descriptor.reasoningOptions.indexOf(config.reasoning))
        customHeaders.text = config.headersJson
        extraOptions.text = config.optionsJson
    }
    function reasoningLabel(value) {
        const labels = { "default": "服务默认（不发送）", "none": "关闭推理", "minimal": "最低",
            "low": "低", "medium": "中", "high": "高", "xhigh": "极高", "max": "最高" }
        return labels[value] || value
    }
    Connections {
        target: root.providerTools
        function onChanged() {
            if (root.providerTools.models.length > 0) {
                const selected = modelName.editText
                root.modelOptions = root.providerTools.models
                modelName.editText = selected
            }
        }
    }

    UiTheme { id: ui }
    ColumnLayout {
        width: root.availableWidth
        spacing: 16
        UiSection {
            Layout.fillWidth: true
            title: "服务连接"
            description: "每家服务独立保存配置，当前选择用于划词翻译。"
            Label { text: "当前提供商"; color: ui.muted; font.pixelSize: 12 }
            UiCombo {
                id: provider
                objectName: "providerCombo"
                Layout.fillWidth: true
                model: root.appSettings.providers
                textRole: "name"
                onActivated: function(index) { root.selectProvider(index) }
            }
            Label { text: "服务地址"; color: ui.muted; font.pixelSize: 12; Layout.topMargin: 4 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                UiField { id: endpoint; objectName: "endpointField"; Layout.fillWidth: true; placeholderText: "https://api.example.com/v1" }
                UiButton { text: "重置"; hint: "恢复默认地址"; onClicked: endpoint.text = root.descriptor.defaultEndpoint }
            }
            Label { text: root.descriptor.endpointHint || ""; Layout.fillWidth: true; wrapMode: Text.Wrap; color: ui.muted; font.pixelSize: 11 }
            Label { text: "API 密钥"; color: ui.muted; font.pixelSize: 12; Layout.topMargin: 4 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                UiField {
                    id: apiKey
                    objectName: "apiKeyField"
                    Layout.fillWidth: true
                    placeholderText: "输入 API Key"
                    echoMode: revealKey.checked ? TextInput.Normal : TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                }
                UiCheck { id: revealKey; text: "显示" }
            }
            Label { text: "模型"; color: ui.muted; font.pixelSize: 12; Layout.topMargin: 4 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                UiCombo {
                    id: modelName
                    objectName: "modelField"
                    Layout.fillWidth: true
                    editable: true
                    model: root.modelOptions
                }
                UiButton {
                    text: "获取模型"
                    objectName: "fetchModelsButton"
                    enabled: !root.providerTools.busy
                    onClicked: root.fetchRequested(root.editingProvider)
                }
            }
            Label { text: "可从列表选择，也可直接输入模型名称。"; color: ui.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ui.line; Layout.topMargin: 4; Layout.bottomMargin: 4 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                UiButton {
                    text: "测试翻译"
                    symbol: "language"
                    objectName: "testProviderButton"
                    enabled: !root.providerTools.busy
                    onClicked: root.testRequested(root.editingProvider)
                }
                BusyIndicator { running: root.providerTools.busy; visible: running; implicitWidth: 24; implicitHeight: 24 }
                UiButton { text: "取消"; visible: root.providerTools.busy; quiet: true; onClicked: root.providerTools.clear() }
                Label { text: "使用当前填写的配置"; font.pixelSize: 11; color: ui.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            Label {
                text: root.providerTools.message
                visible: text.length > 0
                color: ui.text
                font.pixelSize: 12
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
        }
        UiSection {
            Layout.fillWidth: true
            title: "模型参数"
            description: "按模型能力配置；使用服务默认值可获得更好的兼容性。"
            Label { text: "OpenAI API 模式"; color: ui.muted; font.pixelSize: 12; visible: root.editingProvider === "openai" }
            UiCombo {
                id: apiMode
                objectName: "apiModeField"
                visible: root.editingProvider === "openai"
                Layout.fillWidth: true
                model: ["Responses（默认）", "Chat Completions"]
            }
            RowLayout {
                Layout.fillWidth: true
                UiCheck { id: enableTemperature; objectName: "temperatureEnabledField"; text: "手动调整随机性（温度）"; Layout.fillWidth: true }
                UiSpin {
                    id: temperature
                    objectName: "temperatureField"
                    from: 0; to: 200; stepSize: 10
                    editable: true
                    enabled: enableTemperature.checked
                    validator: DoubleValidator { bottom: 0; top: 2; decimals: 2; locale: "C" }
                    textFromValue: function(value) { return (value / 100).toFixed(2) }
                    valueFromText: function(text) { return Math.round(Number(text) * 100) }
                }
            }
            Label {
                text: "数值越低，措辞通常越稳定；关闭时使用服务默认值，填写的数值不生效。"
                color: ui.muted
                font.pixelSize: 11
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "最大输出 token 数"; color: ui.text; Layout.fillWidth: true; wrapMode: Text.Wrap }
                UiSpin { id: outputTokens; objectName: "outputTokensField"; from: 0; to: 131072; stepSize: 128; editable: true }
            }
            Label { text: "设为 0 使用服务默认上限。"; color: ui.muted; font.pixelSize: 11 }
            Label { text: "推理级别"; color: ui.muted; font.pixelSize: 12; Layout.topMargin: 4 }
            UiCombo {
                id: reasoning
                objectName: "reasoningField"
                Layout.fillWidth: true
                model: (root.descriptor.reasoningOptions || []).map(function(value) { return { id: value, name: root.reasoningLabel(value) } })
                textRole: "name"
                valueRole: "id"
            }
            Label { text: "不支持推理参数的模型请选择“服务默认”。"; color: ui.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
        UiSection {
            Layout.fillWidth: true
            title: "高级选项"
            description: "为代理服务或特定模型补充请求配置。"
            Label { text: "自定义请求头"; color: ui.muted; font.pixelSize: 12 }
            UiScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 96
                clip: true
                UiTextArea { id: customHeaders; objectName: "headersField"; text: "{}"; font.family: "monospace" }
            }
            Label { text: "额外请求参数"; color: ui.muted; font.pixelSize: 12; Layout.topMargin: 4 }
            UiScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 96
                clip: true
                UiTextArea { id: extraOptions; objectName: "optionsField"; text: "{}"; font.family: "monospace" }
            }
            Label { text: "填写 JSON 对象，例如 {\"top_p\": 0.9}。已有独立控件的字段无需重复填写。"; color: ui.muted; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
    }
}
