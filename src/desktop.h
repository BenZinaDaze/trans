#pragma once

#include "controller.h"
#include <QAction>
#include <QPointer>
#include <QRect>
#include <QWindow>

class QSystemTrayIcon;
class QMenu;

namespace Tran {

struct SelectionResult { QString text; QString error; };

class SelectionReader {
public:
    virtual ~SelectionReader() = default;
    virtual SelectionResult read() const = 0;
};

class X11SelectionReader final : public SelectionReader {
public:
    SelectionResult read() const override;
};

class ShortcutService : public QObject {
    Q_OBJECT
public:
    explicit ShortcutService(QObject *parent = nullptr);
    virtual void initialize(const QString &sequence);
    virtual bool apply(const QString &sequence);
    virtual QString sequence() const;
    QString text() const;
    QString error() const { return m_error; }
signals:
    void triggered();
    void changed();
private:
    QAction m_action;
    QString m_text;
    QString m_error;
    bool m_initialized = false;
};

class PopupPresenter {
public:
    static QRect centeredGeometry(const QRect &available, QSize requested);
    static QRect cursorGeometry(const QRect &available, QSize requested, QPoint cursor);
    static QRect boundedGeometry(const QRect &available, QRect requested, QMargins frameMargins);
    void rememberSource(QWindow *popup, QWindow *settings);
    void show(QWindow *window, QSize requested, const QString &position = QStringLiteral("screen"));
    void close(QWindow *window, QWindow *other, bool restoreFocus = true);
private:
    WId m_sourceWindow = 0;
};

class DesktopBridge final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.tran.Tran")
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(QString shortcutText READ shortcutText NOTIFY shortcutChanged)
    Q_PROPERTY(QString shortcutError READ shortcutError NOTIFY shortcutChanged)
    Q_PROPERTY(QString settingsError READ settingsError NOTIFY settingsErrorChanged)
    Q_PROPERTY(QSize popupAvailableSize READ popupAvailableSize NOTIFY popupAvailableSizeChanged)
public:
    DesktopBridge(TranslationController &controller, AppSettings &settings, QObject *parent = nullptr,
                  ShortcutService *shortcut = nullptr);
    ~DesktopBridge() override;
    void setWindows(QWindow *popup, QWindow *settings);
    void initialize();
    QString shortcutText() const { return m_shortcut->text(); }
    QString shortcutError() const { return m_shortcut->error(); }
    QString settingsError() const { return m_settingsError; }
    QSize popupAvailableSize() const;
    Q_INVOKABLE void resizeTranslation(QSize preferred);
    Q_INVOKABLE void closeTranslation();
    Q_INVOKABLE void closeSettings();
    Q_INVOKABLE void copyTranslation();
    Q_INVOKABLE bool saveSettings(const QVariantMap &values);
    Q_INVOKABLE QString shortcutForKey(int key, int modifiers) const;
    Q_SCRIPTABLE Q_INVOKABLE void ShowTranslation();
    Q_SCRIPTABLE Q_INVOKABLE void TranslateSelection();
    Q_SCRIPTABLE Q_INVOKABLE void ShowSettings();
signals:
    void shortcutChanged();
    void settingsErrorChanged();
    void popupAvailableSizeChanged();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void showTranslationWindow();
    void watchPopupScreen();
    TranslationController &m_controller;
    AppSettings &m_settings;
    X11SelectionReader m_selection;
    std::unique_ptr<ShortcutService> m_ownedShortcut;
    ShortcutService *m_shortcut;
    QString m_settingsError;
    PopupPresenter m_presenter;
    QPointer<QWindow> m_popup;
    QPointer<QWindow> m_settingsWindow;
    QSize m_preferredPopupSize{360, 300};
    QSize m_reportedPopupAvailableSize;
    bool m_resizePending = false;
    QMetaObject::Connection m_screenGeometryConnection;
    std::unique_ptr<QMenu> m_menu;
    std::unique_ptr<QSystemTrayIcon> m_tray;
};

} // namespace Tran
