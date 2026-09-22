#include "platform/shortcut_service.h"
#include "settings.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <array>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Trans {
namespace {

struct NativeShortcut {
    QString sequence;
    UINT modifiers = 0;
    UINT key = 0;
};

PlatformError nativeError(DWORD error)
{
    if (error == ERROR_HOTKEY_ALREADY_REGISTERED)
        return {PlatformErrorCode::Conflict, QStringLiteral("此快捷键已被 Windows 或其他应用占用，请录制另一个组合。")};
    if (error == ERROR_ACCESS_DENIED)
        return {PlatformErrorCode::PermissionDenied, QStringLiteral("Windows 拒绝注册此全局快捷键。")};
    if (error == ERROR_INVALID_PARAMETER)
        return {PlatformErrorCode::Unsupported, QStringLiteral("Windows 不支持此全局快捷键组合。")};
    return {PlatformErrorCode::Failed, QStringLiteral("Windows 无法更新全局快捷键（错误 %1）。").arg(error)};
}

PlatformError parseShortcut(const QString &requested, NativeShortcut &native)
{
    const auto validation = validateShortcut(requested);
    if (!validation.isEmpty()) return {PlatformErrorCode::Failed, validation};
    if (requested.isEmpty()) return {};
    const auto sequence = QKeySequence::fromString(requested, QKeySequence::PortableText);
    const auto combination = sequence[0];
    const auto modifiers = combination.keyboardModifiers();
    if (modifiers & ~(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))
        return {PlatformErrorCode::Unsupported, QStringLiteral("Windows 全局快捷键不支持此修饰键或区分数字小键盘。")};
    if (modifiers.testFlag(Qt::MetaModifier))
        return {PlatformErrorCode::Unsupported, QStringLiteral("含 Windows / Meta 键的快捷键由系统保留，请改用 Ctrl 或 Alt。")};
    if (modifiers.testFlag(Qt::ControlModifier)) native.modifiers |= MOD_CONTROL;
    if (modifiers.testFlag(Qt::AltModifier)) native.modifiers |= MOD_ALT;
    if (modifiers.testFlag(Qt::ShiftModifier)) native.modifiers |= MOD_SHIFT;

    const int key = combination.key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        native.key = UINT('A' + key - Qt::Key_A);
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        native.key = UINT(VK_F1 + key - Qt::Key_F1);
    } else {
        switch (key) {
        case Qt::Key_Escape: native.key = VK_ESCAPE; break;
        case Qt::Key_Tab: native.key = VK_TAB; break;
        case Qt::Key_Backtab: native.key = VK_TAB; native.modifiers |= MOD_SHIFT; break;
        case Qt::Key_Backspace: native.key = VK_BACK; break;
        case Qt::Key_Return: native.key = VK_RETURN; break;
        case Qt::Key_Insert: native.key = VK_INSERT; break;
        case Qt::Key_Delete: native.key = VK_DELETE; break;
        case Qt::Key_Pause: native.key = VK_PAUSE; break;
        case Qt::Key_Print: native.key = VK_SNAPSHOT; break;
        case Qt::Key_Home: native.key = VK_HOME; break;
        case Qt::Key_End: native.key = VK_END; break;
        case Qt::Key_Left: native.key = VK_LEFT; break;
        case Qt::Key_Up: native.key = VK_UP; break;
        case Qt::Key_Right: native.key = VK_RIGHT; break;
        case Qt::Key_Down: native.key = VK_DOWN; break;
        case Qt::Key_PageUp: native.key = VK_PRIOR; break;
        case Qt::Key_PageDown: native.key = VK_NEXT; break;
        case Qt::Key_Space: native.key = VK_SPACE; break;
        default:
            // Punctuation and digits depend on the active keyboard layout. Include
            // the character's required Shift; never pretend AltGr is a simple key.
            if (key >= 0x21 && key <= 0xffff) {
                const SHORT translated = VkKeyScanExW(WCHAR(key), GetKeyboardLayout(0));
                if (translated != -1 && (HIBYTE(translated) & ~1) == 0) {
                    native.key = LOBYTE(translated);
                    if (HIBYTE(translated) & 1) native.modifiers |= MOD_SHIFT;
                }
            }
            break;
        }
    }
    if (!native.key)
        return {PlatformErrorCode::Unsupported, QStringLiteral("此按键不能可靠映射为 Windows 全局快捷键，请改用字母、功能键或普通导航键。")};
    const bool reserved = native.key == VK_F12 || native.key == VK_SNAPSHOT
        || (native.key == VK_TAB && (native.modifiers & MOD_ALT))
        || (native.key == VK_ESCAPE && (native.modifiers & (MOD_CONTROL | MOD_ALT)))
        || ((native.key == VK_F4 || native.key == VK_SPACE) && native.modifiers == MOD_ALT)
        || (native.key == VK_DELETE && (native.modifiers & (MOD_CONTROL | MOD_ALT)) == (MOD_CONTROL | MOD_ALT));
    if (reserved)
        return {PlatformErrorCode::Unsupported, QStringLiteral("此快捷键由 Windows 或调试器保留，请录制另一个组合。")};
    native.sequence = sequence.toString(QKeySequence::PortableText);
    return {};
}

class WindowsShortcutService;

class WindowsShortcutJob final : public ShortcutJob {
public:
    WindowsShortcutJob(WindowsShortcutService *service, ShortcutAction action, QString sequence, QObject *owner);
    void cancel() override { fail({PlatformErrorCode::Cancelled, QStringLiteral("快捷键更新已取消。")}); }
};

