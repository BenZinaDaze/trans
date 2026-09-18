#include "desktop.h"
#include "region_capture.h"

#include <KGlobalAccel>
#include <KWindowSystem>
#include <KX11Extras>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>

namespace Trans {

SelectionResult X11SelectionReader::read() const
{
    auto *clipboard = QGuiApplication::clipboard();
    if (QGuiApplication::platformName() != "xcb" || !clipboard->supportsSelection()) {
        return {{}, QStringLiteral("当前版本的选区翻译需要 X11 会话。请在登录时选择 Plasma (X11)。")};
    }
    return {clipboard->text(QClipboard::Selection), {}};
}

ShortcutService::ShortcutService(QObject *parent, const QString &actionId, const QString &label, const QString &defaultSequence)
    : QObject(parent), m_action(this), m_defaultSequence(defaultSequence)
{
    m_action.setObjectName(actionId);
    m_action.setText(label);
    connect(&m_action, &QAction::triggered, this, &ShortcutService::triggered);
}

void ShortcutService::initialize(const QString &requested)
{
    if (m_initialized) {
        apply(requested);
        return;
    }
    m_initialized = true;
    auto *accelerator = KGlobalAccel::self();
    connect(accelerator, &KGlobalAccel::globalShortcutChanged, this, [this, accelerator](QAction *action, const QKeySequence &) {
        if (action == &m_action) {
            const auto keys = accelerator->shortcut(&m_action);
            m_text = keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::PortableText);
            emit changed();
        }
    });
    const QList<QKeySequence> defaults{QKeySequence(m_defaultSequence)};
    accelerator->setDefaultShortcut(&m_action, defaults);
    accelerator->setShortcut(&m_action, defaults, KGlobalAccel::Autoloading);
    const auto keys = accelerator->shortcut(&m_action);
    m_text = keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::PortableText);
    apply(requested);
}

bool ShortcutService::apply(const QString &requested)
{
    m_error = validateShortcut(requested);
    if (!m_error.isEmpty()) {
        emit changed();
        return false;
    }
    const auto key = QKeySequence::fromString(requested, QKeySequence::PortableText);
    if (m_initialized) {
        auto *accelerator = KGlobalAccel::self();
        const auto previous = accelerator->shortcut(&m_action);
        // KDE's availability query also counts this action's active binding as occupied;
        // the component argument does not exclude shortcuts we already own.
        if (!key.isEmpty() && !previous.contains(key)
            && !KGlobalAccel::isGlobalShortcutAvailable(key, QCoreApplication::applicationName())) {
            m_error = QStringLiteral("此快捷键已被其他应用占用，请录制另一个组合。");
            emit changed();
            return false;
        }
        const QList<QKeySequence> desired = key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key};
        if (!accelerator->setShortcut(&m_action, desired, KGlobalAccel::NoAutoloading)
            || accelerator->shortcut(&m_action) != desired) {
            accelerator->setShortcut(&m_action, previous, KGlobalAccel::NoAutoloading);
            m_error = QStringLiteral("KDE 未能应用快捷键，请检查全局快捷键服务后重试。");
            emit changed();
            return false;
        }
    }
    m_text = key.toString(QKeySequence::PortableText);
    emit changed();
    return true;
}

