#include "selection_protocol_windows.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <QCoreApplication>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using Trans::SelectionProtocol::Request;
using Trans::SelectionProtocol::Status;
namespace Protocol = Trans::SelectionProtocol;

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) : m_value(value) {}
    ~Handle() { if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const { return m_value; }
    explicit operator bool() const { return m_value && m_value != INVALID_HANDLE_VALUE; }
private:
    HANDLE m_value;
};

class ComApartment final {
public:
    ComApartment() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    const HRESULT result;
};

class TextBuffer final {
public:
    ~TextBuffer() { SysFreeString(value); }
    BSTR value = nullptr;
};

// The parent normally kills this job at 3.5 seconds. This independent ceiling
// also bounds a manually launched helper or a parent whose event loop stalls.
class Deadline final {
public:
    Deadline() : m_done(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
                 m_thread(m_done ? CreateThread(nullptr, 0, expire, m_done.get(), 0, nullptr) : nullptr) {}
    ~Deadline()
    {
        if (m_thread) {
            SetEvent(m_done.get());
            WaitForSingleObject(m_thread.get(), INFINITE);
        }
    }
    bool valid() const { return static_cast<bool>(m_thread); }
private:
    static DWORD WINAPI expire(void *done)
    {
        if (WaitForSingleObject(static_cast<HANDLE>(done), 8000) != WAIT_OBJECT_0)
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        return 0;
    }
    Handle m_done;
    Handle m_thread;
};

Status errorStatus(HRESULT error)
{
    if (error == E_ACCESSDENIED || error == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
        return Status::PermissionDenied;
    if (error == UIA_E_NOTSUPPORTED || error == E_NOINTERFACE || error == E_NOTIMPL)
        return Status::Unsupported;
    if (error == UIA_E_ELEMENTNOTAVAILABLE) return Status::Cancelled;
    return Status::Failed;
}

DWORD pipeOperation(HANDLE pipe, void *data, DWORD bytes, DWORD &transferred, bool writing)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return GetLastError();
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    const BOOL ready = writing ? WriteFile(pipe, data, bytes, &transferred, &operation)
                               : ReadFile(pipe, data, bytes, &transferred, &operation);
    if (ready) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) return error;
    return GetOverlappedResult(pipe, &operation, &transferred, TRUE) ? ERROR_SUCCESS : GetLastError();
}

bool readRequest(HANDLE input, QByteArray &bytes)
{
    char buffer[512];
    for (;;) {
        DWORD count = 0;
        const DWORD error = pipeOperation(input, buffer, sizeof(buffer), count, false);
        if (error != ERROR_SUCCESS) return error == ERROR_BROKEN_PIPE && !bytes.isEmpty();
        if (!count) return !bytes.isEmpty();
        if (bytes.size() + count > Protocol::MaxRequestBytes) return false;
        bytes.append(buffer, static_cast<qsizetype>(count));
    }
}

bool writeResponse(HANDLE output, const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > Protocol::MaxResponseBytes) return false;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (pipeOperation(output, const_cast<char *>(bytes.constData() + offset),
                          static_cast<DWORD>(bytes.size() - offset), count, true) != ERROR_SUCCESS || !count)
            return false;
        offset += count;
    }
    return true;
}

bool sourceMatches(const Request &request, HWND focus = nullptr)
{
    const HWND window = reinterpret_cast<HWND>(static_cast<quintptr>(request.window));
    if (!IsWindow(window) || GetForegroundWindow() != window
        || request.processId == request.parentProcessId || request.processId == GetCurrentProcessId()) return false;
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != request.threadId || process != request.processId) return false;
    GUITHREADINFO information{sizeof(information)};
    if (!GetGUIThreadInfo(request.threadId, &information) || !information.hwndFocus
        || (information.hwndFocus != window && !IsChild(window, information.hwndFocus))) return false;
    DWORD focusProcess = 0;
    if (!GetWindowThreadProcessId(information.hwndFocus, &focusProcess)
        || focusProcess == request.parentProcessId || focusProcess == GetCurrentProcessId())
        return false;
    return !focus || focus == information.hwndFocus;
}

