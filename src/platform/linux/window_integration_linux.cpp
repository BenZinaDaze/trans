#include "platform/window_integration.h"

#include <KWindowSystem>
#include <KX11Extras>
#include <QGuiApplication>
#include <QWindow>

namespace Trans {
namespace {

class X11SourceContext final : public SourceContext {
public:
    explicit X11SourceContext(WId window) : window(window) {}
    const WId window;
};

class LinuxWindowIntegration final : public WindowIntegration {
public:
    using WindowIntegration::WindowIntegration;
    SourceContextPtr captureSource(QWindow *popup, QWindow *settings) override
    {
        if (QGuiApplication::platformName() != "xcb") return {};
        const auto active = KX11Extras::activeWindow();
        if (!active || (popup && active == popup->winId()) || (settings && active == settings->winId()))
            return {};
        return std::make_shared<X11SourceContext>(active);
    }
    void activate(QWindow *window) override
    {
        if (!window) return;
        if (QGuiApplication::platformName() == "xcb") KWindowSystem::activateWindow(window);
        else window->requestActivate();
    }
    void restoreSource(const SourceContextPtr &source) override
    {
        if (QGuiApplication::platformName() != "xcb") return;
        const auto context = std::dynamic_pointer_cast<const X11SourceContext>(source);
        if (context && KX11Extras::hasWId(context->window)) KX11Extras::activateWindow(context->window);
    }
    void popupMapped(QWindow *window) override
    {
        if (window && QGuiApplication::platformName() == "xcb")
            KX11Extras::setState(window->winId(), NET::SkipTaskbar | NET::SkipPager);
    }
    CapabilityState focusRestoration() const override
    {
        return QGuiApplication::platformName() == "xcb" ? CapabilityState::Available : CapabilityState::Unsupported;
    }
    QString focusRestorationReason() const override
    {
        return focusRestoration() == CapabilityState::Available ? QString()
            : QStringLiteral("当前窗口系统不允许应用恢复其他窗口的焦点。");
    }
};

} // namespace

WindowIntegration *createWindowIntegration(QObject *owner) { return new LinuxWindowIntegration(owner); }

} // namespace Trans
