#pragma once

#include "platform_error.h"
#include <QObject>
#include <memory>

class QWindow;

namespace Trans {

// Only the backend that created a context may interpret it. Shared code retains
// the context for the request without depending on a native window identifier.
class SourceContext {
public:
    virtual ~SourceContext() = default;
};
using SourceContextPtr = std::shared_ptr<const SourceContext>;

class WindowIntegration : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual SourceContextPtr captureSource(QWindow *popup, QWindow *settings) = 0;
    virtual void activate(QWindow *window) = 0;
    virtual void restoreSource(const SourceContextPtr &source) = 0;
    virtual void popupMapped(QWindow *window) = 0;
    virtual CapabilityState focusRestoration() const = 0;
    virtual QString focusRestorationReason() const = 0;
signals:
    void capabilityChanged();
};

WindowIntegration *createWindowIntegration(QObject *owner = nullptr);

} // namespace Trans

Q_DECLARE_METATYPE(Trans::SourceContextPtr)
