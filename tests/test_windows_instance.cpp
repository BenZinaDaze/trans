#include "platform/windows/instance_channel_windows.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>
#include <QTextStream>
#include <QUuid>
#include <QtEndian>

#include <windows.h>

#include <cstdio>

using namespace Trans;

namespace {

constexpr int forwardedExit = 0;
constexpr int primaryExit = 2;
constexpr int errorExit = 3;
constexpr int invalidFrameExit = 10;
constexpr int notReadyExit = 11;
constexpr int crashedAfterDispatchExit = 77;
constexpr int helperFailureExit = 90;

void announce(const QString &line)
{
    QTextStream(stdout) << line << '\n' << Qt::flush;
}

QByteArray commandFrame(quint32 command)
{
    auto bytes = QByteArray::fromHex("54524e530102000400000000");
    qToBigEndian(command, bytes.data() + 8);
    return bytes;
}

QByteArray readReply(QLocalSocket &socket)
{
    QDeadlineTimer deadline(30000);
    QByteArray bytes;
    while (bytes.size() < 12 && !deadline.hasExpired()) {
        bytes += socket.read(12 - bytes.size());
        if (bytes.size() == 12)
            break;
        if (!socket.waitForReadyRead(int(deadline.remainingTime()))) {
            bytes += socket.read(12 - bytes.size());
            break;
        }
    }
    return bytes;
}

bool sendBytes(QLocalSocket &socket, const QByteArray &bytes)
{
    if (socket.write(bytes) != bytes.size())
        return false;
    QDeadlineTimer deadline(5000);
    while (socket.bytesToWrite() && !deadline.hasExpired()) {
        if (!socket.waitForBytesWritten(int(deadline.remainingTime())))
            return socket.bytesToWrite() == 0;
    }
    return socket.bytesToWrite() == 0;
}

int rawClient(const QStringList &arguments)
{
    QLocalSocket socket;
    socket.setReadBufferSize(13);
    const auto name = windowsInstanceEndpointName(arguments.value(2));
    if (name.isEmpty())
        return helperFailureExit;
    socket.connectToServer(name);
    if (!socket.waitForConnected(5000)
        || readReply(socket) != QByteArray::fromHex("54524e530101000400000000"))
        return helperFailureExit;
    const auto bytes = QByteArray::fromHex(arguments.value(3).toLatin1());
    const auto mode = arguments.value(4);
    if (mode == QStringLiteral("fragment")) {
        if (!sendBytes(socket, bytes.first(5)))
            return helperFailureExit;
        announce(QStringLiteral("PARTIAL"));
        if (std::getchar() == EOF || !sendBytes(socket, bytes.sliced(5)))
            return helperFailureExit;
    } else if (!sendBytes(socket, bytes)) {
        return helperFailureExit;
    }
    announce(QStringLiteral("SENT"));
    if (mode == QStringLiteral("truncate")) {
        socket.abort();
        return 0;
    }
    if (mode == QStringLiteral("queued")) {
        if (socket.bytesAvailable() || socket.waitForReadyRead(150)
            || socket.state() != QLocalSocket::ConnectedState)
            return helperFailureExit;
        announce(QStringLiteral("WAITING"));
    }
    const auto reply = readReply(socket);
    if (reply.size() != 12 || reply.first(8) != QByteArray::fromHex("54524e5301030004"))
        return helperFailureExit;
    const auto result = qFromBigEndian<quint32>(reply.constData() + 8);
    if (result == 0)
        return forwardedExit;
    if (result == 1)
        return invalidFrameExit;
    if (result == 2)
        return notReadyExit;
    return helperFailureExit;
}

int childMain(QCoreApplication &app, const QStringList &arguments)
{
    if (arguments.value(1) == QStringLiteral("--ipc-raw"))
        return rawClient(arguments);
    auto channel = createWindowsInstanceChannel(arguments.value(2));
    if (arguments.value(1) == QStringLiteral("--ipc-forward")) {
        const auto command = static_cast<AppCommand>(arguments.value(3).toInt());
        const auto result = channel->start(command);
        if (result.role == InstanceRole::Forwarded)
            return forwardedExit;
        if (result.role == InstanceRole::Primary)
            return primaryExit;
        return errorExit;
    }
    if (arguments.value(1) != QStringLiteral("--ipc-crash-primary")
        || channel->start(AppCommand::ShowTranslation).role != InstanceRole::Primary)
        return helperFailureExit;
    channel->setReady([](AppCommand command) {
        announce(QStringLiteral("DISPATCH %1").arg(int(command)));
        // Keep delivery ambiguous: terminate after the real handler ran but
        // before InstanceChannel can send its acknowledgment or release mutex.
        ExitProcess(crashedAfterDispatchExit);
    });
    announce(QStringLiteral("PRIMARY"));
    return app.exec();
}

class Child final {
public:
    ~Child()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }

