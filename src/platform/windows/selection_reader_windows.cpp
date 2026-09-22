#include "platform/selection_reader.h"
#include "selection_protocol_windows.h"
#include "source_context_windows.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <string>

namespace Trans {
namespace {

using SelectionProtocol::Status;

QString helperPath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/trans_selection_helper.exe"));
}

PlatformError resultError(Status status)
{
    switch (status) {
    case Status::NoSelection:
        return {PlatformErrorCode::NoSelection,
                QStringLiteral("请先在其他应用中选中文本，再按翻译快捷键。")};
    case Status::Unsupported:
        return {PlatformErrorCode::Unsupported,
                QStringLiteral("此控件不支持通过 Windows UI Automation 读取选区，请使用截图翻译。")};
    case Status::PermissionDenied:
        return {PlatformErrorCode::PermissionDenied,
                QStringLiteral("Windows 不允许读取此选区（密码控件、受保护窗口或更高权限的应用）。请使用非敏感内容或截图翻译。")};
    case Status::Cancelled:
        return {PlatformErrorCode::Cancelled, QStringLiteral("来源窗口或焦点已改变，选区读取已取消。")};
    case Status::Failed:
    case Status::Success:
        return {PlatformErrorCode::Failed, QStringLiteral("Windows UI Automation 选区读取失败，请重试或使用截图翻译。")};
    }
    return {PlatformErrorCode::Failed, QStringLiteral("选区读取失败。")};
}

class WindowsSelectionJob final : public SelectionJob {
public:
    WindowsSelectionJob(SourceContextPtr source, QObject *owner)
        : SelectionJob(owner), m_source(std::move(source)),
          m_context(std::dynamic_pointer_cast<const WindowsSourceContext>(m_source))
    {
        m_deadline.setSingleShot(true);
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            reject({PlatformErrorCode::Timeout,
                    QStringLiteral("读取选区超时。目标应用没有及时响应，请重试或使用截图翻译。")});
        });
        m_contextWatch.setInterval(40);
        connect(&m_contextWatch, &QTimer::timeout, this, [this] {
            if (!contextMatches()) reject(resultError(Status::Cancelled));
        });
        QTimer::singleShot(0, this, [this] { start(); });
    }

    ~WindowsSelectionJob() override { dispose(); }

    void cancel() override
    {
        reject({PlatformErrorCode::Cancelled, QStringLiteral("选区读取已取消。")});
    }

