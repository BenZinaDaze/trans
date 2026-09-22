#pragma once

#include "controller.h"
#include "platform/platform_services.h"
#include <QPointer>
#include <QQueue>
#include <QRect>
#include <QWindow>
#include <array>

class QSystemTrayIcon;
class QMenu;

namespace Trans {

enum class AppCommand;

class PopupPresenter {
public:
    explicit PopupPresenter(WindowIntegration &windows) : m_windows(windows) {}
    static QRect centeredGeometry(const QRect &available, QSize requested);
    static QRect cursorGeometry(const QRect &available, QSize requested, QPoint cursor);
    static QRect boundedGeometry(const QRect &available, QRect requested, QMargins frameMargins);
    void rememberSource(QWindow *popup, QWindow *settings);
    void setSource(SourceContextPtr source);
    SourceContextPtr source() const { return m_source; }
    void show(QWindow *window, QSize requested, const QString &position = QStringLiteral("screen"));
    void close(QWindow *window, QWindow *other, bool restoreFocus = true);
private:
    WindowIntegration &m_windows;
    SourceContextPtr m_source;
};

class DesktopBridge final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(bool captureActive READ captureActive NOTIFY captureVisibilityChanged)
    Q_PROPERTY(QString shortcutText READ shortcutText NOTIFY shortcutChanged)
    Q_PROPERTY(QString shortcutError READ shortcutError NOTIFY shortcutChanged)
    Q_PROPERTY(QString settingsError READ settingsError NOTIFY settingsErrorChanged)
    Q_PROPERTY(bool settingsBusy READ settingsBusy NOTIFY settingsBusyChanged)
    Q_PROPERTY(QVariantMap capabilities READ capabilities NOTIFY capabilitiesChanged)
    Q_PROPERTY(QSize popupAvailableSize READ popupAvailableSize NOTIFY popupAvailableSizeChanged)
public:
    DesktopBridge(TranslationController &controller, AppSettings &settings, PlatformServices &platform, QObject *parent = nullptr);
    ~DesktopBridge() override;
    void setWindows(QWindow *popup, QWindow *settings);
    void initialize();
    void dispatchCommand(AppCommand command);
    bool captureActive() const { return m_captureHidden; }
    QString shortcutText() const { return m_platform.shortcuts().text(ShortcutAction::Selection); }
    QString shortcutError() const;
    QString settingsError() const { return m_settingsError; }
    bool settingsBusy() const { return m_settingsBusy; }
    QVariantMap capabilities() const;
    QSize popupAvailableSize() const;
    Q_INVOKABLE void resizeTranslation(QSize preferred);
    Q_INVOKABLE void closeTranslation();
    Q_INVOKABLE void closeSettings();
    Q_INVOKABLE void copyTranslation();
    Q_INVOKABLE void saveSettings(const QVariantMap &values);
    Q_INVOKABLE QString shortcutForKey(int key, int modifiers) const;
    Q_INVOKABLE void ShowTranslation();
    Q_INVOKABLE void TranslateSelection();
    Q_INVOKABLE void TranslateScreenshot();
    Q_INVOKABLE void ShowSettings();
signals:
    void captureVisibilityChanged();
    void shortcutChanged();
    void settingsErrorChanged();
    void settingsBusyChanged();
    void settingsSaveFinished(bool success);
    void capabilitiesChanged();
    void popupAvailableSizeChanged();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    struct SettingsRequest { QVariantMap values; bool persist; };
    struct ShortcutUpdate { ShortcutAction action; QString sequence; };
    void showTranslationWindow();
    void watchPopupScreen();
    void restoreCaptureWindows();
    void startSettingsRequest();
    void runShortcutUpdate();
    void rollbackSettings(const QString &error);
    void finishSettingsRequest(bool success);
    TranslationController &m_controller;
    AppSettings &m_settings;
    PlatformServices &m_platform;
    bool m_captureHidden = false;
    bool m_startingSelection = false;
    bool m_popupWasVisible = false;
    bool m_settingsWasVisible = false;
    bool m_initialized = false;
    QString m_settingsError;
    QString m_shortcutError;
    bool m_settingsBusy = false;
    QQueue<SettingsRequest> m_settingsQueue;
    SettingsRequest m_settingsRequest;
    std::array<QString, 2> m_previousShortcuts;
    std::array<bool, 2> m_changedShortcuts{};
    QList<ShortcutUpdate> m_shortcutUpdates;
    qsizetype m_shortcutUpdateIndex = 0;
    bool m_rollingBack = false;
    QStringList m_rollbackErrors;
    QPointer<ShortcutJob> m_shortcutJob;
    PopupPresenter m_presenter;
    QPointer<QWindow> m_popup;
    QPointer<QWindow> m_settingsWindow;
    QSize m_preferredPopupSize{480, 360};
    QSize m_reportedPopupAvailableSize;
    bool m_resizePending = false;
    QMetaObject::Connection m_screenGeometryConnection;
    std::unique_ptr<QMenu> m_menu;
    std::unique_ptr<QSystemTrayIcon> m_tray;
};

} // namespace Trans
