#include "source_context_windows.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QWindow>
#include <shobjidl.h>

namespace Trans {
namespace {

class WindowsWindowIntegration final : public WindowIntegration, public QAbstractNativeEventFilter {
public:
    explicit WindowsWindowIntegration(QObject *owner) : WindowIntegration(owner)
    {
        if (!supported()) return;
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_uninitializeCom = SUCCEEDED(initialized);
        // Qt may already own a COM apartment. A different apartment is also valid
        // for this in-process shell interface, but must not be uninitialized here.
        if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) {
            if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&m_taskbar)))) {
                if (FAILED(m_taskbar->HrInit())) {
                    m_taskbar->Release();
                    m_taskbar = nullptr;
                }
            }
        }
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
        QCoreApplication::instance()->installNativeEventFilter(this);
    }

    ~WindowsWindowIntegration() override
    {
        if (QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
        if (m_taskbar) m_taskbar->Release();
        if (m_uninitializeCom) CoUninitialize();
    }

    SourceContextPtr captureSource(QWindow *, QWindow *) override
    {
        if (!supported()) return {};
        const HWND window = GetForegroundWindow();
        if (!window || !IsWindow(window)) return {};
        DWORD processId = 0;
        const DWORD threadId = GetWindowThreadProcessId(window, &processId);
        // Exclude every Trans window, including settings, menus and capture overlays.
        if (!threadId || !processId || processId == GetCurrentProcessId()) return {};
        return std::make_shared<WindowsSourceContext>(window, processId, threadId);
    }

    void activate(QWindow *window) override
    {
        if (!supported() || !window || !window->isVisible()) return;
        // Respect the foreground lock. No synthetic input or thread-input attachment.
        SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
    }

    void restoreSource(const SourceContextPtr &source) override
    {
        if (!supported()) return;
        const auto context = std::dynamic_pointer_cast<const WindowsSourceContext>(source);
        if (!context || !context->window || !IsWindow(context->window)
            || !IsWindowVisible(context->window) || context->processId == GetCurrentProcessId()) return;
        DWORD processId = 0;
        const DWORD threadId = GetWindowThreadProcessId(context->window, &processId);
        if (!threadId || threadId != context->threadId || processId != context->processId) return;
        SetForegroundWindow(context->window);
    }

    void popupMapped(QWindow *window) override
    {
        if (!supported() || !window) return;
        m_popup = window;
        const HWND handle = reinterpret_cast<HWND>(window->winId());
        if (!IsWindow(handle)) return;
        const LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
        const LONG_PTR desired = (style | WS_EX_TOOLWINDOW) & ~LONG_PTR(WS_EX_APPWINDOW);
        if (desired != style) SetWindowLongPtrW(handle, GWL_EXSTYLE, desired);
        // Never impose topmost independently of the shared Qt stay-on-top setting.
        const bool topmost = window->flags().testFlag(Qt::WindowStaysOnTopHint);
        if (desired != style || bool(style & WS_EX_TOPMOST) != topmost) {
            SetWindowPos(handle, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        // The shared popup starts as Qt::Window. Remove a taskbar button that the
        // shell already created, without a hide/show cycle or changing Qt flags.
        if (m_taskbar) m_taskbar->DeleteTab(handle);
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override
    {
        if (eventType != "windows_generic_MSG" || !m_popup || !m_taskbarCreated) return false;
        const auto *native = static_cast<const MSG *>(message);
        if (native->message == m_taskbarCreated
            && native->hwnd == reinterpret_cast<HWND>(m_popup->winId())) popupMapped(m_popup);
        return false;
    }

    CapabilityState focusRestoration() const override
    {
        return supported() ? CapabilityState::Available : CapabilityState::Unsupported;
    }

    QString focusRestorationReason() const override
    {
        return supported()
            ? QStringLiteral("Windows 可能阻止后台窗口激活；焦点恢复仅尽力执行，不会强行抢占焦点。")
            : QStringLiteral("当前窗口系统不支持 Windows 焦点恢复。");
    }

private:
    static bool supported() { return QGuiApplication::platformName() == "windows"; }
    QPointer<QWindow> m_popup;
    ITaskbarList *m_taskbar = nullptr;
    UINT m_taskbarCreated = 0;
    bool m_uninitializeCom = false;
};

} // namespace

WindowIntegration *createWindowIntegration(QObject *owner) { return new WindowsWindowIntegration(owner); }

} // namespace Trans
