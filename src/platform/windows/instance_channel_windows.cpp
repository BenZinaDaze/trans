#include "instance_channel_windows.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <utility>

namespace Trans {
namespace {

constexpr int forwardingTimeoutMs = 25000;
constexpr int readinessTimeoutMs = 20000;
constexpr int replyDrainTimeoutMs = 1000;
constexpr int retryIntervalMs = 25;
constexpr int maximumConnections = 32;
constexpr qsizetype frameSize = 12;
constexpr quint32 frameMagic = 0x54524e53; // TRNS
constexpr quint8 protocolVersion = 1;
enum class FrameType : quint8 { Hello = 1, Command = 2, Result = 3 };
enum class Reply : quint32 { Dispatched = 0, Invalid = 1, NotReady = 2 };

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE value = nullptr) : m_value(value) {}
    ~NativeHandle() { reset(); }
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    HANDLE get() const { return m_value; }
    explicit operator bool() const { return m_value && m_value != INVALID_HANDLE_VALUE; }
    HANDLE release() { return std::exchange(m_value, nullptr); }
    void reset(HANDLE value = nullptr)
    {
        if (*this)
            CloseHandle(m_value);
        m_value = value;
    }
private:
    HANDLE m_value;
};

struct LocalAllocation {
    ~LocalAllocation() { LocalFree(value); }
    HLOCAL value = nullptr;
};

InstanceStartResult failure(PlatformErrorCode code, const QString &message)
{
    return {InstanceRole::Error, {code, message}};
}

InstanceStartResult nativeFailure(const QString &operation, DWORD error = GetLastError())
{
    const auto code = error == ERROR_ACCESS_DENIED ? PlatformErrorCode::PermissionDenied
        : error == ERROR_INVALID_HANDLE ? PlatformErrorCode::Conflict : PlatformErrorCode::Failed;
    return failure(code, QStringLiteral("%1 (Windows error %2).").arg(operation).arg(error));
}

struct Identity {
    QByteArray userSid;
    QByteArray logonSid;
    DWORD session = 0;
    LUID authentication = {};
};

bool tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind, QByteArray &buffer)
{
    DWORD size = 0;
    if (GetTokenInformation(token, kind, nullptr, 0, &size)
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 1024 * 1024)
        return false;
    buffer.resize(size);
    return GetTokenInformation(token, kind, buffer.data(), size, &size);
}

bool readIdentity(HANDLE process, Identity &identity)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken))
        return false;
    NativeHandle token(rawToken);
    QByteArray user;
    QByteArray groups;
    if (!tokenInformation(token.get(), TokenUser, user)
        || !tokenInformation(token.get(), TokenGroups, groups))
        return false;
    const auto userInfo = reinterpret_cast<const TOKEN_USER *>(user.constData());
    identity.userSid = QByteArray(static_cast<const char *>(userInfo->User.Sid),
                                  GetLengthSid(userInfo->User.Sid));
    const auto groupInfo = reinterpret_cast<const TOKEN_GROUPS *>(groups.constData());
    for (DWORD i = 0; i < groupInfo->GroupCount; ++i) {
        if ((groupInfo->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            identity.logonSid = QByteArray(static_cast<const char *>(groupInfo->Groups[i].Sid),
                                           GetLengthSid(groupInfo->Groups[i].Sid));
            break;
        }
    }
    if (identity.logonSid.isEmpty()) {
        SetLastError(ERROR_NO_SUCH_LOGON_SESSION);
        return false;
    }
    TOKEN_STATISTICS statistics = {};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &identity.session, sizeof(identity.session), &size)
        || !GetTokenInformation(token.get(), TokenStatistics, &statistics, sizeof(statistics), &size))
        return false;
    identity.authentication = statistics.AuthenticationId;
    return true;
}

bool sameIdentity(const Identity &left, const Identity &right)
{
    return left.userSid == right.userSid && left.logonSid == right.logonSid
        && left.session == right.session
        && left.authentication.HighPart == right.authentication.HighPart
        && left.authentication.LowPart == right.authentication.LowPart;
}

