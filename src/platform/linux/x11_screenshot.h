#pragma once

#include "screenshot.h"
#include "region_capture.h"
#include <QPointer>
#include <QTimer>

namespace Trans {

class X11RegionScreenshotJob final : public ScreenshotJob {
    Q_OBJECT
public:
    explicit X11RegionScreenshotJob(QObject *owner = nullptr);
    ~X11RegionScreenshotJob() override;
    void cancel() override;
private:
    void start();
    void clearOverlays();
    void fail(PlatformErrorCode code, const QString &message);
    QList<QPointer<RegionOverlay>> m_overlays;
    QTimer m_timer;
    bool m_done = false;
};

} // namespace Trans
