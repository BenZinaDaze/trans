#include "desktop.h"
#include "platform/instance_channel.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>

namespace Trans {

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
    setSource(m_windows.captureSource(popup, settings));
}

void PopupPresenter::setSource(SourceContextPtr source)
{
    if (source) m_source = std::move(source);
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
    m_windows.activate(window);
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
        m_windows.activate(other);
    } else {
        m_windows.restoreSource(m_source);
    }
}

DesktopBridge::DesktopBridge(TranslationController &controller, AppSettings &settings, PlatformServices &platform, QObject *parent)
    : QObject(parent), m_controller(controller), m_settings(settings), m_platform(platform), m_presenter(platform.windows())
{
    connect(&platform.shortcuts(), &ShortcutService::triggered, this, [this](ShortcutAction action) {
        if (action == ShortcutAction::Selection) TranslateSelection();
        else TranslateScreenshot();
    });
    connect(&platform.shortcuts(), &ShortcutService::changed, this, &DesktopBridge::shortcutChanged);
    connect(&platform.shortcuts(), &ShortcutService::capabilityChanged, this, &DesktopBridge::capabilitiesChanged);
    connect(&platform.shortcuts(), &ShortcutService::capabilityChanged, this, &DesktopBridge::shortcutChanged);
    connect(&platform.selection(), &SelectionReader::capabilityChanged, this, &DesktopBridge::capabilitiesChanged);
    connect(&platform.screenshots(), &ScreenshotService::capabilityChanged, this, &DesktopBridge::capabilitiesChanged);
    connect(&platform.windows(), &WindowIntegration::capabilityChanged, this, &DesktopBridge::capabilitiesChanged);
    connect(&m_controller, &TranslationController::selectionFinished, this, [this](const SourceContextPtr &source, bool cancelled) {
        restoreCaptureWindows();
        if (cancelled) return;
        m_presenter.setSource(source);
        showTranslationWindow();
    });
    connect(&m_controller, &TranslationController::captureFinished, this, [this](bool cancelled) {
        if (!m_startingSelection) restoreCaptureWindows();
        if (!cancelled) showTranslationWindow();
    });
}

DesktopBridge::~DesktopBridge()
{
    disconnect(&m_controller, nullptr, this, nullptr);
    m_controller.cancel();
    if (m_shortcutJob) {
        disconnect(m_shortcutJob, nullptr, this, nullptr);
        m_shortcutJob->cancel();
    }
}

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
        // Native mapping hints are applied after the window manager knows the frame.
        m_platform.windows().popupMapped(m_popup);
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
    if (m_initialized) return;
    m_initialized = true;
    m_settingsQueue.enqueue({m_settings.snapshot(), false});
    if (!m_settingsBusy) {
        m_settingsBusy = true;
        emit settingsBusyChanged();
        QTimer::singleShot(0, this, &DesktopBridge::startSettingsRequest);
    }
    m_menu = std::make_unique<QMenu>();
    auto *openAction = m_menu->addAction(QStringLiteral("打开翻译窗口"), this, &DesktopBridge::ShowTranslation);
    m_menu->setDefaultAction(openAction);
    auto *selectionAction = m_menu->addAction(QStringLiteral("选区翻译"), this, &DesktopBridge::TranslateSelection);
    auto *screenshotAction = m_menu->addAction(QStringLiteral("截图翻译"), this, &DesktopBridge::TranslateScreenshot);
    auto updateActions = [this, selectionAction, screenshotAction] {
        selectionAction->setEnabled(m_platform.selection().availability() == CapabilityState::Available);
        selectionAction->setToolTip(m_platform.selection().unavailableReason());
        screenshotAction->setEnabled(m_platform.screenshots().availability() == CapabilityState::Available);
        screenshotAction->setToolTip(m_platform.screenshots().unavailableReason());
    };
    connect(this, &DesktopBridge::capabilitiesChanged, m_menu.get(), updateActions);
    updateActions();
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
    m_platform.windows().popupMapped(m_popup);
}

void DesktopBridge::ShowTranslation()
{
    if (m_controller.status() == "capturing" || m_controller.status() == "selecting") return;
    // Reopening is a view operation: keep the current result (or request state),
    // without reading PRIMARY or issuing another paid translation request.
    m_presenter.rememberSource(m_popup, m_settingsWindow);
    showTranslationWindow();
}

void DesktopBridge::TranslateSelection()
{
    if (!m_captureHidden) m_presenter.rememberSource(m_popup, m_settingsWindow);
    // Cancel old capture/OCR/translation before starting the selection job, but
    // never show or activate a window until the asynchronous read has finished.
    m_startingSelection = true;
    m_controller.cancel();
    m_startingSelection = false;
    m_controller.translateSelection(m_platform.selection().read(m_presenter.source(), &m_controller));
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
    m_controller.cancel();
    if (m_platform.screenshots().availability() != CapabilityState::Available) {
        restoreCaptureWindows();
        m_controller.selectionError(m_platform.screenshots().unavailableReason());
        showTranslationWindow();
        return;
    }
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
    m_controller.translateScreenshot(m_platform.screenshots().captureRegion(&m_controller));
}

