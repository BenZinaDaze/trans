#include "platform/screenshot_service.h"
#include "region_capture.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

namespace Trans {
namespace {

class WindowsRegionScreenshotJob final : public ScreenshotJob, public QAbstractNativeEventFilter {
public:
    explicit WindowsRegionScreenshotJob(QObject *owner) : ScreenshotJob(owner)
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] {
            fail(PlatformErrorCode::Timeout, QStringLiteral("截图请求超时，请重试。"));
        });
        m_timeout.start(180000);
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { displayChanged(); });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { displayChanged(); });
        const auto screens = QGuiApplication::screens();
        for (auto *screen : screens) {
            connect(screen, &QScreen::geometryChanged, this, [this] { displayChanged(); });
            connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { displayChanged(); });
            connect(screen, &QScreen::physicalDotsPerInchChanged, this, [this] { displayChanged(); });
            connect(screen, &QScreen::orientationChanged, this, [this] { displayChanged(); });
            connect(screen, &QObject::destroyed, this, [this] { displayChanged(); });
        }
        if (QGuiApplication::platformName() == "windows") {
            QCoreApplication::instance()->installNativeEventFilter(this);
            m_filterInstalled = true;
        }
        // The caller hides its windows synchronously. Let Qt submit those changes
        // and DWM remove the frames before sampling any screen.
        QTimer::singleShot(150, this, [this] { start(); });
    }

    ~WindowsRegionScreenshotJob() override
    {
        m_done = true;
        cleanup();
    }

    void cancel() override { fail(PlatformErrorCode::Cancelled, QStringLiteral("截图已取消。")); }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override
    {
        if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
            const auto *native = static_cast<const MSG *>(message);
            if (native->message == WM_DISPLAYCHANGE) displayChanged();
        }
        return false;
    }

private:
    void start()
    {
        if (m_done) return;
        if (QGuiApplication::platformName() != "windows") {
            fail(PlatformErrorCode::Unsupported, QStringLiteral("当前显示平台不支持 Windows 区域截图。"));
            return;
        }
        const auto screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            fail(PlatformErrorCode::Unavailable, QStringLiteral("未找到可截图的屏幕。"));
            return;
        }
        if (FAILED(DwmFlush())) {
            fail(PlatformErrorCode::Unavailable, QStringLiteral("Windows 桌面合成器暂不可用，请在可交互桌面重试截图。"));
            return;
        }
        // QScreen::grabWindow(0) captures that screen in native pixels, including
        // its native origin on negative-coordinate/mixed-DPI desktop layouts.
        // Keep the full physical image; RegionOverlay maps local logical pointer
        // coordinates by the actual image/overlay dimensions, not an assumed DPR.
        // Freeze every screen before even constructing an overlay.
        struct CapturedScreen {
            QPointer<QScreen> screen;
            QRect geometry;
            QImage image;
        };
        QList<CapturedScreen> captures;
        captures.reserve(screens.size());
        for (auto *screen : screens) captures.append({screen, screen->geometry(), {}});
        for (auto &capture : captures) {
            if (m_done || !capture.screen) return;
            auto image = capture.screen->grabWindow(0).toImage();
            if (m_done) return;
            if (image.isNull()) {
                fail(PlatformErrorCode::Failed, QStringLiteral("无法读取屏幕图像，请确认桌面可交互后重试。"));
                return;
            }
            capture.image = std::move(image);
        }
        for (auto &capture : captures) {
            if (m_done || !capture.screen) return;
            auto *overlay = new RegionOverlay(std::move(capture.image), capture.geometry);
            m_overlays.append(overlay);
            overlay->setWindowFlag(Qt::Tool, true);
            overlay->setScreen(capture.screen);
            overlay->setGeometry(capture.geometry);
            if (m_done) return;
            overlay->cancelled = [this] { cancel(); };
            overlay->selected = [this](const QImage &cropped) {
                if (m_done) return;
                if (cropped.isNull()) {
                    fail(PlatformErrorCode::Failed, QStringLiteral("所选区域没有有效图像，请重新截图。"));
                    return;
                }
                m_done = true;
                cleanup();
                deleteLater();
                emit succeeded(cropped);
            };
        }
        if (m_done) return;
        QPointer<RegionOverlay> active = m_overlays.first();
        const auto cursor = QCursor::pos();
        const auto overlays = m_overlays;
        for (const auto &overlay : overlays) {
            if (m_done || !overlay) return;
            overlay->show();
            if (m_done || !overlay) return;
            overlay->raise();
            if (m_done || !overlay) return;
            if (overlay->geometry().contains(cursor)) active = overlay;
        }
        // Normal application activation only. Mouse selection/right-click remain
        // usable if Windows denies foreground activation; no hooks are installed.
        active->activateWindow();
        if (m_done || !active) return;
        active->setFocus();
        // GDI/Qt cannot reliably identify protected/DRM black pixels. A black
        // capture is not reclassified as a permission error or synthesized image.
    }

    void displayChanged()
    {
        fail(PlatformErrorCode::Cancelled, QStringLiteral("显示器布局或缩放已改变，请重新截图。"));
    }

    void cleanup()
    {
        m_timeout.stop();
        if (m_filterInstalled && QCoreApplication::instance()) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
            m_filterInstalled = false;
        }
        // A selection/cancel callback may still be on an overlay's event stack.
        // Hide and sever callbacks immediately, then destroy widgets safely.
        const auto overlays = std::move(m_overlays);
        m_overlays.clear();
        for (const auto &overlay : overlays) {
            if (!overlay) continue;
            overlay->selected = {};
            overlay->cancelled = {};
            overlay->hide();
            overlay->deleteLater();
        }
    }

    void fail(PlatformErrorCode code, const QString &message)
    {
        if (m_done) return;
        m_done = true;
        cleanup();
        deleteLater();
        emit failed({code, message});
    }

    QList<QPointer<RegionOverlay>> m_overlays;
    QTimer m_timeout;
    bool m_done = false;
    bool m_filterInstalled = false;
};

