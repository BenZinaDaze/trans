pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trans.Core

ApplicationWindow {
    id: root
    required property AppSettings appSettings
    required property DesktopBridge desktop
    required property ProviderTools providerTools
    property var draft: ({})
    property string feedback: ""
    property bool saved: false

    title: "Trans · 配置中心"
    width: 940
    height: 740
    minimumWidth: 500
    minimumHeight: 420
    visible: false
    // The window manager keeps this dialog above its translation parent, including when pinned.
    flags: Qt.Dialog | Qt.WindowTitleHint | Qt.WindowCloseButtonHint

    function reload() {
        draft = appSettings.snapshot()
        providerPage.load(draft.providerConfigs, draft.providerId)
        translationPage.load(draft)
        desktopPage.load(draft)
        ocrPage.load(draft)
        saved = false
        feedback = appSettings.lastError
    }
    function collect() {
        providerPage.flush()
        draft.providerConfigs = providerPage.configs
        draft.providerId = providerPage.editingProvider
        translationPage.write(draft)
        desktopPage.write(draft)
        ocrPage.write(draft)
        return draft
    }
    function saveAll() {
        if (desktop.settingsBusy) return
        saved = false
        feedback = ""
        desktop.saveSettings(collect())
    }
    Connections {
        target: root.desktop
        function onSettingsSaveFinished(success) {
            if (success && root.visible) root.reload()
            root.saved = success
            root.feedback = success ? "所有设置已保存。下一次翻译将使用新配置。" : root.desktop.settingsError
        }
    }
    onVisibleChanged: {
        if (desktop.captureActive) return
        if (visible) {
            reload()
        } else {
            providerTools.clear()
            providerPage.forget()
            ocrPage.forget()
            draft = ({})
            desktopPage.recording = false
            desktopPage.recordingScreenshot = false
        }
    }
    onClosing: function(close) {
        close.accepted = false
        desktop.closeSettings()
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: root.visible && !desktopPage.recording && !desktopPage.recordingScreenshot
        onActivated: root.desktop.closeSettings()
    }

    UiTheme { id: ui }
    readonly property bool narrow: width < 720
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

    function openPage(index) {
        desktop.ShowSettings()
        tabs.currentIndex = index
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.preferredWidth: root.narrow ? 64 : 184
            Layout.fillHeight: true
            color: ui.surface
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: ui.line }
            ColumnLayout {
                id: tabs
                objectName: "settingsTabs"
                property int currentIndex: 0
                anchors.fill: parent
                anchors.margins: root.narrow ? 10 : 16
                spacing: 8
                RowLayout {
                    Layout.topMargin: 12
                    Layout.bottomMargin: 28
                    Layout.alignment: root.narrow ? Qt.AlignHCenter : Qt.AlignLeft
                    spacing: 10
                    Rectangle {
                        implicitWidth: 32
                        implicitHeight: 32
                        radius: 9
                        color: ui.accentSoft
                        UiIcon { anchors.centerIn: parent; name: "language"; color: ui.accent; implicitWidth: 22; implicitHeight: 22 }
                    }
                    Label { visible: !root.narrow; text: "Trans"; color: ui.text; font.pixelSize: 23; font.weight: Font.DemiBold }
                }
                Label {
                    visible: !root.narrow
                    text: "偏好设置"
                    color: ui.muted
                    font.pixelSize: 11
                    Layout.leftMargin: 10
                    Layout.bottomMargin: 4
                }
                Repeater {
                    model: [ { name: "翻译服务", icon: "service" }, { name: "翻译偏好", icon: "language" }, { name: "快捷键与窗口", icon: "window" }, { name: "截图 OCR", icon: "window" } ]
                    delegate: Button {
                        id: navButton
                        required property var modelData
                        required property int index
                        objectName: "settingsNav" + index
                        Layout.fillWidth: true
                        implicitHeight: 42
                        leftPadding: 11
                        rightPadding: 11
                        hoverEnabled: true
                        Accessible.name: modelData.name
                        ToolTip.visible: hovered && root.narrow
                        ToolTip.text: modelData.name
                        ToolTip.delay: 500
                        onClicked: tabs.currentIndex = index
                        background: Rectangle {
                            radius: 8
                            color: tabs.currentIndex === navButton.index ? ui.accentSoft : (navButton.hovered ? ui.inset : "transparent")
                            border.color: navButton.activeFocus ? ui.accent : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 10
                            UiIcon { name: navButton.modelData.icon; color: tabs.currentIndex === navButton.index ? ui.accent : ui.muted }
                            Label {
                                visible: !root.narrow
                                text: navButton.modelData.name
                                color: tabs.currentIndex === navButton.index ? ui.accent : ui.text
                                font.pixelSize: 13
                                font.weight: tabs.currentIndex === navButton.index ? Font.DemiBold : Font.Normal
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Label { visible: !root.narrow; text: "Trans  /  " + Qt.application.version; color: ui.muted; font.pixelSize: 11; Layout.leftMargin: 10; Layout.bottomMargin: 6 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: root.narrow ? 16 : 26
                Layout.bottomMargin: 20
                spacing: 6
                Label {
                    text: ["翻译服务", "翻译偏好", "快捷键与窗口", "截图 OCR"][tabs.currentIndex]
                    color: ui.text
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                }
                Label {
                    text: ["连接常用模型，选择适合你的翻译服务。", "设置语言、提示词与翻译请求偏好。", "让翻译融入你的桌面工作方式。", "识别图片文字，自动翻译截图内容。"][tabs.currentIndex]
                    color: ui.muted
                    font.pixelSize: 12
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }
            }
            StackLayout {
                enabled: !root.desktop.settingsBusy
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: root.narrow ? 16 : 26
                Layout.rightMargin: root.narrow ? 16 : 26
                Layout.bottomMargin: 16
                currentIndex: tabs.currentIndex
                ProviderPage {
                    id: providerPage
                    objectName: "providerPage"
                    appSettings: root.appSettings
                    providerTools: root.providerTools
                    onFetchRequested: function(id) { root.providerTools.fetchModels(id, root.collect()) }
                    onTestRequested: function(id) { root.providerTools.testTranslation(id, root.collect()) }
                }
                TranslationPage {
                    id: translationPage
                    objectName: "translationPage"
                    appSettings: root.appSettings
                }
                DesktopPage {
                    id: desktopPage
                    objectName: "desktopPage"
                    desktop: root.desktop
                    appSettings: root.appSettings
                }
                OcrPage { id: ocrPage; objectName: "ocrPage"; desktop: root.desktop }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: ui.line }
            Pane {
                Layout.fillWidth: true
                padding: root.narrow ? 12 : 18
                background: Rectangle { color: ui.surface }
                contentItem: ColumnLayout {
                    spacing: 10
                    Label {
                        text: root.feedback
                        objectName: "saveFeedback"
                        visible: text.length > 0
                        color: root.saved ? ui.success : ui.danger
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: "设置与密钥仅保存在本机"
                            visible: !root.narrow
                            color: ui.muted
                            font.pixelSize: 11
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                        Item { visible: root.narrow; Layout.fillWidth: true }
                        UiButton { text: "放弃修改"; quiet: true; enabled: !root.desktop.settingsBusy; onClicked: root.reload() }
                        UiButton {
                            text: root.desktop.settingsBusy ? "正在保存…" : "保存全部"
                            symbol: "check"
                            objectName: "saveAllButton"
                            highlighted: true
                            enabled: !root.desktop.settingsBusy && !desktopPage.recording && !desktopPage.recordingScreenshot
                            onClicked: root.saveAll()
                        }
                    }
                }
            }
        }
    }
}