class WindowsShortcutService final : public ShortcutService, public QAbstractNativeEventFilter {
public:
    explicit WindowsShortcutService(QObject *owner) : ShortcutService(owner)
    {
        if (QGuiApplication::platformName() != "windows") return;
        // A private, never-shown HWND keeps IDs local to this service and lets Qt
        // deliver WM_HOTKEY through its normal native event filter.
        m_receiver = std::make_unique<QWindow>();
        m_receiver->setFlags(Qt::Tool | Qt::FramelessWindowHint);
        m_receiver->create();
        m_handle = reinterpret_cast<HWND>(m_receiver->winId());
        if (IsWindow(m_handle)) QCoreApplication::instance()->installNativeEventFilter(this);
        else m_handle = nullptr;
    }

    ~WindowsShortcutService() override
    {
        if (QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
        for (size_t i = 0; i < m_bindings.size(); ++i) {
            if (m_bindings[i].key) UnregisterHotKey(m_handle, int(i + 1));
        }
        // Destroying the private HWND also removes any OS registration if an
        // explicit unregister failed during teardown.
        m_receiver.reset();
    }

    ShortcutJob *update(ShortcutAction action, const QString &sequence, QObject *owner) override
    {
        return new WindowsShortcutJob(this, action, sequence, owner);
    }

    QString sequence(ShortcutAction action) const override { return m_bindings[index(action)].sequence; }

    CapabilityState availability() const override
    {
        if (QGuiApplication::platformName() != "windows") return CapabilityState::Unsupported;
        return m_handle ? CapabilityState::Available : CapabilityState::Unavailable;
    }

    QString unavailableReason() const override
    {
        if (availability() == CapabilityState::Unsupported)
            return QStringLiteral("当前窗口系统不支持 Windows 全局快捷键。");
        return m_handle ? QString() : QStringLiteral("无法创建 Windows 全局快捷键接收窗口。");
    }

    PlatformError apply(ShortcutAction action, const QString &requested, bool &success)
    {
        success = false;
        if (availability() != CapabilityState::Available)
            return {availability() == CapabilityState::Unsupported ? PlatformErrorCode::Unsupported : PlatformErrorCode::Unavailable,
                    unavailableReason()};
        NativeShortcut desired;
        const auto parsed = parseShortcut(requested, desired);
        if (!parsed.message.isEmpty()) return parsed;
        const auto i = index(action);
        const auto other = 1 - i;
        if (desired.key && desired.key == m_bindings[other].key && desired.modifiers == m_bindings[other].modifiers)
            return {PlatformErrorCode::Conflict, QStringLiteral("截图翻译与选区翻译不能使用相同的 Windows 快捷键。")};
        auto &actual = m_bindings[i];
        if (actual.key == desired.key && actual.modifiers == desired.modifiers) {
            if (actual.sequence != desired.sequence) {
                actual.sequence = desired.sequence;
                emit changed();
            }
            success = true;
            return {};
        }
        const auto previous = actual;
        const int id = int(i + 1);
        if (actual.key && !UnregisterHotKey(m_handle, id)) {
            const DWORD error = GetLastError();
            if (error != ERROR_HOTKEY_NOT_REGISTERED) return nativeError(error);
        }
        actual = {};
        if (!desired.key || RegisterHotKey(m_handle, id, desired.modifiers | MOD_NOREPEAT, desired.key)) {
            actual = std::move(desired);
            success = true;
            emit changed();
            return {};
        }
        const DWORD error = GetLastError();
        // Report only a registration that Windows actually accepted. Restoration
        // can itself fail if another application took the old binding meanwhile.
        const bool restored = previous.key
            && RegisterHotKey(m_handle, id, previous.modifiers | MOD_NOREPEAT, previous.key);
        const DWORD restorationError = restored || !previous.key ? ERROR_SUCCESS : GetLastError();
        if (restored) actual = previous;
        emit changed();
        auto failure = nativeError(error);
        if (previous.key && !restored)
            failure.message += QStringLiteral(" 原快捷键也未能恢复（错误 %1），当前未绑定。").arg(restorationError);
        return failure;
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
        if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") return false;
        const auto *native = static_cast<const MSG *>(message);
        if (native->message != WM_HOTKEY || native->hwnd != m_handle) return false;
        if (native->wParam < 1 || native->wParam > m_bindings.size()) return false;
        const size_t i = size_t(native->wParam - 1);
        const auto &binding = m_bindings[i];
        // Ignore a queued event belonging to a binding that was just replaced.
        if (binding.key && HIWORD(native->lParam) == binding.key
            && (LOWORD(native->lParam) & ~MOD_NOREPEAT) == binding.modifiers) {
            emit triggered(i == 0 ? ShortcutAction::Selection : ShortcutAction::Screenshot);
        }
        if (result) *result = 0;
        return true;
    }

private:
    static size_t index(ShortcutAction action) { return action == ShortcutAction::Selection ? 0 : 1; }
    std::array<NativeShortcut, 2> m_bindings;
    std::unique_ptr<QWindow> m_receiver;
    HWND m_handle = nullptr;
};

WindowsShortcutJob::WindowsShortcutJob(WindowsShortcutService *service, ShortcutAction action, QString sequence, QObject *owner)
    : ShortcutJob(owner)
{
    QTimer::singleShot(0, this, [this, service = QPointer<WindowsShortcutService>(service), action, sequence = std::move(sequence)] {
        if (finished()) return;
        if (!service) {
            fail({PlatformErrorCode::Unavailable, QStringLiteral("快捷键服务已关闭。")});
            return;
        }
        bool applied = false;
        const QPointer<WindowsShortcutJob> guard(this);
        const auto error = service->apply(action, sequence, applied);
        if (!guard || finished()) return;
        if (applied) succeed();
        else fail(error);
    });
}

} // namespace

ShortcutService *createShortcutService(QObject *owner) { return new WindowsShortcutService(owner); }

} // namespace Trans
