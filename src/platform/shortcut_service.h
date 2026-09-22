#pragma once

#include "platform_error.h"
#include <QKeySequence>
#include <QObject>

namespace Trans {

enum class ShortcutAction { Selection, Screenshot };

class ShortcutJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() = 0;
signals:
    void succeeded();
    void failed(const Trans::PlatformError &error);
protected:
    bool finished() const { return m_finished; }
    void succeed()
    {
        if (m_finished) return;
        m_finished = true;
        emit succeeded();
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

class ShortcutService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // Updates complete asynchronously. sequence() always reports the actual
    // binding, including after an unsuccessful update or compensation attempt.
    virtual ShortcutJob *update(ShortcutAction action, const QString &sequence, QObject *owner) = 0;
    virtual QString sequence(ShortcutAction action) const = 0;
    virtual CapabilityState availability() const = 0;
    virtual QString unavailableReason() const = 0;
    QString text(ShortcutAction action) const
    {
        return QKeySequence::fromString(sequence(action), QKeySequence::PortableText).toString(QKeySequence::NativeText);
    }
signals:
    void triggered(Trans::ShortcutAction action);
    void changed();
    void capabilityChanged();
};

ShortcutService *createShortcutService(QObject *owner = nullptr);

} // namespace Trans

Q_DECLARE_METATYPE(Trans::ShortcutAction)
