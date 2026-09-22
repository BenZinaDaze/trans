import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trans.Core

UiScrollView {
    id: root
    required property DesktopBridge desktop
    contentWidth: availableWidth
    clip: true
    function load(values) {
        apiKey.text = values.ocrApiKey
        secretKey.text = values.ocrSecretKey
    }
    function write(values) {
        values.ocrApiKey = apiKey.text.trim()
        values.ocrSecretKey = secretKey.text.trim()
    }
    function forget() {
        apiKey.clear()
        secretKey.clear()
    }
    UiTheme { id: ui }
    ColumnLayout {
        width: root.availableWidth
        spacing: 16
        Label {
            text: root.desktop.capabilities.screenshots.reason
            visible: text.length > 0
            color: ui.muted
            font.pixelSize: 12
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
        UiSection {
            Layout.fillWidth: true
            title: "百度高精度文字识别"
            description: "框选截图后自动识别语言，再使用当前翻译服务翻译文字。"
            Label { text: "API Key"; color: ui.muted; font.pixelSize: 12 }
            UiField { id: apiKey; objectName: "ocrApiKeyField"; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "百度智能云应用的 API Key" }
            Label { text: "Secret Key"; color: ui.muted; font.pixelSize: 12 }
            UiField { id: secretKey; objectName: "ocrSecretKeyField"; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: "百度智能云应用的 Secret Key" }
            Label {
                Layout.fillWidth: true
                text: "请在百度智能云开通通用文字识别高精度版。OCR 与翻译分别按所选服务计费；未填写 OCR 密钥不影响选区翻译。"
                color: ui.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
        UiSection {
            Layout.fillWidth: true
            title: "截图与数据"
            description: "按住鼠标左键拖出矩形，松开后识别框内文字；Esc 或右键取消。"
            Label {
                Layout.fillWidth: true
                text: "只有框选区域将发送给百度，识别文字再发送给当前翻译服务。应用不保存截图历史。密钥以明文保存在本机受限权限配置文件中。请求超时沿用翻译偏好设置。"
                color: ui.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
    }
}
