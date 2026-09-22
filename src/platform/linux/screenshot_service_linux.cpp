#include "platform/screenshot_service.h"
#include "portal_screenshot.h"
#include "x11_screenshot.h"
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QTimer>

namespace Trans {
namespace {

class UnsupportedScreenshotJob final : public ScreenshotJob {
public:
    UnsupportedScreenshotJob(QObject *owner, QString reason) : ScreenshotJob(owner)
    {
        QTimer::singleShot(0, this, [this, reason = std::move(reason)] {
            finish({PlatformErrorCode::Unsupported, reason});
        });
    }
    void cancel() override { finish({PlatformErrorCode::Cancelled, QStringLiteral("截图已取消。")}); }
private:
    void finish(const PlatformError &error)
    {
        if (m_done) return;
        m_done = true;
        emit failed(error);
        deleteLater();
    }
    bool m_done = false;
};

class LinuxScreenshotService final : public ScreenshotService {
public:
    explicit LinuxScreenshotService(QObject *owner) : ScreenshotService(owner)
    {
        const auto platform = QGuiApplication::platformName();
        if (platform == "xcb") {
            m_backend = Backend::X11;
            refreshScreens();
            connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { refreshScreens(); });
            connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { refreshScreens(); });
        } else if (platform == "wayland" || platform == "wayland-egl") {
            m_backend = Backend::Portal;
            auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.freedesktop.portal.Desktop"),
                QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
            connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { refreshPortal(); });
            refreshPortal();
        } else {
            setAvailability(CapabilityState::Unsupported,
                QStringLiteral("当前显示平台不支持区域截图（%1）。").arg(platform));
        }
    }

    CapabilityState availability() const override { return m_state; }
    QString unavailableReason() const override { return m_reason; }
    ScreenshotJob *captureRegion(QObject *owner) override
    {
        switch (m_backend) {
        case Backend::X11: return new X11RegionScreenshotJob(owner);
        case Backend::Portal: return new PortalScreenshotJob(owner);
        case Backend::Unsupported: return new UnsupportedScreenshotJob(owner, m_reason);
        }
        Q_UNREACHABLE();
    }

private:
    enum class Backend { Unsupported, X11, Portal };
    void setAvailability(CapabilityState state, const QString &reason = {})
    {
        if (m_state == state && m_reason == reason) return;
        m_state = state;
        m_reason = reason;
        emit capabilityChanged();
    }
    void refreshScreens()
    {
        if (QGuiApplication::screens().isEmpty())
            setAvailability(CapabilityState::Unavailable, QStringLiteral("未找到可截图的屏幕。"));
        else
            setAvailability(CapabilityState::Available);
    }
    void refreshPortal()
    {
        const auto generation = ++m_probeGeneration;
        setAvailability(CapabilityState::Unavailable, QStringLiteral("正在检查桌面区域截图能力。"));
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"), QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("GetAll"));
        message << QStringLiteral("org.freedesktop.portal.Screenshot");
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 10000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
            QDBusPendingReply<QVariantMap> reply = *watcher;
            watcher->deleteLater();
            if (generation != m_probeGeneration) return;
            if (reply.isError()) {
                if (reply.error().type() == QDBusError::AccessDenied
                    || reply.error().name() == "org.freedesktop.portal.Error.NotAllowed")
                    setAvailability(CapabilityState::PermissionDenied, QStringLiteral("桌面拒绝了截图服务访问，请检查权限。"));
                else
                    setAvailability(CapabilityState::Unavailable,
                        QStringLiteral("桌面截图服务不可用，请安装或检查 xdg-desktop-portal 和 xdg-desktop-portal-kde。"));
                return;
            }
            const auto properties = reply.value();
            if (properties.value("version").toUInt() < 3 || !(properties.value("AvailableTargets").toUInt() & 4)) {
                setAvailability(CapabilityState::Unsupported,
                    QStringLiteral("当前桌面 Portal 不支持区域截图。请使用 Plasma X11，或升级到支持区域截图的桌面后端。"));
                return;
            }
            setAvailability(CapabilityState::Available);
        });
    }

    Backend m_backend = Backend::Unsupported;
    CapabilityState m_state = CapabilityState::Unavailable;
    QString m_reason;
    quint64 m_probeGeneration = 0;
};

} // namespace

ScreenshotService *createScreenshotService(QObject *owner)
{
    return new LinuxScreenshotService(owner);
}

} // namespace Trans