private:
    bool contextMatches() const
    {
        if (!m_context || !m_context->window || !IsWindow(m_context->window)
            || GetForegroundWindow() != m_context->window
            || m_context->processId == GetCurrentProcessId()) return false;
        DWORD processId = 0;
        const DWORD threadId = GetWindowThreadProcessId(m_context->window, &processId);
        return threadId == m_context->threadId && processId == m_context->processId
            && (!m_sourceProcess || WaitForSingleObject(m_sourceProcess, 0) == WAIT_TIMEOUT);
    }

    bool prepareContainment()
    {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return false;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return false;
        m_attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes));
        if (!m_attributes || !InitializeProcThreadAttributeList(m_attributes, 1, 0, &bytes)) return false;
        m_attributesInitialized = true;
        // Assignment is atomic with CreateProcess: even a parent crash during
        // startup cannot leave an uncontained helper waiting on its input pipe.
        return UpdateProcThreadAttribute(m_attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                         &m_job, sizeof(m_job), nullptr, nullptr);
    }

    void start()
    {
        if (finished()) return;
        if (!m_context) {
            reject(resultError(Status::NoSelection));
            return;
        }
        if (!contextMatches()) {
            reject(resultError(Status::Cancelled));
            return;
        }
        const QString program = helperPath();
        if (!QFileInfo(program).isFile()) {
            reject({PlatformErrorCode::Unavailable,
                    QStringLiteral("缺少 trans_selection_helper.exe，请重新安装完整的 Windows 程序包。")});
            return;
        }
        m_sourceProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE, m_context->processId);
        if (!m_sourceProcess) {
            reject(resultError(GetLastError() == ERROR_ACCESS_DENIED ? Status::PermissionDenied : Status::Failed));
            return;
        }
        if (!prepareContainment()) {
            reject({PlatformErrorCode::Unavailable,
                    QStringLiteral("无法创建受隔离的 Windows 选区读取进程。请检查系统安全策略。")});
            return;
        }
        m_request = {QUuid::createUuid().toString(QUuid::Id128),
                     static_cast<quint64>(reinterpret_cast<quintptr>(m_context->window)),
                     m_context->processId, m_context->threadId, GetCurrentProcessId()};
        m_nativeProgram = program.toStdWString();
        // A running QProcess must never be destroyed on the GUI thread: its
        // destructor waits for exit. It instead reaps itself after finished;
        // dispose() closes the kill-on-close job and never waits for a provider.
        auto *process = new QProcess;
        m_process = process;
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) process->deleteLater();
        });
        process->setProcessChannelMode(QProcess::SeparateChannels);
        process->setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments *arguments) {
            m_startup.StartupInfo = *arguments->startupInfo;
            m_startup.StartupInfo.cb = sizeof(m_startup);
            m_startup.lpAttributeList = m_attributes;
            arguments->startupInfo = &m_startup.StartupInfo;
            arguments->applicationName = m_nativeProgram.c_str();
            arguments->flags |= EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
        });
        connect(process, &QProcess::started, this, [this] {
            if (!m_process || finished()) return;
            if (!contextMatches()) {
                reject(resultError(Status::Cancelled));
                return;
            }
            const QByteArray request = SelectionProtocol::encodeRequest(m_request);
            if (m_process->write(request) != request.size()) {
                reject(resultError(Status::Failed));
                return;
            }
            m_process->closeWriteChannel();
        });
        connect(process, &QProcess::readyReadStandardOutput, this, [this] { drainOutput(); });
        connect(process, &QProcess::readyReadStandardError, this, [this] { drainOutput(); });
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                reject({PlatformErrorCode::Unavailable, QStringLiteral("无法启动 Windows 选区读取程序。请检查安装和安全策略。")});
            else
                reject(resultError(Status::Failed));
        });
        connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            if (finished()) return;
            drainOutput();
            if (finished()) return;
            if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
                reject(resultError(Status::Failed));
                return;
            }
            if (!contextMatches()) {
                reject(resultError(Status::Cancelled));
                return;
            }
            Status status = Status::Failed;
            QString text;
            if (!SelectionProtocol::parseResponse(m_stdout, m_request.id, status, text)) {
                reject({PlatformErrorCode::Failed, QStringLiteral("选区读取程序返回了无效的数据。")});
                return;
            }
            if (status != Status::Success) {
                reject(resultError(status));
                return;
            }
            dispose();
            succeed({text, m_source});
        });
        m_deadline.start(SelectionProtocol::TimeoutMs);
        m_contextWatch.start();
        process->start(program, {}, QIODevice::ReadWrite);
    }

    void drainOutput()
    {
        if (!m_process || finished()) return;
        m_process->setReadChannel(QProcess::StandardOutput);
        const qint64 stdoutBytes = m_process->bytesAvailable();
        if (stdoutBytes > SelectionProtocol::MaxResponseBytes - m_stdout.size()) {
            reject({PlatformErrorCode::Failed, QStringLiteral("选区读取程序返回的数据过大。")});
            return;
        }
        m_stdout += m_process->read(stdoutBytes);
        m_process->setReadChannel(QProcess::StandardError);
        const qint64 stderrBytes = m_process->bytesAvailable();
        if (stderrBytes > SelectionProtocol::MaxStderrBytes - m_stderrBytes) {
            reject({PlatformErrorCode::Failed, QStringLiteral("选区读取程序返回的错误输出过大。")});
            return;
        }
        m_stderrBytes += stderrBytes;
        // Provider diagnostics are neither displayed nor logged: they can
        // contain private application text. Keep only the bounded byte count.
        m_process->read(stderrBytes);
    }

    void reject(const PlatformError &error)
    {
        if (finished()) return;
        dispose();
        fail(error);
    }

    void dispose()
    {
        m_deadline.stop();
        m_contextWatch.stop();
        if (m_process) {
            auto *process = m_process.data();
            m_process.clear();
            process->disconnect(this);
            // Cover cancellation during startup without ever killing by PID.
            connect(process, &QProcess::started, process, [process] { process->kill(); });
            if (process->state() == QProcess::NotRunning) process->deleteLater();
            else process->kill();
        }
        if (m_job) {
            CloseHandle(m_job);
            m_job = nullptr;
        }
        if (m_attributes) {
            if (m_attributesInitialized) DeleteProcThreadAttributeList(m_attributes);
            HeapFree(GetProcessHeap(), 0, m_attributes);
            m_attributes = nullptr;
            m_attributesInitialized = false;
        }
        if (m_sourceProcess) {
            CloseHandle(m_sourceProcess);
            m_sourceProcess = nullptr;
        }
    }

    SourceContextPtr m_source;
    std::shared_ptr<const WindowsSourceContext> m_context;
    SelectionProtocol::Request m_request;
    QPointer<QProcess> m_process;
    QTimer m_deadline;
    QTimer m_contextWatch;
    QByteArray m_stdout;
    qsizetype m_stderrBytes = 0;
    HANDLE m_sourceProcess = nullptr;
    HANDLE m_job = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST m_attributes = nullptr;
    bool m_attributesInitialized = false;
    STARTUPINFOEXW m_startup{};
    std::wstring m_nativeProgram;
};

class WindowsSelectionReader final : public SelectionReader {
public:
    using SelectionReader::SelectionReader;

    SelectionJob *read(SourceContextPtr source, QObject *owner) override
    {
        return new WindowsSelectionJob(std::move(source), owner);
    }

    CapabilityState availability() const override
    {
        return QFileInfo(helperPath()).isFile() ? CapabilityState::Available : CapabilityState::Unavailable;
    }

    QString unavailableReason() const override
    {
        return availability() == CapabilityState::Available ? QString()
            : QStringLiteral("缺少 trans_selection_helper.exe，请重新安装完整的 Windows 程序包。");
    }
};

} // namespace

SelectionReader *createSelectionReader(QObject *owner) { return new WindowsSelectionReader(owner); }

} // namespace Trans