    bool start(const QStringList &arguments)
    {
        process.start(QCoreApplication::applicationFilePath(), arguments);
        return process.waitForStarted(5000);
    }

    bool waitFor(const QByteArray &marker)
    {
        return QTest::qWaitFor([&] {
            output += process.readAllStandardOutput();
            return output.contains(marker + '\n');
        }, 5000);
    }

    bool finish(int timeout = 30000)
    {
        const bool finished = QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, timeout);
        output += process.readAllStandardOutput();
        return finished;
    }

    QProcess process;
    QByteArray output;
};

QString isolatedNamespace()
{
    return QStringLiteral("trans-instance-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QStringList forwardArguments(const QString &name, AppCommand command)
{
    return { QStringLiteral("--ipc-forward"), name, QString::number(int(command)) };
}

QStringList rawArguments(const QString &name, const QByteArray &bytes, const QString &mode = {})
{
    return { QStringLiteral("--ipc-raw"), name, QString::fromLatin1(bytes.toHex()), mode };
}

} // namespace

class WindowsInstanceTest final : public QObject {
    Q_OBJECT
private slots:
    void queuesUntilReady()
    {
        const auto name = isolatedNamespace();
        auto channel = createWindowsInstanceChannel(name);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        QList<AppCommand> commands;
        Child client;
        QVERIFY(client.start(rawArguments(name, commandFrame(4), QStringLiteral("queued"))));
        // The peer confirms its complete frame was accepted and no ACK arrived
        // during a real bounded pipe read, before readiness is installed.
        QVERIFY(client.waitFor("WAITING"));
        QCOMPARE(client.process.state(), QProcess::Running);
        channel->setReady([&](AppCommand command) { commands.append(command); });
        QVERIFY(client.finish());
        QCOMPARE(client.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(client.process.exitCode(), forwardedExit);
        QCOMPARE(commands, QList<AppCommand>{AppCommand::ShowSettings});
    }

    void dispatchesAllCommands()
    {
        const auto name = isolatedNamespace();
        auto channel = createWindowsInstanceChannel(name);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        QList<AppCommand> commands;
        channel->setReady([&](AppCommand command) { commands.append(command); });
        const QList<AppCommand> expected{AppCommand::ShowTranslation, AppCommand::TranslateSelection,
                                        AppCommand::TranslateScreenshot, AppCommand::ShowSettings};
        for (qsizetype i = 0; i < expected.size(); ++i) {
            Child client;
            QVERIFY(client.start(forwardArguments(name, expected[i])));
            QVERIFY(client.finish());
            QCOMPARE(client.process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(client.process.exitCode(), forwardedExit);
            QCOMPARE(commands, expected.first(i + 1));
        }
    }

    void waitsForCompleteFrame()
    {
        const auto name = isolatedNamespace();
        auto channel = createWindowsInstanceChannel(name);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        QList<AppCommand> commands;
        channel->setReady([&](AppCommand command) { commands.append(command); });
        Child client;
        QVERIFY(client.start(rawArguments(name, commandFrame(2), QStringLiteral("fragment"))));
        QVERIFY(client.waitFor("PARTIAL"));
        QCOMPARE(client.process.state(), QProcess::Running);
        QVERIFY(commands.isEmpty());
        QCOMPARE(client.process.write("continue\n"), qint64(9));
        QVERIFY(client.finish());
        QCOMPARE(client.process.exitCode(), forwardedExit);
        QCOMPARE(commands, QList<AppCommand>{AppCommand::TranslateSelection});
    }

    void rejectsMalformedFrames_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("exitCode");
        QTest::newRow("unknown-command") << commandFrame(99) << QString() << invalidFrameExit;
        auto version = commandFrame(1);
        version[4] = 2;
        QTest::newRow("unsupported-version") << version << QString() << invalidFrameExit;
        auto oversized = commandFrame(1);
        oversized[6] = char(0xff);
        oversized[7] = char(0xff);
        QTest::newRow("oversized-payload") << oversized << QString() << invalidFrameExit;
        QTest::newRow("multiple-requests") << commandFrame(1) + commandFrame(4) << QString() << invalidFrameExit;
        QTest::newRow("truncated-frame") << commandFrame(1).first(5) << QStringLiteral("truncate") << 0;
    }

    void rejectsMalformedFrames()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(QString, mode);
        QFETCH(int, exitCode);
        const auto name = isolatedNamespace();
        auto channel = createWindowsInstanceChannel(name);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        QList<AppCommand> commands;
        channel->setReady([&](AppCommand command) { commands.append(command); });
        Child malformed;
        QVERIFY(malformed.start(rawArguments(name, bytes, mode)));
        QVERIFY(malformed.finish());
        QCOMPARE(malformed.process.exitCode(), exitCode);
        Child valid;
        QVERIFY(valid.start(forwardArguments(name, AppCommand::TranslateScreenshot)));
        QVERIFY(valid.finish());
        QCOMPARE(valid.process.exitCode(), forwardedExit);
        QCOMPARE(commands, QList<AppCommand>{AppCommand::TranslateScreenshot});
    }

    void expiresUndispatchedRequests()
    {
        const auto name = isolatedNamespace();
        auto channel = createWindowsInstanceChannel(name);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        Child expired;
        QVERIFY(expired.start(rawArguments(name, commandFrame(4))));
        QVERIFY(expired.waitFor("SENT"));
        QVERIFY(expired.finish());
        QCOMPARE(expired.process.exitCode(), notReadyExit);
        QList<AppCommand> commands;
        channel->setReady([&](AppCommand command) { commands.append(command); });
        Child valid;
        QVERIFY(valid.start(forwardArguments(name, AppCommand::ShowTranslation)));
        QVERIFY(valid.finish());
        QCOMPARE(valid.process.exitCode(), forwardedExit);
        QCOMPARE(commands, QList<AppCommand>{AppCommand::ShowTranslation});
    }

    void doesNotReplayAmbiguousDispatchAndRecoversAbandonment()
    {
        const auto name = isolatedNamespace();
        Child primary;
        QVERIFY(primary.start({QStringLiteral("--ipc-crash-primary"), name}));
        QVERIFY(primary.waitFor("PRIMARY"));
        // Retain a handle so the abandoned kernel mutex survives both children;
        // the replacement must acquire WAIT_ABANDONED, not create a fresh mutex.
        const auto mutexName = QStringLiteral("Local\\") + windowsInstanceEndpointName(name)
            + QStringLiteral(".mutex");
        HANDLE retainedMutex = OpenMutexW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
        QVERIFY(retainedMutex);
        const auto closeMutex = qScopeGuard([&] { CloseHandle(retainedMutex); });
        Child forwarder;
        QVERIFY(forwarder.start(forwardArguments(name, AppCommand::TranslateSelection)));
        QVERIFY(primary.finish());
        QCOMPARE(primary.process.exitCode(), crashedAfterDispatchExit);
        QCOMPARE(primary.output.count("DISPATCH "), 1);
        QVERIFY(forwarder.finish());
        // Re-election or a replay would return Primary or Forwarded, not Error.
        QCOMPARE(forwarder.process.exitCode(), errorExit);
        auto replacement = createWindowsInstanceChannel(name);
        QCOMPARE(replacement->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        QList<AppCommand> commands;
        replacement->setReady([&](AppCommand command) { commands.append(command); });
        Child fresh;
        QVERIFY(fresh.start(forwardArguments(name, AppCommand::ShowSettings)));
        QVERIFY(fresh.finish());
        QCOMPARE(fresh.process.exitCode(), forwardedExit);
        QCOMPARE(commands, QList<AppCommand>{AppCommand::ShowSettings});
    }

    void rejectsUnknownApplicationCommand()
    {
        const auto name = isolatedNamespace();
        auto invalid = createWindowsInstanceChannel(name);
        QCOMPARE(invalid->start(static_cast<AppCommand>(99)).role, InstanceRole::Error);
        auto valid = createWindowsInstanceChannel(name);
        QCOMPARE(valid->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.value(1).startsWith(QStringLiteral("--ipc-")))
        return childMain(app, arguments);
    WindowsInstanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_windows_instance.moc"