QString ShortcutService::text() const
{
    return QKeySequence::fromString(m_text, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

QString ShortcutService::sequence() const { return m_text; }

QRect PopupPresenter::centeredGeometry(const QRect &available, QSize requested)
{
    if (available.isEmpty())
        return QRect(QPoint(0, 0), requested.expandedTo({1, 1}));
    const QSize size = requested.expandedTo({1, 1}).boundedTo(available.size());
    return QRect(QPoint(available.x() + (available.width() - size.width()) / 2,
                        available.y() + (available.height() - size.height()) / 2), size);
}

void PopupPresenter::rememberSource(QWindow *popup, QWindow *settings)
{
    if (QGuiApplication::platformName() != "xcb")
        return;
    const auto active = KX11Extras::activeWindow();
    if (active && (!popup || active != popup->winId()) && (!settings || active != settings->winId()))
        m_sourceWindow = active;
}

QRect PopupPresenter::cursorGeometry(const QRect &available, QSize requested, QPoint cursor)
{
    QRect geometry = centeredGeometry(available, requested);
    if (!available.isEmpty()) {
        geometry.moveLeft(qBound(available.left(), cursor.x() + 16, available.right() - geometry.width() + 1));
        geometry.moveTop(qBound(available.top(), cursor.y() + 20, available.bottom() - geometry.height() + 1));
    }
    return geometry;
}

QRect PopupPresenter::boundedGeometry(const QRect &available, QRect requested, QMargins frameMargins)
{
    if (available.isEmpty())
        return requested;
    const QSize frameSize(frameMargins.left() + frameMargins.right(), frameMargins.top() + frameMargins.bottom());
    requested.setSize(requested.size().expandedTo({1, 1}).boundedTo((available.size() - frameSize).expandedTo({1, 1})));
    auto outer = requested.marginsAdded(frameMargins);
    outer.moveLeft(qBound(available.left(), outer.left(), qMax(available.left(), available.right() - outer.width() + 1)));
    outer.moveTop(qBound(available.top(), outer.top(), qMax(available.top(), available.bottom() - outer.height() + 1)));
    return outer.marginsRemoved(frameMargins);
}

void PopupPresenter::show(QWindow *window, QSize requested, const QString &position)
{
    if (!window)
        return;
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        window->setScreen(screen);
        window->setGeometry(position == "cursor" ? cursorGeometry(screen->availableGeometry(), requested, QCursor::pos())
                                                  : centeredGeometry(screen->availableGeometry(), requested));
    }
    window->show();
    window->raise();
    if (QGuiApplication::platformName() == "xcb")
        KWindowSystem::activateWindow(window);
    else
        window->requestActivate();
}

void PopupPresenter::close(QWindow *window, QWindow *other, bool restoreFocus)
{
    if (!window)
        return;
    const bool wasActive = window->isActive();
    window->hide();
    if (!wasActive || !restoreFocus)
        return;
    if (other && other->isVisible()) {
        other->requestActivate();
    } else if (QGuiApplication::platformName() == "xcb" && m_sourceWindow
               && KX11Extras::hasWId(m_sourceWindow)) {
        KX11Extras::activateWindow(m_sourceWindow);
    }
}

DesktopBridge::DesktopBridge(TranslationController &controller, AppSettings &settings, QObject *parent, ShortcutService *shortcut, ShortcutService *screenshotShortcut)
    : QObject(parent), m_controller(controller), m_settings(settings),
      m_ownedShortcut(shortcut ? nullptr : std::make_unique<ShortcutService>()),
      m_shortcut(shortcut ? shortcut : m_ownedShortcut.get()),
      m_ownedScreenshotShortcut(screenshotShortcut ? nullptr : std::make_unique<ShortcutService>(nullptr,
          QStringLiteral("translate-screenshot"), QStringLiteral("截图翻译"), QStringLiteral("Meta+Shift+O"))),
      m_screenshotShortcut(screenshotShortcut ? screenshotShortcut : m_ownedScreenshotShortcut.get())
{
    connect(m_shortcut, &ShortcutService::triggered, this, &DesktopBridge::TranslateSelection);
    connect(m_shortcut, &ShortcutService::changed, this, &DesktopBridge::shortcutChanged);
    connect(m_screenshotShortcut, &ShortcutService::triggered, this, &DesktopBridge::TranslateScreenshot);
    connect(m_screenshotShortcut, &ShortcutService::changed, this, &DesktopBridge::shortcutChanged);
    connect(&m_controller, &TranslationController::captureFinished, this, [this](bool cancelled) {
        restoreCaptureWindows();
        if (!cancelled) showTranslationWindow();
    });
}

DesktopBridge::~DesktopBridge() = default;

void DesktopBridge::setWindows(QWindow *popup, QWindow *settings)
{
    if (m_popup) {
        m_popup->removeEventFilter(this);
        disconnect(m_popup, nullptr, this, nullptr);
    }
    m_popup = popup;
    m_settingsWindow = settings;
    if (m_popup) {
        m_popup->installEventFilter(this);
        connect(m_popup, &QWindow::screenChanged, this, &DesktopBridge::watchPopupScreen);
    }
    watchPopupScreen();
}

QSize DesktopBridge::popupAvailableSize() const
{
    const auto *screen = m_popup && m_popup->screen() ? m_popup->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return {1024, 768};
    const auto margins = m_popup ? m_popup->frameMargins() : QMargins();
    return (screen->availableGeometry().size()
        - QSize(margins.left() + margins.right(), margins.top() + margins.bottom())).expandedTo({1, 1});
}

void DesktopBridge::watchPopupScreen()
{
    disconnect(m_screenGeometryConnection);
    if (m_popup && m_popup->screen()) {
        m_screenGeometryConnection = connect(m_popup->screen(), &QScreen::availableGeometryChanged, this, [this] {
            m_reportedPopupAvailableSize = popupAvailableSize();
            emit popupAvailableSizeChanged();
            resizeTranslation(m_preferredPopupSize);
        });
    }
    m_reportedPopupAvailableSize = popupAvailableSize();
    emit popupAvailableSizeChanged();
    resizeTranslation(m_preferredPopupSize);
}

void DesktopBridge::resizeTranslation(QSize preferred)
{
    m_preferredPopupSize = preferred.expandedTo({1, 1});
    if (!m_popup || !m_popup->isVisible() || m_resizePending)
        return;
    m_resizePending = true;
    // Text and layout bindings settle together before applying a single geometry change.
    QTimer::singleShot(0, this, [this] {
        m_resizePending = false;
        if (!m_popup || !m_popup->isVisible() || !m_popup->screen())
            return;
        auto desired = QRect(m_popup->position(), m_preferredPopupSize);
        if (m_settings.popupPosition() == "screen")
            desired.moveCenter(m_popup->geometry().center());
        desired = PopupPresenter::boundedGeometry(m_popup->screen()->availableGeometry(), desired, m_popup->frameMargins());
        if (desired != m_popup->geometry())
            m_popup->setGeometry(desired);
    });
}

bool DesktopBridge::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_popup && m_popup && m_popup->isVisible() && event->type() == QEvent::Expose) {
        // After mapping, KWin knows the frame margins and accepts native state changes reliably.
        if (QGuiApplication::platformName() == "xcb")
            KX11Extras::setState(m_popup->winId(), NET::SkipTaskbar | NET::SkipPager);
        if (popupAvailableSize() != m_reportedPopupAvailableSize) {
            m_reportedPopupAvailableSize = popupAvailableSize();
            emit popupAvailableSizeChanged();
            resizeTranslation(m_preferredPopupSize);
        }
    }
    if (watched == m_popup && m_popup && m_popup->isVisible()
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            // Claim the window's Escape before QML shortcuts or focused text controls.
            // No global Escape registration: other windows keep their own key behavior.
            key->accept();
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
                closeTranslation();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

void DesktopBridge::initialize()
{
    m_shortcut->initialize(m_settings.shortcut());
    m_screenshotShortcut->initialize(m_settings.snapshot().value("screenshotShortcut").toString());
    m_menu = std::make_unique<QMenu>();
    auto *openAction = m_menu->addAction(QStringLiteral("打开翻译窗口"), this, &DesktopBridge::ShowTranslation);
    m_menu->setDefaultAction(openAction);
    m_menu->addAction(QStringLiteral("选区翻译"), this, &DesktopBridge::TranslateSelection);
    m_menu->addAction(QStringLiteral("截图翻译"), this, &DesktopBridge::TranslateScreenshot);
    m_menu->addAction(QStringLiteral("设置…"), this, &DesktopBridge::ShowSettings);
    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("退出 Trans"), qApp, &QCoreApplication::quit);
    m_tray = std::make_unique<QSystemTrayIcon>(QGuiApplication::windowIcon(), this);
    m_tray->setToolTip(QStringLiteral("Trans · 点击查看翻译"));
    m_tray->setContextMenu(m_menu.get());
    connect(m_tray.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            ShowTranslation();
    });
    m_tray->show();
    if (!shortcutError().isEmpty())
        m_tray->showMessage(QStringLiteral("Trans"), shortcutError(), QSystemTrayIcon::Warning);
}