class WindowsScreenshotService final : public ScreenshotService {
public:
    explicit WindowsScreenshotService(QObject *owner) : ScreenshotService(owner)
    {
        refreshScreens();
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { refreshScreens(); });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { refreshScreens(); });
    }

    ~WindowsScreenshotService() override
    {
        if (m_active) m_active->cancel();
    }

    CapabilityState availability() const override { return m_state; }
    QString unavailableReason() const override { return m_reason; }

    ScreenshotJob *captureRegion(QObject *owner) override
    {
        if (m_active) m_active->cancel();
        auto *job = new WindowsRegionScreenshotJob(owner);
        m_active = job;
        connect(job, &ScreenshotJob::succeeded, this, [this, job] {
            if (m_active == job) m_active.clear();
        });
        connect(job, &ScreenshotJob::failed, this, [this, job] {
            if (m_active == job) m_active.clear();
        });
        return job;
    }

private:
    void refreshScreens()
    {
        CapabilityState state = CapabilityState::Available;
        QString reason;
        if (QGuiApplication::platformName() != "windows") {
            state = CapabilityState::Unsupported;
            reason = QStringLiteral("当前显示平台不支持 Windows 区域截图。");
        } else if (QGuiApplication::screens().isEmpty()) {
            state = CapabilityState::Unavailable;
            reason = QStringLiteral("未找到可截图的屏幕。");
        }
        if (state == m_state && reason == m_reason) return;
        m_state = state;
        m_reason = std::move(reason);
        emit capabilityChanged();
    }

    QPointer<WindowsRegionScreenshotJob> m_active;
    CapabilityState m_state = CapabilityState::Unavailable;
    QString m_reason;
};

} // namespace

ScreenshotService *createScreenshotService(QObject *owner) { return new WindowsScreenshotService(owner); }

} // namespace Trans