void DesktopBridge::ShowSettings()
{
    if (m_controller.status() == "capturing" || m_controller.status() == "selecting") m_controller.cancel();
    restoreCaptureWindows();
    m_presenter.rememberSource(m_popup, m_settingsWindow);
    if (m_settingsWindow)
        m_presenter.show(m_settingsWindow, m_settingsWindow->size());
}

void DesktopBridge::closeTranslation()
{
    m_controller.cancel();
    restoreCaptureWindows();
    m_presenter.close(m_popup, m_settingsWindow, m_settings.restoreFocus());
}

void DesktopBridge::closeSettings()
{
    if (m_controller.status() == "selecting") m_controller.cancel();
    restoreCaptureWindows();
    m_presenter.close(m_settingsWindow, m_popup, m_settings.restoreFocus());
}

void DesktopBridge::copyTranslation()
{
    if (!m_controller.translatedText().isEmpty())
        QGuiApplication::clipboard()->setText(m_controller.translatedText(), QClipboard::Clipboard);
}


QString DesktopBridge::shortcutError() const
{
    return m_shortcutError.isEmpty() ? m_platform.shortcuts().unavailableReason() : m_shortcutError;
}

QVariantMap DesktopBridge::capabilities() const
{
    auto capability = [](CapabilityState state, const QString &reason) {
        QString name;
        switch (state) {
        case CapabilityState::Available: name = QStringLiteral("available"); break;
        case CapabilityState::Unavailable: name = QStringLiteral("unavailable"); break;
        case CapabilityState::Unsupported: name = QStringLiteral("unsupported"); break;
        case CapabilityState::PermissionDenied: name = QStringLiteral("permissionDenied"); break;
        }
        return QVariantMap{{"state", name}, {"available", state == CapabilityState::Available}, {"reason", reason}};
    };
    return {{"selection", capability(m_platform.selection().availability(), m_platform.selection().unavailableReason())},
            {"screenshots", capability(m_platform.screenshots().availability(), m_platform.screenshots().unavailableReason())},
            {"shortcuts", capability(m_platform.shortcuts().availability(), m_platform.shortcuts().unavailableReason())},
            {"focusRestoration", capability(m_platform.windows().focusRestoration(), m_platform.windows().focusRestorationReason())}};
}

void DesktopBridge::dispatchCommand(AppCommand command)
{
    switch (command) {
    case AppCommand::ShowTranslation: ShowTranslation(); break;
    case AppCommand::TranslateSelection: TranslateSelection(); break;
    case AppCommand::TranslateScreenshot: TranslateScreenshot(); break;
    case AppCommand::ShowSettings: ShowSettings(); break;
    }
}

void DesktopBridge::saveSettings(const QVariantMap &values)
{
    // Copy each submitted draft. A second caller cannot interleave registration
    // or persistence with an in-flight transaction, including its rollback.
    m_settingsQueue.enqueue({values, true});
    if (m_settingsBusy) return;
    m_settingsBusy = true;
    emit settingsBusyChanged();
    QTimer::singleShot(0, this, &DesktopBridge::startSettingsRequest);
}

void DesktopBridge::startSettingsRequest()
{
    m_settingsRequest = m_settingsQueue.dequeue();
    m_settingsError.clear();
    m_rollbackErrors.clear();
    m_rollingBack = false;
    m_shortcutUpdates.clear();
    m_shortcutUpdateIndex = 0;
    if (m_settingsRequest.persist) {
        m_settingsError = m_settings.validate(m_settingsRequest.values);
        if (!m_settingsError.isEmpty()) {
            finishSettingsRequest(false);
            return;
        }
    }
    auto &shortcuts = m_platform.shortcuts();
    const auto currentSettings = m_settings.snapshot();
    const std::array<ShortcutAction, 2> actions{ShortcutAction::Selection, ShortcutAction::Screenshot};
    const std::array<QString, 2> keys{QStringLiteral("shortcut"), QStringLiteral("screenshotShortcut")};
    std::array<QString, 2> desired;
    for (size_t i = 0; i < actions.size(); ++i) {
        m_previousShortcuts[i] = shortcuts.sequence(actions[i]);
        desired[i] = QKeySequence::fromString(m_settingsRequest.values.value(keys[i]).toString(), QKeySequence::PortableText)
                         .toString(QKeySequence::PortableText);
        m_changedShortcuts[i] = !m_settingsRequest.persist || desired[i] != m_previousShortcuts[i];
        // An unavailable capability must not prevent saving unrelated options.
        // Preserve the configured binding, without pretending it is registered.
        if (m_settingsRequest.persist && shortcuts.availability() != CapabilityState::Available
            && m_settingsRequest.values.value(keys[i]) == currentSettings.value(keys[i]))
            m_changedShortcuts[i] = false;
    }
    // Release both affected bindings before applying either, allowing a swap.
    for (size_t i = 0; i < actions.size(); ++i)
        if (m_changedShortcuts[i]) m_shortcutUpdates.append({actions[i], {}});
    for (size_t i = 0; i < actions.size(); ++i)
        if (m_changedShortcuts[i]) m_shortcutUpdates.append({actions[i], desired[i]});
    runShortcutUpdate();
}

