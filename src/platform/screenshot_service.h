#pragma once

#include "screenshot.h"

namespace Trans {

class ScreenshotService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual CapabilityState availability() const = 0;
    virtual QString unavailableReason() const = 0;
    // Results arrive after this call returns; the owner may cancel or destroy the job.
    virtual ScreenshotJob *captureRegion(QObject *owner) = 0;
signals:
    void capabilityChanged();
};

ScreenshotService *createScreenshotService(QObject *owner = nullptr);

} // namespace Trans
