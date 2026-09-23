#include "windows_support.h"
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>

namespace {
using namespace Trans::Native::Windows;
namespace Protocol = SelectionProtocol;
using Microsoft::WRL::ComPtr;
using Protocol::Request;
using Protocol::Status;

class Apartment {
  public:
    Apartment() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Apartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
    HRESULT result;
};
struct TextBuffer {
    BSTR value = nullptr;
    ~TextBuffer() { SysFreeString(value); }
};
// This independent ceiling also covers a stalled UIA provider when the parent
// has stopped scheduling. The parent normally closes the job after 3.5 seconds.
class Deadline {
  public:
    Deadline()
        : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          thread_(done_ ? CreateThread(nullptr, 0, expire, done_.get(), 0, nullptr) : nullptr) {}
    ~Deadline() {
        if (thread_) {
            SetEvent(done_.get());
            WaitForSingleObject(thread_.get(), INFINITE);
        }
    }
    explicit operator bool() const { return bool(thread_); }

  private:
    static DWORD WINAPI expire(void *done) {
        if (WaitForSingleObject(static_cast<HANDLE>(done), 8000) != WAIT_OBJECT_0)
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        return 0;
    }
    Handle done_, thread_;
};
Status errorStatus(HRESULT error) {
    if (error == E_ACCESSDENIED || error == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
        return Status::PermissionDenied;
    if (error == UIA_E_NOTSUPPORTED || error == E_NOINTERFACE || error == E_NOTIMPL)
        return Status::Unsupported;
    if (error == UIA_E_ELEMENTNOTAVAILABLE)
        return Status::Cancelled;
    return Status::Failed;
}
bool sourceMatches(const Request &request, HWND focus = nullptr) {
    HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.window));
    if (!IsWindow(window) || GetForegroundWindow() != window || request.process == request.parent ||
        request.process == GetCurrentProcessId())
        return false;
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != request.thread || process != request.process)
        return false;
    GUITHREADINFO information{sizeof(information)};
    if (!GetGUIThreadInfo(request.thread, &information) || !information.hwndFocus ||
        (information.hwndFocus != window && !IsChild(window, information.hwndFocus)))
        return false;
    DWORD focusProcess = 0;
    if (!GetWindowThreadProcessId(information.hwndFocus, &focusProcess) || focusProcess == request.parent ||
        focusProcess == GetCurrentProcessId())
        return false;
    return !focus || information.hwndFocus == focus;
}
bool nativePassword(HWND window) {
    wchar_t className[32]{};
    return GetClassNameW(window, className, 32) && _wcsicmp(className, L"Edit") == 0 &&
           (GetWindowLongPtrW(window, GWL_STYLE) & ES_PASSWORD);
}
Status readSelection(const Request &request, std::wstring &text) {
    if (!sourceMatches(request))
        return Status::Cancelled;
    Handle source(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, request.process));
    if (!source)
        return GetLastError() == ERROR_ACCESS_DENIED ? Status::PermissionDenied : Status::Failed;
    DWORD sourceLevel = 0, helperLevel = 0;
    if (!integrityLevel(source.get(), sourceLevel) || !integrityLevel(GetCurrentProcess(), helperLevel))
        return GetLastError() == ERROR_ACCESS_DENIED ? Status::PermissionDenied : Status::Failed;
    if (sourceLevel > helperLevel)
        return Status::PermissionDenied;
    GUITHREADINFO information{sizeof(information)};
    if (!GetGUIThreadInfo(request.thread, &information))
        return Status::Cancelled;
    const HWND focusWindow = information.hwndFocus;
    const HWND sourceWindow = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.window));
    if (nativePassword(focusWindow))
        return Status::PermissionDenied;
    Apartment apartment;
    if (FAILED(apartment.result))
        return errorStatus(apartment.result);
    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(automation.GetAddressOf()));
    if (FAILED(result))
        return errorStatus(result);
    ComPtr<IUIAutomationElement> focus;
    result = automation->GetFocusedElement(focus.GetAddressOf());
    if (FAILED(result))
        return errorStatus(result);
    if (!focus)
        return Status::Unsupported;
    ComPtr<IUIAutomationTreeWalker> walker;
    result = automation->get_RawViewWalker(walker.GetAddressOf());
    if (FAILED(result))
        return errorStatus(result);
    if (!walker)
        return Status::Failed;

    // Validate all ancestors before reading text. HWND-less document children
    // are accepted only if their ancestry reaches the captured native window.
    std::vector<ComPtr<IUIAutomationElement>> ancestry;
    ancestry.reserve(16);
    ComPtr<IUIAutomationElement> element = focus;
    bool reachedSource = false;
    std::vector<DWORD> verifiedProcesses{request.process};
    for (int depth = 0; element && depth < 64; ++depth) {
        if (!sourceMatches(request, focusWindow))
            return Status::Cancelled;
        int processId = 0;
        result = element->get_CurrentProcessId(&processId);
        if (FAILED(result))
            return errorStatus(result);
        if (processId <= 0)
            return Status::Unsupported;
        if (DWORD(processId) == request.parent || DWORD(processId) == GetCurrentProcessId())
            return Status::PermissionDenied;
        if (std::find(verifiedProcesses.begin(), verifiedProcesses.end(), DWORD(processId)) ==
            verifiedProcesses.end()) {
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(processId)));
            DWORD level = 0;
            if (!process || !integrityLevel(process.get(), level) || level > helperLevel)
                return Status::PermissionDenied;
            verifiedProcesses.push_back(DWORD(processId));
        }
        BOOL password = TRUE;
        result = element->get_CurrentIsPassword(&password);
        if (FAILED(result))
            return errorStatus(result);
        if (password)
            return Status::PermissionDenied;
        UIA_HWND native{};
        result = element->get_CurrentNativeWindowHandle(&native);
        if (FAILED(result))
            return errorStatus(result);
        HWND nativeWindow = reinterpret_cast<HWND>(native);
        if (nativeWindow) {
            DWORD nativeProcess = 0;
            if (!IsWindow(nativeWindow) || !GetWindowThreadProcessId(nativeWindow, &nativeProcess) ||
                nativeProcess != DWORD(processId) ||
                (nativeWindow != sourceWindow && !IsChild(sourceWindow, nativeWindow)))
                return Status::Unsupported;
            if (nativeProcess == request.parent || nativeProcess == GetCurrentProcessId() ||
                nativePassword(nativeWindow))
                return Status::PermissionDenied;
        }
        ancestry.push_back(element);
        if (nativeWindow == sourceWindow) {
            reachedSource = true;
            break;
        }
        ComPtr<IUIAutomationElement> parent;
        result = walker->GetParentElement(element.Get(), parent.GetAddressOf());
        if (FAILED(result))
            return errorStatus(result);
        element = std::move(parent);
    }
    if (!reachedSource)
        return Status::Unsupported;
    ComPtr<IUIAutomationTextPattern> pattern;
    for (const auto &candidate : ancestry) {
        if (!sourceMatches(request, focusWindow))
            return Status::Cancelled;
        result = candidate->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(pattern.GetAddressOf()));
        if (SUCCEEDED(result) && pattern)
            break;
        if (FAILED(result) && errorStatus(result) != Status::Unsupported)
            return errorStatus(result);
        pattern.Reset();
    }
    if (!pattern)
        return Status::Unsupported;
    SupportedTextSelection supported = SupportedTextSelection_None;
    result = pattern->get_SupportedTextSelection(&supported);
    if (FAILED(result))
        return errorStatus(result);
    if (supported == SupportedTextSelection_None)
        return Status::Unsupported;
    ComPtr<IUIAutomationTextRangeArray> ranges;
    result = pattern->GetSelection(ranges.GetAddressOf());
    if (FAILED(result))
        return errorStatus(result);
    if (!ranges)
        return Status::NoSelection;
    int count = 0;
    result = ranges->get_Length(&count);
    if (FAILED(result))
        return errorStatus(result);
    if (count < 0 || count > Protocol::MaxRanges)
        return Status::Failed;
    for (int index = 0; index < count; ++index) {
        if (!sourceMatches(request, focusWindow))
            return Status::Cancelled;
        ComPtr<IUIAutomationTextRange> range;
        result = ranges->GetElement(index, range.GetAddressOf());
        if (FAILED(result))
            return errorStatus(result);
        if (!range)
            return Status::Failed;
        TextBuffer selected;
        const auto remaining =
            std::ptrdiff_t(Protocol::MaxTextChars) - std::ptrdiff_t(text.size()) - (text.empty() ? 0 : 1);
        if (remaining < 0)
            return Status::Failed;
        result = range->GetText(int(remaining + 1), &selected.value);
        if (FAILED(result))
            return errorStatus(result);
        const auto length = SysStringLen(selected.value);
        if (length > size_t(remaining))
            return Status::Failed;
        if (!length)
            continue;
        // Validate without allocating or copying a temporary UTF-8 fragment.
        const std::wstring_view fragment(selected.value, length);
        if (fragment.find(L'\0') != fragment.npos ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, fragment.data(), int(fragment.size()), nullptr, 0,
                                 nullptr, nullptr))
            return Status::Failed;
        if (!text.empty())
            text += L'\n';
        text.append(fragment);
    }
    ComPtr<IUIAutomationElement> finalFocus;
    result = automation->GetFocusedElement(finalFocus.GetAddressOf());
    if (FAILED(result))
        return errorStatus(result);
    if (!finalFocus)
        return Status::Cancelled;
    BOOL same = FALSE;
    result = automation->CompareElements(focus.Get(), finalFocus.Get(), &same);
    if (FAILED(result))
        return errorStatus(result);
    BOOL password = TRUE;
    result = finalFocus->get_CurrentIsPassword(&password);
    if (FAILED(result))
        return errorStatus(result);
    if (password || nativePassword(focusWindow))
        return Status::PermissionDenied;
    if (!same || !sourceMatches(request, focusWindow) || WaitForSingleObject(source.get(), 0) != WAIT_TIMEOUT)
        return Status::Cancelled;
    return validText(text) ? Status::Success : Status::NoSelection;
}
bool transferSync(HANDLE pipe, void *data, DWORD length, bool writing) {
    DWORD offset = 0;
    while (offset < length) {
        DWORD count = 0;
        BOOL success = writing ? WriteFile(pipe, static_cast<BYTE *>(data) + offset, length - offset, &count, nullptr)
                               : ReadFile(pipe, static_cast<BYTE *>(data) + offset, length - offset, &count, nullptr);
        if (!success || !count)
            return false;
        offset += count;
    }
    return true;
}
} // namespace

int main(int argc, char **) {
    if (argc != 1)
        return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Deadline deadline;
    if (!deadline)
        return 2;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL contained = FALSE;
    if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(output) != FILE_TYPE_PIPE ||
        !IsProcessInJob(GetCurrentProcess(), nullptr, &contained) || !contained)
        return 2;
    Protocol::RequestBytes bytes{};
    Request request;
    if (!transferSync(input, bytes.data(), DWORD(bytes.size()), false) || !Protocol::decode(bytes, request))
        return 2;
    Identity identity;
    ULONG inputParent = 0, outputParent = 0;
    if (!readIdentity(GetCurrentProcess(), identity) || !verifyPeer(input, false, identity, &inputParent) ||
        !verifyPeer(output, false, identity, &outputParent) || inputParent != request.parent ||
        outputParent != request.parent)
        return 2;
    try {
        std::wstring text;
        Status status = readSelection(request, text);
        auto response = Protocol::response(request, status, text);
        return transferSync(output, response.data(), DWORD(response.size()), true) ? 0 : 2;
    } catch (...) {
        return 2;
    }
}
