import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Tran.Core

ApplicationWindow {
    id: root
    required property TranslationController controller
    required property AppSettings appSettings
    required property DesktopBridge desktop
    required property ProviderTools providerTools
    readonly property bool compact: height < 400
    readonly property string displayProviderId: controller.providerId || appSettings.providerId
    readonly property string providerName: {
        const entry = appSettings.providers.find(function(p) { return p.id === root.displayProviderId })
        return entry ? entry.name : "翻译服务"
    }
    property string sourceLanguage: appSettings.snapshot().sourceLanguage
    property bool copied: false
    readonly property real sizeLimitWidth: Math.max(1, Math.min(720, desktop.popupAvailableSize.width * 0.9))
    readonly property real sizeLimitHeight: Math.max(1, Math.min(900, desktop.popupAvailableSize.height * 0.85))
    readonly property real preferredWidth: Math.ceil(Math.min(sizeLimitWidth,
        Math.max(360, sourceMeasure.implicitWidth + 66, translationMeasure.implicitWidth + 66)))
    readonly property real sourceContentHeight: Math.min(Math.max(22, sourceMeasure.implicitHeight),
        180, Math.max(40, sizeLimitHeight * 0.25))
    readonly property real resultContentHeight: controller.status === "success"
        ? Math.max(24, translationMeasure.implicitHeight) : 30 + messageMeasure.implicitHeight
    // Measure against the chosen width, independently of the current window size.
    // The two budgets include the header, cards, padding, language bar and footer.
    readonly property real compactContentHeight: 198 + sourceContentHeight + resultContentHeight
    readonly property size preferredSize: Qt.size(preferredWidth, Math.ceil(Math.min(sizeLimitHeight,
        Math.max(260, (compactContentHeight < 400 ? 198 : 278) + sourceContentHeight + resultContentHeight))))
    onPreferredSizeChanged: desktop.resizeTranslation(preferredSize)

    Text {
        id: sourceMeasure
        visible: false
        text: root.controller.sourceText
        font: sourceText.font
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        width: Math.max(1, root.preferredWidth - 66)
    }
    Text {
        id: translationMeasure
        visible: false
        text: root.controller.translatedText
        font: translationText.font
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        width: Math.max(1, root.preferredWidth - 66)
    }
    Text {
        id: messageMeasure
        visible: false
        text: stateMessage.text
        font: stateMessage.font
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        width: Math.max(1, root.preferredWidth - 66)
    }
    function languageName(id) {
        if (id === "auto") return "自动检测"
        const entry = appSettings.languages.find(function(l) { return l.id.toLowerCase() === id.toLowerCase() })
        return entry ? entry.name : id
    }
    UiTheme { id: ui }
    Connections {
        target: root.appSettings
        function onSettingsChanged() { root.sourceLanguage = root.appSettings.snapshot().sourceLanguage }
    }
    Connections {
        target: root.controller
        function onStateChanged() { root.copied = false }
    }
    Timer { id: copyTimer; interval: 1800; onTriggered: root.copied = false }

    title: "Tran · 选区翻译"
    width: 360
    height: 260
    minimumWidth: Math.min(360, sizeLimitWidth)
    minimumHeight: Math.min(260, sizeLimitHeight)
    visible: false
    onVisibleChanged: { if (visible) desktop.resizeTranslation(preferredSize) }
    color: ui.canvas
    font.pixelSize: 13
    palette.window: ui.canvas
    palette.windowText: ui.text
    palette.text: ui.text
    palette.buttonText: ui.text
    palette.base: ui.inset
    palette.button: ui.surface
    palette.highlight: ui.accent
    palette.highlightedText: ui.accentText
    palette.placeholderText: ui.muted
    flags: Qt.Window | (appSettings.stayOnTop ? Qt.WindowStaysOnTopHint : 0) | Qt.WindowCloseButtonHint | Qt.WindowTitleHint
    onClosing: function(close) {
        close.accepted = false
        desktop.closeTranslation()
    }

    header: Item {
        implicitHeight: 44
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 10
            spacing: 9
            UiIcon { name: "language"; color: ui.accent; implicitWidth: 22; implicitHeight: 22 }
            Label { text: "Tran"; color: ui.text; font.pixelSize: 16; font.weight: Font.DemiBold }
            Label { text: "划词翻译"; color: ui.muted; font.pixelSize: 12; Layout.fillWidth: true }
            UiButton {
                symbol: "settings"
                quiet: true
                hint: "打开设置"
                onClicked: settingsWindow.openPage(0)
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.compact ? 8 : 12
        anchors.topMargin: 2
        spacing: root.compact ? 6 : 10

        Pane {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(root.sourceContentHeight + (root.compact ? 16 : 46), root.height * 0.35)
            padding: root.compact ? 8 : 14
            background: Rectangle { color: ui.surface; radius: 11 }
            contentItem: ColumnLayout {
                spacing: 4
                RowLayout {
                    visible: !root.compact
                    Layout.fillWidth: true
                    Label { text: "原文"; color: ui.muted; font.pixelSize: 12; Layout.fillWidth: true }
                    Label {
                        text: root.controller.sourceText.length + " 字符"
                        visible: root.controller.sourceText.length > 0
                        color: ui.muted
                        font.pixelSize: 11
                    }
                }
                UiScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    UiTextArea {
                        id: sourceText
                        objectName: "sourceText"
                        text: root.controller.sourceText
                        readOnly: true
                        padding: 0
                        font.pixelSize: 15
                        placeholderText: "选中文字，按 " + (root.desktop.shortcutText || "翻译快捷键")
                        background: null
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.compact ? 34 : 40
            color: ui.hover
            radius: 9
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 5
                spacing: 10
                Label {
                    text: root.languageName(root.controller.detectedLanguage || root.controller.sourceLanguage || root.sourceLanguage)
                    color: ui.text
                    font.pixelSize: 13
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    elide: Text.ElideRight
                }
                UiIcon { name: "arrow"; color: ui.muted; implicitWidth: 18; implicitHeight: 18 }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    spacing: 4
                    Label {
                        text: root.languageName(root.controller.targetLanguage || root.appSettings.targetLanguage)
                        color: ui.text
                        font.pixelSize: 13
                        horizontalAlignment: Text.AlignRight
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    UiButton {
                        quiet: true
                        symbol: "down"
                        hint: "设置翻译语言"
                        implicitHeight: 30
                        implicitWidth: 30
                        onClicked: settingsWindow.openPage(1)
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 64
            color: ui.surface
            radius: 11
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: root.compact ? 32 : 42
                    color: ui.inset
                    radius: 11
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 11; color: ui.inset }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 8
                        spacing: 8
                        Rectangle {
                            implicitWidth: 22
                            implicitHeight: 22
                            radius: 6
                            color: root.displayProviderId === "openai" ? (ui.dark ? "#25433c" : "#e5f5ed") : ui.accentSoft
                            Label {
                                anchors.centerIn: parent
                                text: root.displayProviderId === "openai" ? "O" : "D"
                                color: root.displayProviderId === "openai" ? ui.success : ui.accent
                                font.pixelSize: 13
                                font.bold: true
                            }
                        }
                        Label { text: root.providerName; color: ui.text; font.weight: Font.DemiBold; Layout.fillWidth: true }
                        UiButton {
                            objectName: "retryTranslationButton"
                            symbol: "refresh"
                            hint: "重新翻译"
                            quiet: true
                            implicitHeight: 28
                            implicitWidth: 28
                            visible: !root.controller.busy && root.controller.sourceText.length > 0
                            onClicked: root.controller.retry()
                        }
                        UiButton {
                            objectName: "cancelTranslationButton"
                            symbol: "close"
                            hint: "取消翻译"
                            quiet: true
                            implicitHeight: 28
                            implicitWidth: 28
                            visible: root.controller.busy
                            onClicked: root.controller.cancel()
                        }
                    }
                }
                UiScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: root.compact ? 8 : 14
                    visible: root.controller.status === "success"
                    clip: true
                    UiTextArea {
                        id: translationText
                        objectName: "translationText"
                        text: root.controller.translatedText
                        readOnly: true
                        padding: 0
                        font.pixelSize: root.appSettings.fontSize
                        background: null
                    }
                }
                UiScrollView {
                    id: stateView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: root.compact ? 6 : 16
                    visible: root.controller.status !== "success"
                    contentWidth: availableWidth
                    clip: true
                    ColumnLayout {
                        width: stateView.availableWidth
                        spacing: 8
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 8
                            BusyIndicator {
                                running: root.controller.busy
                                visible: running
                                implicitWidth: 22
                                implicitHeight: 22
                            }
                            Label {
                                text: root.controller.busy ? "正在翻译…" : (root.controller.status === "idle" ? "等待选中文字" : (root.controller.status === "cancelled" ? "已取消翻译" : "暂时无法翻译"))
                                color: ui.text
                                font.pixelSize: root.compact ? 12 : 14
                                font.weight: Font.Medium
                            }
                        }
                        Label {
                            id: stateMessage
                            text: root.controller.busy ? "正在等待 " + root.providerName + " 返回结果" : (root.controller.message || "在任意应用中选中文字，按快捷键即可查看译文。")
                            textFormat: Text.PlainText
                            color: ui.muted
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 7
            Rectangle {
                implicitWidth: 31
                implicitHeight: 21
                radius: 4
                color: ui.inset
                border.color: ui.line
                Label { anchors.centerIn: parent; text: "Esc"; color: ui.muted; font.pixelSize: 11 }
            }
            Label { text: "关闭窗口"; color: ui.muted; font.pixelSize: 11; Layout.fillWidth: true }
            UiButton {
                objectName: "copyTranslationButton"
                text: root.copied ? "已复制" : "复制译文"
                symbol: root.copied ? "check" : "copy"
                highlighted: true
                implicitHeight: root.compact ? 28 : 34
                enabled: root.controller.status === "success"
                onClicked: {
                    root.desktop.copyTranslation()
                    root.copied = true
                    copyTimer.restart()
                }
            }
        }
    }

    SettingsWindow {
        id: settingsWindow
        objectName: "settingsWindow"
        transientParent: root
        appSettings: root.appSettings
        desktop: root.desktop
        providerTools: root.providerTools
    }
}