void DesktopBridge::runShortcutUpdate()
{
    if (m_shortcutUpdateIndex == m_shortcutUpdates.size()) {
        if (m_rollingBack) {
            const auto selection = m_platform.shortcuts().sequence(ShortcutAction::Selection);
            const auto screenshot = m_platform.shortcuts().sequence(ShortcutAction::Screenshot);
            if (selection != m_previousShortcuts[0] || screenshot != m_previousShortcuts[1])
                m_rollbackErrors.append(QStringLiteral("实际快捷键未恢复到原值"));
            if (!m_rollbackErrors.isEmpty()) {
                m_settingsError += QStringLiteral("\n快捷键恢复失败：%1。当前选区快捷键：%2；截图快捷键：%3。设置文件未更改，请重试保存。")
                    .arg(m_rollbackErrors.join(QStringLiteral("；")),
                         selection.isEmpty() ? QStringLiteral("未绑定") : selection,
                         screenshot.isEmpty() ? QStringLiteral("未绑定") : screenshot);
                m_shortcutError = m_settingsError;
            }
            finishSettingsRequest(false);
        } else if (m_settingsRequest.persist && !m_settings.save(m_settingsRequest.values)) {
            rollbackSettings(m_settings.lastError());
        } else {
            if (!m_shortcutUpdates.isEmpty()) m_shortcutError.clear();
            finishSettingsRequest(true);
        }
        return;
    }
    const auto update = m_shortcutUpdates[m_shortcutUpdateIndex++];
    m_shortcutJob = m_platform.shortcuts().update(update.action, update.sequence, this);
    connect(m_shortcutJob, &ShortcutJob::succeeded, this, [this] {
        m_shortcutJob.clear();
        runShortcutUpdate();
    });
    connect(m_shortcutJob, &ShortcutJob::failed, this, [this](const PlatformError &error) {
        m_shortcutJob.clear();
        const auto message = error.message.isEmpty() ? QStringLiteral("快捷键更新失败。") : error.message;
        if (m_rollingBack) {
            m_rollbackErrors.append(message);
            runShortcutUpdate();
        } else {
            m_shortcutError = message;
            rollbackSettings(message);
        }
    });
}

void DesktopBridge::rollbackSettings(const QString &error)
{
    m_settingsError = error;
    m_rollingBack = true;
    m_shortcutUpdates.clear();
    m_shortcutUpdateIndex = 0;
    const std::array<ShortcutAction, 2> actions{ShortcutAction::Selection, ShortcutAction::Screenshot};
    for (size_t i = 0; i < actions.size(); ++i)
        if (m_changedShortcuts[i]) m_shortcutUpdates.append({actions[i], {}});
    for (size_t i = 0; i < actions.size(); ++i)
        if (m_changedShortcuts[i]) m_shortcutUpdates.append({actions[i], m_previousShortcuts[i]});
    runShortcutUpdate();
}

void DesktopBridge::finishSettingsRequest(bool success)
{
    const bool persisted = m_settingsRequest.persist;
    // Do not retain credentials from submitted drafts after completion.
    m_settingsRequest.values.clear();
    emit settingsErrorChanged();
    emit shortcutChanged();
    if (m_settingsQueue.isEmpty()) {
        m_settingsBusy = false;
        emit settingsBusyChanged();
    } else {
        QTimer::singleShot(0, this, &DesktopBridge::startSettingsRequest);
    }
    if (persisted) emit settingsSaveFinished(success);
    else if (!success) {
        if (m_tray) m_tray->showMessage(QStringLiteral("Trans"), shortcutError(), QSystemTrayIcon::Warning);
        if (!m_controller.busy()) ShowSettings();
    }
}

QString DesktopBridge::shortcutForKey(int key, int modifiers) const
{
    const auto sequence = QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers)
        & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier), static_cast<Qt::Key>(key)))
        .toString(QKeySequence::PortableText);
    return validateShortcut(sequence).isEmpty() ? sequence : QString();
}

} // namespace Trans