void DesktopBridge::showTranslationWindow()
{
    if (!m_popup)
        return;
    if (auto *screen = QGuiApplication::screenAt(QCursor::pos()))
        m_popup->setScreen(screen);
    m_presenter.show(m_popup, m_preferredPopupSize, m_settings.popupPosition());
    emit popupAvailableSizeChanged();
    resizeTranslation(m_preferredPopupSize);
    // Keep the tray-style popup out of task/pager lists without Qt::Tool's group-transient
    // relationship, which can put it above its own settings dialog on KDE.
    if (m_popup && QGuiApplication::platformName() == "xcb")
        KX11Extras::setState(m_popup->winId(), NET::SkipTaskbar | NET::SkipPager);
}

void DesktopBridge::ShowTranslation()
{
    if (m_controller.status() == "capturing") return;
    // Reopening is a view operation: keep the current result (or request state),
    // without reading PRIMARY or issuing another paid translation request.
    m_presenter.rememberSource(m_popup, m_settingsWindow);
    showTranslationWindow();
}

void DesktopBridge::TranslateSelection()
{
    if (!m_captureHidden) m_presenter.rememberSource(m_popup, m_settingsWindow);
    // Read PRIMARY before showing a window or changing keyboard focus.
    const auto selection = m_selection.read();
    restoreCaptureWindows();
    if (selection.error.isEmpty())
        m_controller.translateText(selection.text);
    else
        m_controller.selectionError(selection.error);
    showTranslationWindow();
}