QString sidString(const QByteArray &sid)
{
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(const_cast<char *>(sid.constData()), &text))
        return {};
    LocalAllocation allocation{ text };
    return QString::fromWCharArray(text);
}

QString endpointName(const Identity &identity, const QString &userSid, const QString &logonSid,
                     const QString &instanceNamespace)
{
    // Token-derived identity, never USERNAME or another mutable environment
    // variable. Hashing keeps even maximum-length SIDs below pipe name limits.
    auto identityName = QStringLiteral("%1/%2/%3/%4/%5")
        .arg(userSid, logonSid).arg(identity.session)
        .arg(quint32(identity.authentication.HighPart)).arg(identity.authentication.LowPart);
    if (!instanceNamespace.isEmpty())
        identityName.prepend(instanceNamespace + QLatin1Char('\n'));
    return QStringLiteral("io.github.trans.Trans.") + QString::fromLatin1(
        QCryptographicHash::hash(identityName.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool verifyMutexSecurity(HANDLE mutex, const Identity &identity)
{
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD error = GetSecurityInfo(mutex, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    LocalAllocation allocation{ descriptor };
    if (error != ERROR_SUCCESS) {
        SetLastError(error);
        return false;
    }
    void *rawAce = nullptr;
    if (!owner || !EqualSid(owner, const_cast<char *>(identity.userSid.constData()))
        || !dacl || !IsValidAcl(dacl) || dacl->AceCount != 1 || !GetAce(dacl, 0, &rawAce)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    const auto ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE
        || !EqualSid(&ace->SidStart, const_cast<char *>(identity.logonSid.constData()))) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    return true;
}

bool verifyPeer(HANDLE pipe, bool serverEnd, const Identity &identity)
{
    ULONG processId = 0;
    if (!(serverEnd ? GetNamedPipeClientProcessId(pipe, &processId)
                   : GetNamedPipeServerProcessId(pipe, &processId)))
        return false;
    if (!processId) {
        SetLastError(ERROR_PIPE_NOT_CONNECTED);
        return false;
    }
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    Identity peer;
    if (!process || !readIdentity(process.get(), peer))
        return false;
    if (!sameIdentity(identity, peer)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    return true;
}

quint32 commandValue(AppCommand command)
{
    switch (command) {
    case AppCommand::ShowTranslation: return 1;
    case AppCommand::TranslateSelection: return 2;
    case AppCommand::TranslateScreenshot: return 3;
    case AppCommand::ShowSettings: return 4;
    }
    return 0;
}

bool decodeCommand(quint32 value, AppCommand &command)
{
    switch (value) {
    case 1: command = AppCommand::ShowTranslation; return true;
    case 2: command = AppCommand::TranslateSelection; return true;
    case 3: command = AppCommand::TranslateScreenshot; return true;
    case 4: command = AppCommand::ShowSettings; return true;
    default: return false;
    }
}

// One request per connection; big-endian magic, version, type, payload size,
// and one fixed 32-bit value. No QDataStream allocations based on peer input.
QByteArray frame(FrameType type, quint32 value)
{
    QByteArray bytes(frameSize, '\0');
    qToBigEndian(frameMagic, bytes.data());
    bytes[4] = char(protocolVersion);
    bytes[5] = char(type);
    qToBigEndian<quint16>(4, bytes.data() + 6);
    qToBigEndian(value, bytes.data() + 8);
    return bytes;
}

bool decodeFrame(const QByteArray &bytes, FrameType expected, quint32 &value)
{
    if (bytes.size() != frameSize || qFromBigEndian<quint32>(bytes.constData()) != frameMagic
        || quint8(bytes[4]) != protocolVersion || quint8(bytes[5]) != quint8(expected)
        || qFromBigEndian<quint16>(bytes.constData() + 6) != 4)
        return false;
    value = qFromBigEndian<quint32>(bytes.constData() + 8);
    return true;
}

enum class ReadResult { Complete, Closed, Timeout, Invalid };

ReadResult readFrame(QLocalSocket &socket, const QDeadlineTimer &deadline, QByteArray &bytes)
{
    bytes.clear();
    bytes.reserve(frameSize + 1);
    while (!deadline.hasExpired()) {
        if (socket.bytesAvailable()) {
            bytes += socket.read(frameSize + 1 - bytes.size());
            if (bytes.size() > frameSize || (bytes.size() == frameSize && socket.bytesAvailable()))
                return ReadResult::Invalid;
            if (bytes.size() == frameSize)
                return ReadResult::Complete;
        }
        if (socket.state() != QLocalSocket::ConnectedState)
            return ReadResult::Closed;
        if (!socket.waitForReadyRead(int(deadline.remainingTime())))
            return deadline.hasExpired() ? ReadResult::Timeout : ReadResult::Closed;
    }
    return ReadResult::Timeout;
}

class WindowsInstanceChannel final : public InstanceChannel {
    class Connection final : public QObject {
    public:
        Connection(WindowsInstanceChannel *owner, QLocalSocket *socket)
            : QObject(owner), m_owner(owner), m_socket(socket), m_deadline(readinessTimeoutMs)
        {
            m_socket->setParent(this);
            m_socket->setReadBufferSize(frameSize + 1);
            m_timer.setSingleShot(true);
            connect(&m_timer, &QTimer::timeout, this, [this] {
                if (m_closing || !m_received)
                    dispose();
                else
                    reply(Reply::NotReady);
            });
            connect(m_socket, &QLocalSocket::readyRead, this, [this] { receive(); });
            connect(m_socket, &QLocalSocket::disconnected, this, [this] { dispose(); });
            connect(m_socket, &QLocalSocket::errorOccurred, this,
                    [this](QLocalSocket::LocalSocketError) { dispose(); });
        }

        void start()
        {
            m_timer.start(readinessTimeoutMs);
            const auto hello = frame(FrameType::Hello, 0);
            if (m_socket->write(hello) != hello.size()) {
                dispose();
                return;
            }
            receive();
        }

        void dispatch()
        {
            if (m_disposed || m_closing || m_dispatched || !m_received || !m_owner->m_handler)
                return;
            if (m_deadline.hasExpired()) {
                reply(Reply::NotReady);
                return;
            }
            if (m_socket->state() != QLocalSocket::ConnectedState) {
                dispose();
                return;
            }
            if (m_socket->bytesAvailable()) {
                reply(Reply::Invalid);
                return;
            }
            // Mark before invoking application code: it may run a nested event
            // loop, replace the handler, or destroy the whole channel.
            m_dispatched = true;
            m_timer.stop();
            QPointer<Connection> alive(this);
            const auto handler = m_owner->m_handler;
            handler(m_command);
            if (alive && !m_disposed)
                reply(Reply::Dispatched);
        }

    private:
        void receive()
        {
            if (m_disposed || !m_socket->bytesAvailable())
                return;
            if (m_closing || m_received) {
                // A later frame never becomes a second command, even if the
                // first already dispatched before these extra bytes arrived.
                if (m_closing || m_dispatched)
                    dispose();
                else
                    reply(Reply::Invalid);
                return;
            }
            m_input += m_socket->read(frameSize + 1 - m_input.size());
            if (m_input.size() > frameSize || m_socket->bytesAvailable()) {
                reply(Reply::Invalid);
                return;
            }
            if (m_input.size() != frameSize)
                return;
            quint32 value = 0;
            if (!decodeFrame(m_input, FrameType::Command, value) || !decodeCommand(value, m_command)) {
                reply(Reply::Invalid);
                return;
            }
            m_received = true;
            dispatch();
        }

        void reply(Reply result)
        {
            if (m_disposed || m_closing)
                return;
            m_closing = true;
            m_timer.start(replyDrainTimeoutMs);
            const auto response = frame(FrameType::Result, quint32(result));
            if (m_socket->write(response) != response.size()) {
                dispose();
                return;
            }
            m_socket->disconnectFromServer();
        }

        void dispose()
        {
            if (m_disposed)
                return;
            m_disposed = true;
            m_timer.stop();
            m_owner->m_connections.removeAll(this);
            m_socket->abort();
            deleteLater();
        }

        WindowsInstanceChannel *m_owner;
        QLocalSocket *m_socket;
        QTimer m_timer;
        QDeadlineTimer m_deadline;
        QByteArray m_input;
        AppCommand m_command = AppCommand::ShowTranslation;
        bool m_received = false;
        bool m_dispatched = false;
        bool m_closing = false;
        bool m_disposed = false;
    };

public:
    WindowsInstanceChannel(QString instanceNamespace, QObject *parent)
        : InstanceChannel(parent), m_namespace(std::move(instanceNamespace))
    {
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(maximumConnections);
        m_server.setListenBacklogSize(8);
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            QPointer<WindowsInstanceChannel> alive(this);
            while (m_server.hasPendingConnections()) {
                auto *socket = m_server.nextPendingConnection();
                if (!socket)
                    break;
                if (m_connections.size() >= maximumConnections
                    || !verifyPeer(reinterpret_cast<HANDLE>(socket->socketDescriptor()), true, m_identity)) {
                    socket->abort();
                    delete socket;
                    continue;
                }
                auto *connection = new Connection(this, socket);
                m_connections.append(connection);
                connection->start();
                if (!alive)
                    return;
            }
        });
    }

    ~WindowsInstanceChannel() override
    {
        m_handler = {};
        m_server.close();
        // Close accepted pipes before releasing the election. Do not leave
        // deleteLater() work depending on a GUI event loop that is exiting.
        const auto connections = std::exchange(m_connections, {});
        for (const auto &connection : connections) {
            if (connection)
                delete connection.data();
        }
        if (m_ownsMutex)
            ReleaseMutex(m_mutex.get());
    }

    InstanceStartResult start(AppCommand command) override
    {
        if (m_started)
            return failure(PlatformErrorCode::Conflict, QStringLiteral("The instance channel has already been started."));
        m_started = true;
        if (QThread::currentThread() != thread())
            return failure(PlatformErrorCode::Failed, QStringLiteral("The instance channel must run on its owning thread."));
        const quint32 value = commandValue(command);
        if (!value)
            return failure(PlatformErrorCode::Failed, QStringLiteral("Unknown application command."));
        if (!readIdentity(GetCurrentProcess(), m_identity))
            return nativeFailure(QStringLiteral("Cannot determine the Windows user and logon session"));
        const QString userSid = sidString(m_identity.userSid);
        const QString logonSid = sidString(m_identity.logonSid);
        if (userSid.isEmpty() || logonSid.isEmpty())
            return nativeFailure(QStringLiteral("Cannot encode the Windows user and logon session"));

        m_name = endpointName(m_identity, userSid, logonSid, m_namespace);
        m_pipePath = QStringLiteral("\\\\.\\pipe\\") + m_name;
        const QString mutexName = QStringLiteral("Local\\") + m_name + QStringLiteral(".mutex");

        // Qt 6.5+ UserAccessOption grants FILE_ALL_ACCESS to TokenUser on
        // Windows, not to Everyone. A native first instance additionally fixes
        // the shared pipe DACL to the logon SID, excluding this user's OTHER
        // logins and network logons. The same protected DACL secures the mutex.
        const QString sddl = QStringLiteral("O:%1D:P(A;;GA;;;%2)").arg(userSid, logonSid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr))
            return nativeFailure(QStringLiteral("Cannot secure the instance channel"));
        LocalAllocation security{ descriptor };
        SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
        m_mutex.reset(CreateMutexExW(&attributes, reinterpret_cast<LPCWSTR>(mutexName.utf16()), 0,
                                    SYNCHRONIZE | MUTEX_MODIFY_STATE | READ_CONTROL));
        if (!m_mutex)
            return nativeFailure(QStringLiteral("Cannot open the instance election mutex"));
        if (!verifyMutexSecurity(m_mutex.get(), m_identity))
            return nativeFailure(QStringLiteral("The instance mutex has an unexpected owner or access policy"));

        QDeadlineTimer deadline(forwardingTimeoutMs);
        while (!deadline.hasExpired()) {
            const DWORD elected = WaitForSingleObject(m_mutex.get(), 0);
            if (elected == WAIT_OBJECT_0 || elected == WAIT_ABANDONED) {
                m_ownsMutex = true;
                return listen(attributes, deadline);
            }
            if (elected != WAIT_TIMEOUT)
                return nativeFailure(QStringLiteral("Cannot elect the primary instance"));

            // QLocalSocket::connectToServer() on Windows repeatedly waits five
            // seconds on busy pipes, with no overall deadline. Open once without
            // waiting, then let QLocalSocket own the overlapped pipe instead.
            NativeHandle pipe(CreateFileW(reinterpret_cast<LPCWSTR>(m_pipePath.utf16()),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            if (!pipe) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY
                    && error != ERROR_PIPE_NOT_CONNECTED && error != ERROR_NO_DATA)
                    return nativeFailure(QStringLiteral("Cannot connect to the running Trans instance"), error);
                pause(deadline);
                continue;
            }
            if (!verifyPeer(pipe.get(), false, m_identity)) {
                const DWORD error = GetLastError();
                if (error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BROKEN_PIPE
                    || error == ERROR_NO_DATA || error == ERROR_INVALID_PARAMETER) {
                    pipe.reset();
                    pause(deadline);
                    continue;
                }
                return nativeFailure(QStringLiteral("Cannot verify the running Trans instance's logon identity"), error);
            }
            QLocalSocket socket;
            socket.setReadBufferSize(frameSize + 1);
            if (!socket.setSocketDescriptor(reinterpret_cast<qintptr>(pipe.get()), QLocalSocket::ConnectedState))
                return failure(PlatformErrorCode::Failed, socket.errorString());
            pipe.release();
            QByteArray incoming;
            const auto helloResult = readFrame(socket, deadline, incoming);
            if (helloResult == ReadResult::Closed) {
                // A reservation pipe closed, or the primary exited before any
                // command was sent. These are the only safe delivery retries.
                socket.abort();
                pause(deadline);
                continue;
            }
            if (helloResult == ReadResult::Timeout)
                return timeout();
            quint32 hello = 0;
            if (helloResult != ReadResult::Complete || !decodeFrame(incoming, FrameType::Hello, hello) || hello != 0)
                return failure(PlatformErrorCode::Conflict, QStringLiteral("The instance endpoint uses an invalid or incompatible protocol."));
            if (deadline.hasExpired())
                return timeout();

            const auto request = frame(FrameType::Command, value);
            // From this write onward, even a failed/partial write or missing ACK
            // is ambiguous. Never re-elect or replay this command.
            if (socket.write(request) != request.size())
                return failure(PlatformErrorCode::Unavailable, QStringLiteral("The command could not be delivered; it will not be retried."));
            // waitForReadyRead also services Qt's overlapped writer. Waiting
            // separately for bytesWritten could miss an ACK already buffered
            // when a fast primary has closed its end of the pipe.
            const auto responseResult = readFrame(socket, deadline, incoming);
            if (responseResult == ReadResult::Timeout)
                return timeout();
            quint32 response = 0;
            if (responseResult != ReadResult::Complete || !decodeFrame(incoming, FrameType::Result, response))
                return failure(PlatformErrorCode::Unavailable,
                    QStringLiteral("The primary did not acknowledge command dispatch; the command will not be retried."));
            if (response == quint32(Reply::Dispatched))
                return {InstanceRole::Forwarded, {}};
            if (response == quint32(Reply::NotReady))
                return failure(PlatformErrorCode::Timeout, QStringLiteral("Trans did not become ready before the command timed out."));
            return failure(PlatformErrorCode::Failed, QStringLiteral("The primary rejected the instance command."));
        }
        return timeout();
    }

    void setReady(CommandHandler handler) override
    {
        if (!m_primary || QThread::currentThread() != thread())
            return;
        m_handler = std::move(handler);
        const auto connections = m_connections;
        QPointer<WindowsInstanceChannel> alive(this);
        for (const auto &connection : connections) {
            if (connection)
                connection->dispatch();
            if (!alive)
                return;
        }
    }

private:
    static void pause(const QDeadlineTimer &deadline)
    {
        const auto remaining = deadline.remainingTime();
        if (remaining > 0)
            QThread::msleep(static_cast<unsigned long>(std::min<qint64>(retryIntervalMs, remaining)));
    }

    static InstanceStartResult timeout()
    {
        return failure(PlatformErrorCode::Timeout,
            QStringLiteral("Timed out contacting the running Trans instance; any sent command will not be retried."));
    }

    InstanceStartResult listen(SECURITY_ATTRIBUTES &attributes, const QDeadlineTimer &deadline)
    {
        // QLocalServer alone permits another Windows server to listen to the
        // same name. Atomically reserve the FIRST instance before Qt creates its
        // listeners. Never removeServer(), join, or disconnect a foreign pipe.
        NativeHandle reservation;
        DWORD error = ERROR_SUCCESS;
        while (!deadline.hasExpired()) {
            reservation.reset(CreateNamedPipeW(reinterpret_cast<LPCWSTR>(m_pipePath.utf16()),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 0, 0, 3000, &attributes));
            if (reservation)
                break;
            error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_PIPE_BUSY)
                break;
            // After an abandoned mutex, clients can still hold handles to old
            // disconnected instances. Wait for natural cleanup, bounded by the
            // same startup deadline; never forcibly delete a possibly live pipe.
            pause(deadline);
        }
        if (!reservation) {
            ReleaseMutex(m_mutex.get());
            m_ownsMutex = false;
            if (error == ERROR_SUCCESS)
                return timeout();
            return error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY
                ? failure(PlatformErrorCode::Conflict, QStringLiteral("The instance pipe is already owned or is not accessible."))
                : nativeFailure(QStringLiteral("Cannot reserve the instance pipe"), error);
        }
        if (!m_server.listen(m_name)) {
            const auto message = m_server.errorString();
            m_server.close();
            reservation.reset();
            ReleaseMutex(m_mutex.get());
            m_ownsMutex = false;
            return failure(PlatformErrorCode::Unavailable, QStringLiteral("Cannot listen for instance commands: %1").arg(message));
        }
        // A secondary can race with this temporary instance. The hello handshake
        // ensures it has sent no command and can safely reconnect when it closes.
        reservation.reset();
        m_primary = true;
        return {InstanceRole::Primary, {}};
    }

    QLocalServer m_server;
    QList<QPointer<Connection>> m_connections;
    CommandHandler m_handler;
    NativeHandle m_mutex;
    Identity m_identity;
    QString m_namespace;
    QString m_name;
    QString m_pipePath;
    bool m_started = false;
    bool m_ownsMutex = false;
    bool m_primary = false;
};

} // namespace

QString windowsInstanceEndpointName(const QString &instanceNamespace)
{
    Identity identity;
    if (!readIdentity(GetCurrentProcess(), identity))
        return {};
    const auto userSid = sidString(identity.userSid);
    const auto logonSid = sidString(identity.logonSid);
    if (userSid.isEmpty() || logonSid.isEmpty())
        return {};
    return endpointName(identity, userSid, logonSid, instanceNamespace);
}

std::unique_ptr<InstanceChannel> createWindowsInstanceChannel(const QString &instanceNamespace, QObject *parent)
{
    return std::make_unique<WindowsInstanceChannel>(instanceNamespace, parent);
}

std::unique_ptr<InstanceChannel> createInstanceChannel(QObject *parent)
{
    return createWindowsInstanceChannel({}, parent);
}

} // namespace Trans