bool integrityLevel(HANDLE process, DWORD &level)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) return false;
    Handle token(rawToken);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &bytes);
    if (!bytes || bytes > 4096) return false;
    std::vector<BYTE> storage(bytes);
    if (!GetTokenInformation(token.get(), TokenIntegrityLevel, storage.data(), bytes, &bytes)) return false;
    const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(storage.data());
    if (!IsValidSid(label->Label.Sid)) return false;
    const BYTE count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return false;
    level = *GetSidSubAuthority(label->Label.Sid, count - 1);
    return true;
}

Status readSelection(const Request &request, QString &text)
{
    if (!sourceMatches(request)) return Status::Cancelled;
    Handle source(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, request.processId));
    if (!source) return GetLastError() == ERROR_ACCESS_DENIED ? Status::PermissionDenied : Status::Failed;
    DWORD sourceLevel = 0;
    DWORD helperLevel = 0;
    if (!integrityLevel(source.get(), sourceLevel) || !integrityLevel(GetCurrentProcess(), helperLevel))
        return GetLastError() == ERROR_ACCESS_DENIED ? Status::PermissionDenied : Status::Failed;
    if (sourceLevel > helperLevel) return Status::PermissionDenied;

    GUITHREADINFO information{sizeof(information)};
    if (!GetGUIThreadInfo(request.threadId, &information)) return Status::Cancelled;
    const HWND focusWindow = information.hwndFocus;
    const HWND sourceWindow = reinterpret_cast<HWND>(static_cast<quintptr>(request.window));
    ComApartment apartment;
    if (FAILED(apartment.result)) return errorStatus(apartment.result);
    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(automation.GetAddressOf()));
    if (FAILED(result)) return errorStatus(result);
    ComPtr<IUIAutomationElement> focus;
    result = automation->GetFocusedElement(focus.GetAddressOf());
    if (FAILED(result)) return errorStatus(result);
    if (!focus) return Status::Unsupported;
    ComPtr<IUIAutomationTreeWalker> walker;
    result = automation->get_RawViewWalker(walker.GetAddressOf());
    if (FAILED(result)) return errorStatus(result);
    if (!walker) return Status::Failed;

    // Validate the entire focused-element ancestry before reading any text.
    // Non-HWND document descendants are accepted only when their UIA ancestry
    // reaches the captured source HWND. Embedded document processes need not
    // share the top-level window's PID; real HWNDs must agree with their UIA
    // process identity and belong to that window's native child hierarchy.
    std::vector<ComPtr<IUIAutomationElement>> ancestry;
    ancestry.reserve(16);
    ComPtr<IUIAutomationElement> element = focus;
    bool reachedSource = false;
    for (int depth = 0; element && depth < 64; ++depth) {
        if (!sourceMatches(request, focusWindow)) return Status::Cancelled;
        int processId = 0;
        result = element->get_CurrentProcessId(&processId);
        if (FAILED(result)) return errorStatus(result);
        if (static_cast<quint32>(processId) == request.parentProcessId
            || static_cast<quint32>(processId) == GetCurrentProcessId()) return Status::PermissionDenied;
        BOOL password = TRUE;
        result = element->get_CurrentIsPassword(&password);
        if (FAILED(result)) return errorStatus(result);
        if (password) return Status::PermissionDenied;
        UIA_HWND native{};
        result = element->get_CurrentNativeWindowHandle(&native);
        if (FAILED(result)) return errorStatus(result);
        const HWND nativeWindow = reinterpret_cast<HWND>(native);
        if (nativeWindow) {
            DWORD nativeProcess = 0;
            if (!IsWindow(nativeWindow) || !GetWindowThreadProcessId(nativeWindow, &nativeProcess)
                || (processId > 0 && nativeProcess != static_cast<quint32>(processId))
                || (nativeWindow != sourceWindow && !IsChild(sourceWindow, nativeWindow)))
                return Status::Unsupported;
            if (nativeProcess == request.parentProcessId || nativeProcess == GetCurrentProcessId())
                return Status::PermissionDenied;
            // A native password edit must not depend on a provider correctly
            // reporting IsPassword before refusing to disclose its selection.
            wchar_t className[32]{};
            if (GetClassNameW(nativeWindow, className, 32) && _wcsicmp(className, L"Edit") == 0
                && (GetWindowLongPtrW(nativeWindow, GWL_STYLE) & ES_PASSWORD)) return Status::PermissionDenied;
        }
        ancestry.push_back(element);
        if (nativeWindow == sourceWindow) {
            reachedSource = true;
            break;
        }
        ComPtr<IUIAutomationElement> parent;
        result = walker->GetParentElement(element.Get(), parent.GetAddressOf());
        if (FAILED(result)) return errorStatus(result);
        element = std::move(parent);
    }
    if (!reachedSource) return Status::Unsupported;

    ComPtr<IUIAutomationTextPattern> pattern;
    for (const auto &candidate : ancestry) {
        if (!sourceMatches(request, focusWindow)) return Status::Cancelled;
        result = candidate->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(pattern.GetAddressOf()));
        if (SUCCEEDED(result) && pattern) break;
        if (FAILED(result) && errorStatus(result) != Status::Unsupported) return errorStatus(result);
        pattern.Reset();
    }
    if (!pattern) return Status::Unsupported;
    SupportedTextSelection supported = SupportedTextSelection_None;
    result = pattern->get_SupportedTextSelection(&supported);
    if (FAILED(result)) return errorStatus(result);
    if (supported == SupportedTextSelection_None) return Status::Unsupported;
    ComPtr<IUIAutomationTextRangeArray> ranges;
    result = pattern->GetSelection(ranges.GetAddressOf());
    if (FAILED(result)) return errorStatus(result);
    if (!ranges) return Status::NoSelection;
    int count = 0;
    result = ranges->get_Length(&count);
    if (FAILED(result)) return errorStatus(result);
    if (count < 0 || count > Protocol::MaxRanges) return Status::Failed;
    for (int index = 0; index < count; ++index) {
        if (!sourceMatches(request, focusWindow)) return Status::Cancelled;
        ComPtr<IUIAutomationTextRange> range;
        result = ranges->GetElement(index, range.GetAddressOf());
        if (FAILED(result)) return errorStatus(result);
        if (!range) return Status::Failed;
        TextBuffer selected;
        const qsizetype remaining = Protocol::MaxTextChars - text.size() - (text.isEmpty() ? 0 : 1);
        if (remaining < 0) return Status::Failed;
        result = range->GetText(static_cast<int>(remaining + 1), &selected.value);
        if (FAILED(result)) return errorStatus(result);
        const auto length = static_cast<qsizetype>(SysStringLen(selected.value));
        if (length > remaining) return Status::Failed;
        if (!length) continue; // A caret produces a degenerate, empty range.
        const QString fragment = QString::fromWCharArray(selected.value, length);
        if (!fragment.isValidUtf16() || fragment.contains(QChar::Null)) return Status::Failed;
        if (!text.isEmpty()) text += u'\n';
        text += fragment;
    }

    // Re-query UIA focus as well as HWND focus: virtual controls can change
    // focus without changing the host's native HWND.
    ComPtr<IUIAutomationElement> finalFocus;
    result = automation->GetFocusedElement(finalFocus.GetAddressOf());
    if (FAILED(result)) return errorStatus(result);
    if (!finalFocus) return Status::Cancelled;
    BOOL same = FALSE;
    result = automation->CompareElements(focus.Get(), finalFocus.Get(), &same);
    if (FAILED(result)) return errorStatus(result);
    BOOL password = TRUE;
    result = finalFocus->get_CurrentIsPassword(&password);
    if (FAILED(result)) return errorStatus(result);
    if (password) return Status::PermissionDenied;
    if (!same || !sourceMatches(request, focusWindow)
        || WaitForSingleObject(source.get(), 0) != WAIT_TIMEOUT) return Status::Cancelled;
    return text.trimmed().isEmpty() ? Status::NoSelection : Status::Success;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 1) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Deadline deadline;
    if (!deadline.valid()) return 2;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL contained = FALSE;
    if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(output) != FILE_TYPE_PIPE
        || !IsProcessInJob(GetCurrentProcess(), nullptr, &contained) || !contained) return 2;
    QByteArray bytes;
    Request request;
    if (!readRequest(input, bytes) || !Protocol::parseRequest(bytes, request)) return 2;
    ULONG inputParent = 0;
    ULONG outputParent = 0;
    if (!GetNamedPipeServerProcessId(input, &inputParent) || !GetNamedPipeServerProcessId(output, &outputParent)
        || inputParent != request.parentProcessId || outputParent != request.parentProcessId) return 2;
    QString text;
    const Status status = readSelection(request, text);
    return writeResponse(output, Protocol::encodeResponse(request.id, status, text)) ? 0 : 2;
}
