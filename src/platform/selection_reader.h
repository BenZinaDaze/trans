#pragma once

#include "window_integration.h"

namespace Trans {

struct SelectionResult {
    QString text;
    SourceContextPtr source;
};

class SelectionJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() = 0;
signals:
    void succeeded(const Trans::SelectionResult &result);
    void failed(const Trans::PlatformError &error);
protected:
    bool finished() const { return m_finished; }
    void succeed(const SelectionResult &result)
    {
        if (m_finished) return;
        m_finished = true;
        emit succeeded(result);
        deleteLater();
    }
    void fail(const PlatformError &error)
    {
        if (m_finished) return;
        m_finished = true;
        emit failed(error);
        deleteLater();
    }
private:
    bool m_finished = false;
};

class SelectionReader : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // Completion is always asynchronous; implementations must not change focus.
    virtual SelectionJob *read(SourceContextPtr source, QObject *owner) = 0;
    virtual CapabilityState availability() const = 0;
    virtual QString unavailableReason() const = 0;
signals:
    void capabilityChanged();
};

SelectionReader *createSelectionReader(QObject *owner = nullptr);

} // namespace Trans

Q_DECLARE_METATYPE(Trans::SelectionResult)
