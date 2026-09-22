#include "instance_channel_linux.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QList>
#include <QTimer>
#include <utility>

namespace Trans {
namespace {

const QString serviceName = QStringLiteral("io.github.trans.Trans");
const QString objectPath = QStringLiteral("/Trans");
constexpr int readinessTimeoutMs = 20000;
constexpr int forwardingTimeoutMs = 25000;

QString methodName(AppCommand command)
{
    switch (command) {
    case AppCommand::ShowTranslation: return QStringLiteral("ShowTranslation");
    case AppCommand::TranslateSelection: return QStringLiteral("TranslateSelection");
    case AppCommand::TranslateScreenshot: return QStringLiteral("TranslateScreenshot");
    case AppCommand::ShowSettings: return QStringLiteral("ShowSettings");
    }
    return {};
}

InstanceStartResult failure(const QDBusError &error)
{
    PlatformErrorCode code = PlatformErrorCode::Failed;
    switch (error.type()) {
    case QDBusError::AccessDenied: code = PlatformErrorCode::PermissionDenied; break;
    case QDBusError::NoReply:
    case QDBusError::Timeout:
    case QDBusError::TimedOut: code = PlatformErrorCode::Timeout; break;
    case QDBusError::Disconnected:
    case QDBusError::NoServer: code = PlatformErrorCode::Unavailable; break;
    default: break;
    }
    return {InstanceRole::Error, {code, error.message()}};
}

class InstanceEndpoint final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.trans.Trans")
public:
    explicit InstanceEndpoint(const QDBusConnection &connection)
        : m_connection(connection)
    {
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] {
            while (!m_pending.isEmpty() && m_pending.first().deadline.hasExpired()) {
                const auto request = m_pending.takeFirst();
                m_connection.send(request.message.createErrorReply(QDBusError::TimedOut,
                    QStringLiteral("Trans did not become ready before the command timed out.")));
            }
            if (!m_pending.isEmpty())
                m_timeout.start(int(m_pending.first().deadline.remainingTime()));
        });
    }

    ~InstanceEndpoint() override
    {
        for (const auto &request : std::as_const(m_pending))
            m_connection.send(request.message.createErrorReply(QDBusError::Failed,
                QStringLiteral("Trans stopped before it could handle the command.")));
    }

    void setReady(InstanceChannel::CommandHandler handler)
    {
        m_handler = std::move(handler);
        if (!m_handler)
            return;
        m_timeout.stop();
        const auto pending = std::exchange(m_pending, {});
        for (const auto &request : pending) {
            if (request.deadline.hasExpired()) {
                m_connection.send(request.message.createErrorReply(QDBusError::TimedOut,
                    QStringLiteral("Trans did not become ready before the command timed out.")));
                continue;
            }
            m_handler(request.command);
            m_connection.send(request.message.createReply());
        }
    }

public slots:
    Q_SCRIPTABLE void ShowTranslation() { dispatch(AppCommand::ShowTranslation); }
    Q_SCRIPTABLE void TranslateSelection() { dispatch(AppCommand::TranslateSelection); }
    Q_SCRIPTABLE void TranslateScreenshot() { dispatch(AppCommand::TranslateScreenshot); }
    Q_SCRIPTABLE void ShowSettings() { dispatch(AppCommand::ShowSettings); }

private:
    void dispatch(AppCommand command)
    {
        if (m_handler) {
            m_handler(command);
            return;
        }
        setDelayedReply(true);
        m_pending.append({command, message(), QDeadlineTimer(readinessTimeoutMs)});
        if (!m_timeout.isActive())
            m_timeout.start(readinessTimeoutMs);
    }

    struct PendingCommand {
        AppCommand command;
        QDBusMessage message;
        QDeadlineTimer deadline;
    };
    QDBusConnection m_connection;
    InstanceChannel::CommandHandler m_handler;
    QList<PendingCommand> m_pending;
    QTimer m_timeout;
};