void DesktopBridge::restoreCaptureWindows()
{
    if (!m_captureHidden) return;
    if (m_popupWasVisible && m_popup) m_popup->show();
    if (m_settingsWasVisible && m_settingsWindow) m_settingsWindow->show();
    m_captureHidden = false;
    emit captureVisibilityChanged();
}

void DesktopBridge::TranslateScreenshot()
{
    const auto config = m_settings.snapshot();
    if (config.value("ocrApiKey").toString().trimmed().isEmpty() || config.value("ocrSecretKey").toString().trimmed().isEmpty()) {
        restoreCaptureWindows();
        m_controller.selectionError(QStringLiteral("请在设置 → 截图 OCR 中填写百度 API Key 和 Secret Key。"));
        showTranslationWindow();
        return;
    }
    if (!m_captureHidden) {
        m_presenter.rememberSource(m_popup, m_settingsWindow);
        m_popupWasVisible = m_popup && m_popup->isVisible();
        m_settingsWasVisible = m_settingsWindow && m_settingsWindow->isVisible();
        m_captureHidden = true;
        emit captureVisibilityChanged();
    }
    if (m_popup) m_popup->hide();
    if (m_settingsWindow) m_settingsWindow->hide();
    m_controller.translateScreenshot(createScreenshotJob(&m_controller));
}

void DesktopBridge::ShowSettings()
{
    if (m_controller.status() == "capturing") m_controller.cancel();
    m_presenter.rememberSource(m_popup, m_settingsWindow);
    if (m_settingsWindow)
        m_presenter.show(m_settingsWindow, m_settingsWindow->size());
}

void DesktopBridge::closeTranslation()
{
    m_controller.cancel();
    m_presenter.close(m_popup, m_settingsWindow, m_settings.restoreFocus());
}

void DesktopBridge::closeSettings() { m_presenter.close(m_settingsWindow, m_popup, m_settings.restoreFocus()); }

void DesktopBridge::copyTranslation()
{
    if (!m_controller.translatedText().isEmpty())
        QGuiApplication::clipboard()->setText(m_controller.translatedText(), QClipboard::Clipboard);
}

bool DesktopBridge::saveSettings(const QVariantMap &values)
{
    m_settingsError = m_settings.validate(values);
    if (!m_settingsError.isEmpty()) {
        emit settingsErrorChanged();
        return false;
    }
    const QString previous = m_shortcut->sequence();
    const QString previousScreenshot = m_screenshotShortcut->sequence();
    const QString desired = values.value("shortcut").toString();
    const QString desiredScreenshot = values.value("screenshotShortcut").toString();
    const bool changed = desired != previous;
    const bool screenshotChanged = desiredScreenshot != previousScreenshot;
    auto rollback = [&] {
        // Release both actions before restoring, allowing the user to swap bindings.
        if (changed) m_shortcut->apply({});
        if (screenshotChanged) m_screenshotShortcut->apply({});
        if (changed) m_shortcut->apply(previous);
        if (screenshotChanged) m_screenshotShortcut->apply(previousScreenshot);
    };
    bool applied = true;
    if (changed) applied = m_shortcut->apply({});
    if (applied && screenshotChanged) applied = m_screenshotShortcut->apply({});
    if (applied && changed) applied = m_shortcut->apply(desired);
    if (applied && screenshotChanged) applied = m_screenshotShortcut->apply(desiredScreenshot);
    if (!applied) {
        m_settingsError = shortcutError().isEmpty() ? QStringLiteral("快捷键应用失败，设置未保存。") : shortcutError();
        rollback();
        emit settingsErrorChanged();
        return false;
    }
    if (!m_settings.save(values)) {
        rollback();
        m_settingsError = m_settings.lastError();
        emit settingsErrorChanged();
        return false;
    }
    emit settingsErrorChanged();
    return true;
}

QString DesktopBridge::shortcutForKey(int key, int modifiers) const
{
    const auto sequence = QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers)
        & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier), static_cast<Qt::Key>(key)))
        .toString(QKeySequence::PortableText);
    return validateShortcut(sequence).isEmpty() ? sequence : QString();
}

} // namespace Trans
