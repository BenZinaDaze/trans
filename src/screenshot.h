#pragma once

#include "platform/platform_error.h"
#include <QImage>
#include <QObject>

namespace Trans {

// Capture starts asynchronously. Jobs emit one terminal signal and delete themselves;
// destroying the QObject owner cancels backend resources without emitting a result.
class ScreenshotJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() = 0;
signals:
    void succeeded(const QImage &image);
    void failed(const Trans::PlatformError &error);
};

} // namespace Trans
