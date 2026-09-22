#include "x11_screenshot.h"
#include <QCursor>
#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>

namespace Trans {

X11RegionScreenshotJob::X11RegionScreenshotJob(QObject *owner) : ScreenshotJob(owner)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { fail(PlatformErrorCode::Timeout, QStringLiteral("截图请求超时，请重试。")); });
    // Capture after the compositor has removed Trans's hidden windows, before showing overlays.
    QTimer::singleShot(150, this, &X11RegionScreenshotJob::start);
}
X11RegionScreenshotJob::~X11RegionScreenshotJob() { clearOverlays(); }

void X11RegionScreenshotJob::start()
{
    if (m_done) return;
    if (QGuiApplication::platformName() != "xcb") {
        fail(PlatformErrorCode::Unsupported, QStringLiteral("当前框选后端需要 X11 会话。")); return;
    }
    const auto screens = QGuiApplication::screens();
    // Capture every screen before showing any overlay, so overlays never appear in the source.
    for (auto *screen : screens) {
        const auto image = screen->grabWindow(0).toImage();
        if (image.isNull()) {
            fail(PlatformErrorCode::Failed, QStringLiteral("无法读取屏幕图像，请重新截图。")); return;
        }
        auto *overlay = new RegionOverlay(image, screen->geometry());
        m_overlays.append(overlay);
        connect(screen, &QScreen::geometryChanged, this, [this] { cancel(); });
        connect(screen, &QObject::destroyed, this, [this] { cancel(); });
        overlay->cancelled = [this] { cancel(); };
        overlay->selected = [this](const QImage &cropped) {
            if (m_done) return;
            m_done = true;
            m_timer.stop();
            clearOverlays();
            emit succeeded(cropped);
            deleteLater();
        };
    }
    if (m_overlays.isEmpty()) {
        fail(PlatformErrorCode::Unavailable, QStringLiteral("未找到可截图的屏幕。")); return;
    }
    RegionOverlay *active = m_overlays.first();
    for (const auto &overlay : m_overlays) {
        overlay->show();
        overlay->raise();
        if (overlay->geometry().contains(QCursor::pos())) active = overlay;
    }
    active->activateWindow();
    active->setFocus();
    active->grabKeyboard();
    m_timer.start(180000);
}

void X11RegionScreenshotJob::clearOverlays()
{
    for (const auto &overlay : m_overlays) {
        if (!overlay) continue;
        // These widgets may be inside their mouse callback; defer destruction.
        overlay->selected = {};
        overlay->cancelled = {};
        overlay->releaseKeyboard();
        overlay->hide();
        overlay->deleteLater();
    }
    m_overlays.clear();
}
void X11RegionScreenshotJob::fail(PlatformErrorCode code, const QString &message)
{
    if (m_done) return;
    m_done = true;
    m_timer.stop();
    clearOverlays();
    emit failed({code, message});
    deleteLater();
}
void X11RegionScreenshotJob::cancel() { fail(PlatformErrorCode::Cancelled, QStringLiteral("截图已取消。")); }

} // namespace Trans