class LinuxInstanceChannel final : public InstanceChannel {
public:
    LinuxInstanceChannel(const QDBusConnection &connection, QObject *parent)
        : InstanceChannel(parent), m_connection(connection), m_endpoint(connection)
    {
    }

    ~LinuxInstanceChannel() override
    {
        if (m_primary)
            m_connection.unregisterService(serviceName);
        if (m_registered)
            m_connection.unregisterObject(objectPath);
    }

    InstanceStartResult start(AppCommand command) override
    {
        if (m_started)
            return {InstanceRole::Error, {PlatformErrorCode::Conflict,
                    QStringLiteral("The instance channel has already been started.")}};
        m_started = true;
        const QString method = methodName(command);
        if (method.isEmpty())
            return {InstanceRole::Error, {PlatformErrorCode::Failed,
                    QStringLiteral("Unknown application command.")}};
        if (!m_connection.isConnected())
            return {InstanceRole::Error, {PlatformErrorCode::Unavailable,
                    QStringLiteral("Cannot connect to the session D-Bus.")}};
        // Export before election: even a command arriving during a nested startup
        // event loop reaches a real endpoint and waits for setReady().
        if (!m_connection.registerObject(objectPath, &m_endpoint, QDBusConnection::ExportScriptableSlots))
            return failure(m_connection.lastError());
        m_registered = true;

        QDeadlineTimer deadline(forwardingTimeoutMs);
        auto *bus = m_connection.interface();
        while (!deadline.hasExpired()) {
            const auto registration = bus->registerService(serviceName,
                QDBusConnectionInterface::DontQueueService, QDBusConnectionInterface::DontAllowReplacement);
            if (!registration.isValid())
                return failure(registration.error());
            if (registration.value() == QDBusConnectionInterface::ServiceRegistered) {
                m_primary = true;
                return {InstanceRole::Primary, {}};
            }

            // Pin the call to the elected owner, not a replacement that could
            // take the well-known name between lookup and delivery.
            const auto owner = bus->serviceOwner(serviceName);
            if (!owner.isValid()) {
                if (owner.error().name() == QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner"))
                    continue;
                return failure(owner.error());
            }
            auto message = QDBusMessage::createMethodCall(owner.value(), objectPath, serviceName, method);
            message.setAutoStartService(false);
            QDBusPendingCallWatcher watcher(m_connection.asyncCall(message, int(deadline.remainingTime())));
            QEventLoop loop;
            connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
            if (!watcher.isFinished())
                loop.exec(QEventLoop::ExcludeUserInputEvents);
            const auto reply = watcher.reply();
            if (reply.type() == QDBusMessage::ReplyMessage)
                return {InstanceRole::Forwarded, {}};
            const QDBusError error(reply);
            // These errors prove non-delivery; a timeout does not. Never repeat
            // a command after an ambiguous failure, which could execute it twice.
            if (error.type() == QDBusError::ServiceUnknown
                || error.name() == QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner"))
                continue;
            return failure(error);
        }
        return {InstanceRole::Error, {PlatformErrorCode::Timeout,
                QStringLiteral("Timed out while contacting the running Trans instance.")}};
    }

    void setReady(CommandHandler handler) override
    {
        if (m_primary)
            m_endpoint.setReady(std::move(handler));
    }

private:
    QDBusConnection m_connection;
    InstanceEndpoint m_endpoint;
    bool m_started = false;
    bool m_registered = false;
    bool m_primary = false;
};

} // namespace

std::unique_ptr<InstanceChannel> createLinuxInstanceChannel(const QDBusConnection &connection, QObject *parent)
{
    return std::make_unique<LinuxInstanceChannel>(connection, parent);
}

std::unique_ptr<InstanceChannel> createInstanceChannel(QObject *parent)
{
    return createLinuxInstanceChannel(QDBusConnection::sessionBus(), parent);
}

} // namespace Trans

#include "instance_channel_linux.moc"
