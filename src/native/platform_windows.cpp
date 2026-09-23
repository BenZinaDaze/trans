#include "platform.h"
#include "windows_support.h"
#include <array>
#include <atomic>
#include <commctrl.h>
#include <cstring>
#include <dwmapi.h>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdexcept>
#include <thread>

namespace Trans::Native {
namespace {
using namespace Windows;
namespace Protocol = Windows::SelectionProtocol;

Error nativeError(const char *operation, DWORD error = GetLastError()) {
    return {std::string(operation) + " (Windows error " + std::to_string(error) + ")."};
}
Error selectionError(Protocol::Status status) {
    switch (status) {
    case Protocol::Status::NoSelection:
        return {"请先在其他应用中选中文本，再按翻译快捷键。"};
    case Protocol::Status::Unsupported:
        return {"此控件不支持 Windows UI Automation 选区读取，请使用截图翻译。"};
    case Protocol::Status::PermissionDenied:
        return {"Windows 拒绝读取密码控件、受保护窗口或更高权限应用的选区。"};
    case Protocol::Status::Cancelled:
        return {"来源窗口或焦点已改变，选区读取已取消。"};
    default:
        return {"Windows UI Automation 选区读取失败，请重试或使用截图翻译。"};
    }
}
std::wstring helperPath() {
    std::wstring path(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!count || count >= path.size())
        return {};
    path.resize(count);
    auto slash = path.find_last_of(L"\\/");
    if (slash == path.npos)
        return {};
    path.resize(slash + 1);
    return path + L"trans_selection_helper.exe";
}
struct WindowsSource final : Source {
    HWND window = nullptr;
    DWORD process = 0, thread = 0;
    Handle lifetime;
    bool matches(bool foreground) const {
        DWORD currentProcess = 0;
        return window && IsWindow(window) && process != GetCurrentProcessId() &&
               GetWindowThreadProcessId(window, &currentProcess) == thread && currentProcess == process &&
               (!lifetime || WaitForSingleObject(lifetime.get(), 0) == WAIT_TIMEOUT) &&
               (!foreground || GetForegroundWindow() == window);
    }
};
class DpiScope {
  public:
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~DpiScope() {
        if (previous_)
            SetThreadDpiAwarenessContext(previous_);
    }

  private:
    DPI_AWARENESS_CONTEXT previous_;
};
struct Hotkey {
    UINT modifiers = 0, key = 0;
    bool operator==(const Hotkey &) const = default;
};
Error parseHotkey(const std::string &text, Hotkey &binding) {
    binding = {};
    if (text.empty())
        return {};
    std::wstring wide;
    if (!toWide(text, wide))
        return {"快捷键包含无效 Unicode。"};
    size_t offset = 0;
    for (;;) {
        size_t plus = wide.find(L'+', offset);
        if (plus == wide.npos || plus == offset)
            break;
        std::wstring token = wide.substr(offset, plus - offset);
        for (auto &c : token)
            if (c >= L'A' && c <= L'Z')
                c += L'a' - L'A';
        UINT modifier = token == L"ctrl" || token == L"control"                    ? MOD_CONTROL
                        : token == L"alt"                                          ? MOD_ALT
                        : token == L"shift"                                        ? MOD_SHIFT
                        : token == L"meta" || token == L"win" || token == L"super" ? MOD_WIN
                                                                                   : 0;
        if (!modifier)
            break;
        if (binding.modifiers & modifier)
            return {"快捷键包含重复修饰键。"};
        binding.modifiers |= modifier;
        offset = plus + 1;
    }
    if (binding.modifiers & MOD_WIN)
        return {"含 Windows / Meta 键的快捷键由系统保留，请改用 Ctrl 或 Alt。"};
    if (!(binding.modifiers & (MOD_CONTROL | MOD_ALT)))
        return {"快捷键应包含 Ctrl 或 Alt，以及一个普通按键。"};
    std::wstring key = wide.substr(offset);
    for (auto &c : key)
        if (c >= L'a' && c <= L'z')
            c -= L'a' - L'A';
    if (key.size() == 1 && key[0] >= L'A' && key[0] <= L'Z')
        binding.key = UINT(key[0]);
    else if (key.size() >= 2 && key.size() <= 3 && key[0] == L'F' && key[1] >= L'1' && key[1] <= L'9') {
        unsigned number = unsigned(key[1] - L'0');
        if (key.size() == 3) {
            if (key[2] < L'0' || key[2] > L'9')
                return {"无法识别此快捷键。"};
            number = number * 10 + unsigned(key[2] - L'0');
        }
        if (number > 24)
            return {"Windows 全局快捷键仅支持 F1–F24。"};
        binding.key = VK_F1 + number - 1;
    } else {
        static constexpr std::pair<std::wstring_view, UINT> names[]{
            {L"ESC", VK_ESCAPE},     {L"ESCAPE", VK_ESCAPE},     {L"TAB", VK_TAB},         {L"BACKTAB", VK_TAB},
            {L"BACKSPACE", VK_BACK}, {L"RETURN", VK_RETURN},     {L"ENTER", VK_RETURN},    {L"INS", VK_INSERT},
            {L"INSERT", VK_INSERT},  {L"DEL", VK_DELETE},        {L"DELETE", VK_DELETE},   {L"PAUSE", VK_PAUSE},
            {L"PRINT", VK_SNAPSHOT}, {L"HOME", VK_HOME},         {L"END", VK_END},         {L"LEFT", VK_LEFT},
            {L"UP", VK_UP},          {L"RIGHT", VK_RIGHT},       {L"DOWN", VK_DOWN},       {L"PGUP", VK_PRIOR},
            {L"PAGEUP", VK_PRIOR},   {L"PGDOWN", VK_NEXT},       {L"PAGEDOWN", VK_NEXT},   {L"SPACE", VK_SPACE},
            {L"MENU", VK_APPS},      {L"SCROLLLOCK", VK_SCROLL}, {L"NUMLOCK", VK_NUMLOCK}, {L"CAPSLOCK", VK_CAPITAL}};
        for (auto [name, value] : names)
            if (key == name) {
                binding.key = value;
                break;
            }
        if (key == L"BACKTAB")
            binding.modifiers |= MOD_SHIFT;
        if (!binding.key && key.size() == 1 && key[0] >= 0x21 && (key[0] < 0xd800 || key[0] > 0xdfff)) {
            SHORT translated = VkKeyScanExW(key[0], GetKeyboardLayout(0));
            if (translated != -1 && !(HIBYTE(translated) & ~1)) {
                binding.key = LOBYTE(translated);
                if (HIBYTE(translated) & 1)
                    binding.modifiers |= MOD_SHIFT;
            }
        }
    }
    if (!binding.key)
        return {"此按键不能可靠映射为 Windows 全局快捷键，请使用字母、功能键或普通导航键。"};
    const bool reserved =
        binding.key == VK_F12 || binding.key == VK_SNAPSHOT ||
        (binding.key == VK_TAB && (binding.modifiers & MOD_ALT)) ||
        (binding.key == VK_ESCAPE && (binding.modifiers & (MOD_CONTROL | MOD_ALT))) ||
        ((binding.key == VK_F4 || binding.key == VK_SPACE) && binding.modifiers == MOD_ALT) ||
        (binding.key == VK_DELETE && (binding.modifiers & (MOD_CONTROL | MOD_ALT)) == (MOD_CONTROL | MOD_ALT));
    return reserved ? Error{"此快捷键由 Windows 或调试器保留，请录制另一个组合。"} : Error{};
}
Error hotkeyError(DWORD code) {
    return code == ERROR_HOTKEY_ALREADY_REGISTERED ? Error{"此快捷键已被 Windows 或其他应用占用。"}
                                                   : nativeError("无法更新 Windows 全局快捷键", code);
}

// Same bounded instance frame as the previous native Windows channel: network
// byte order, magic, version, type, four-byte payload. Never allocate from input.
using Frame = std::array<std::uint8_t, 12>;
Frame frame(std::uint8_t type, std::uint32_t value) {
    return {0x54,
            0x52,
            0x4e,
            0x53,
            1,
            type,
            0,
            4,
            std::uint8_t(value >> 24),
            std::uint8_t(value >> 16),
            std::uint8_t(value >> 8),
            std::uint8_t(value)};
}
bool decodeFrame(const Frame &bytes, std::uint8_t type, std::uint32_t &value) {
    const auto expected = frame(type, 0);
    if (!std::equal(bytes.begin(), bytes.begin() + 8, expected.begin()))
        return false;
    value = std::uint32_t(bytes[8]) << 24 | std::uint32_t(bytes[9]) << 16 | std::uint32_t(bytes[10]) << 8 | bytes[11];
    return true;
}
DWORD receiveFrame(HANDLE pipe, Frame &bytes, HANDLE stop, ULONGLONG deadline) {
    DWORD result = transfer(pipe, bytes.data(), DWORD(bytes.size()), false, stop, deadline);
    if (result != ERROR_SUCCESS)
        return result;
    DWORD extra = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &extra, nullptr))
        return GetLastError();
    return extra ? ERROR_INVALID_DATA : ERROR_SUCCESS;
}
std::wstring endpointName(const Identity &identity) {
    std::wstring material = sidString(identity.user) + L"/" + sidString(identity.logon) + L"/" +
                            std::to_wstring(identity.session) + L"/" +
                            std::to_wstring(DWORD(identity.authentication.HighPart)) + L"/" +
                            std::to_wstring(identity.authentication.LowPart);
    std::string utf8;
    if (!toUtf8(material, utf8))
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    std::array<BYTE, 32> digest{};
    NTSTATUS result = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(utf8.data()), ULONG(utf8.size()),
                                 digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0)
        return {};
    std::wstring name = L"io.github.trans.Trans.";
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto byte : digest) {
        name += digits[byte >> 4];
        name += digits[byte & 15];
    }
    return name;
}

struct HelperPipe {
    Handle server, child;
    bool create(bool parentWrites, Security &security) {
        const auto nonce = randomName();
        if (nonce.empty())
            return false;
        const auto name = L"\\\\.\\pipe\\trans-selection-" + nonce;
        server.reset(CreateNamedPipeW(name.c_str(),
                                      (parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
                                          FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                      8192, 8192, 0, security.attributes()));
        if (!server)
            return false;
        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        child.reset(CreateFileW(name.c_str(), parentWrites ? GENERIC_READ : GENERIC_WRITE, 0, &inherited, OPEN_EXISTING,
                                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!child)
            return false;
        // A client opened before ConnectNamedPipe produces ERROR_PIPE_CONNECTED.
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event)
            return false;
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        if (ConnectNamedPipe(server.get(), &operation))
            return true;
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
            return true;
        if (error == ERROR_IO_PENDING) {
            CancelIoEx(server.get(), &operation);
            DWORD count = 0;
            GetOverlappedResult(server.get(), &operation, &count, TRUE);
        }
        SetLastError(error);
        return false;
    }
};
class ProcessAttributes {
  public:
    ~ProcessAttributes() {
        if (initialized_)
            DeleteProcThreadAttributeList(list());
    }
    bool initialize(HANDLE job, std::span<HANDLE> inherited) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
        if (!bytes)
            return false;
        storage_.resize(bytes);
        if (!InitializeProcThreadAttributeList(list(), 2, 0, &bytes))
            return false;
        initialized_ = true;
        job_ = job;
        return UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_, sizeof(job_), nullptr,
                                         nullptr) &&
               UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                         inherited.size_bytes(), nullptr, nullptr);
    }
    LPPROC_THREAD_ATTRIBUTE_LIST list() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data()); }

  private:
    std::vector<BYTE> storage_;
    HANDLE job_ = nullptr;
    bool initialized_ = false;
};
void removeTaskbarTab(HWND window) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) {
        ITaskbarList *taskbar = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)))) {
            if (SUCCEEDED(taskbar->HrInit()))
                taskbar->DeleteTab(window);
            taskbar->Release();
        }
    }
    if (SUCCEEDED(initialized))
        CoUninitialize();
}
LRESULT CALLBACK popupProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (taskbarCreated && message == taskbarCreated)
        removeTaskbarTab(window);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, popupProcedure, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

class WindowsPlatform final : public Platform {
  public:
    WindowsPlatform();
    ~WindowsPlatform() override;
    Capabilities capabilities() const override;
    InstanceResult startInstance(Command initial, std::function<void(Command)> callback) override;
    void setReady() override {
        if (ready_)
            SetEvent(ready_.get());
    }
    Error setShortcuts(const std::string &selection, const std::string &screenshot, bool replaceExisting) override;
    SourcePtr captureSource() override;
    Selection readSelection(const SourcePtr &, std::stop_token) override;
    Capture captureScreens(std::stop_token) override;
    void restoreSource(const SourcePtr &) override;
    void configureWindow(std::uintptr_t handle, bool popup, bool topmost, std::uintptr_t owner) override;
    void activateWindow(std::uintptr_t handle) override;
    Rect availableGeometry(bool atCursor) override;
    Rect cursorGeometry() override;
    Error copyText(const std::string &) override;
    std::string configDirectory() const override;
    Error writePrivateFile(const std::string &path, const std::string &contents) override;

  private:
    static constexpr UINT InvokeMessage = WM_APP + 1;
    static LRESULT CALLBACK messageProcedure(HWND, UINT, WPARAM, LPARAM);
    void messageMain(std::promise<Error> initialized);
    void invoke(std::function<void()> function);
    Error applyHotkeys(const std::array<Hotkey, 2> &desired);
    bool dispatch(Command command);
    void instanceMain(Command initial, std::promise<InstanceResult> result);
    void serveInstances(Handle listener, Handle event, const std::wstring &path, Security &security,
                        const Identity &identity);
    void serveClient(Handle pipe, const Identity &identity);
    Handle stop_, ready_;
    std::thread messageThread_, instanceThread_;
    HWND messageWindow_ = nullptr;
    std::array<Hotkey, 2> hotkeys_{};
    std::mutex callbackMutex_;
    struct Invocation {
        std::function<void()> function;
        std::exception_ptr failure;
        std::atomic_bool completed{false};
    };
    std::mutex invocationMutex_;
    std::atomic<Invocation *> pendingInvocation_{nullptr};
    std::function<void(Command)> callback_;
    Error initializationError_;
    bool started_ = false;
};

WindowsPlatform::WindowsPlatform()
    : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!stop_ || !ready_) {
        initializationError_ = nativeError("Cannot initialize native event handling");
        return;
    }
    std::promise<Error> result;
    auto future = result.get_future();
    messageThread_ = std::thread([this, result = std::move(result)]() mutable { messageMain(std::move(result)); });
    initializationError_ = future.get();
}
WindowsPlatform::~WindowsPlatform() {
    if (stop_)
        SetEvent(stop_.get());
    {
        std::lock_guard lock(callbackMutex_);
        callback_ = {};
    }
    if (instanceThread_.joinable())
        instanceThread_.join();
    if (messageThread_.joinable())
        messageThread_.join();
}
void WindowsPlatform::messageMain(std::promise<Error> initialized) {
    const auto className = L"TransNativeEvents-" + std::to_wstring(GetCurrentProcessId()) + L"-" + randomName();
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = messageProcedure;
    windowClass.lpszClassName = className.c_str();
    if (!RegisterClassExW(&windowClass)) {
        initialized.set_value(nativeError("Cannot register native event window"));
        return;
    }
    messageWindow_ =
        CreateWindowExW(0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, this);
    if (!messageWindow_) {
        auto error = nativeError("Cannot create native event window");
        UnregisterClassW(className.c_str(), windowClass.hInstance);
        initialized.set_value(error);
        return;
    }
    initialized.set_value({});
    HANDLE stop = stop_.get();
    bool quit = false;
    while (!quit) {
        DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
            break;
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    for (int i = 0; i < 2; ++i)
        if (hotkeys_[i].key)
            UnregisterHotKey(messageWindow_, i + 1);
    DestroyWindow(messageWindow_);
    UnregisterClassW(className.c_str(), windowClass.hInstance);
}
LRESULT CALLBACK WindowsPlatform::messageProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *self = reinterpret_cast<WindowsPlatform *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<WindowsPlatform *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && message == InvokeMessage) {
        // Window messages are externally sendable. Never dereference an LPARAM
        // pointer supplied by another process; the work item lives in our queue.
        if (auto *work = self->pendingInvocation_.exchange(nullptr)) {
            try {
                work->function();
            } catch (...) {
                work->failure = std::current_exception();
            }
            work->completed.store(true);
        }
        return 0;
    }
    if (self && message == WM_HOTKEY && wparam >= 1 && wparam <= 2) {
        const auto &binding = self->hotkeys_[size_t(wparam - 1)];
        if (binding.key && HIWORD(lparam) == binding.key && (LOWORD(lparam) & ~MOD_NOREPEAT) == binding.modifiers &&
            WaitForSingleObject(self->ready_.get(), 0) == WAIT_OBJECT_0)
            self->dispatch(wparam == 1 ? Command::TranslateSelection : Command::TranslateScreenshot);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void WindowsPlatform::invoke(std::function<void()> function) {
    if (GetCurrentThreadId() == GetWindowThreadProcessId(messageWindow_, nullptr)) {
        function();
        return;
    }
    Invocation work{std::move(function), {}};
    std::lock_guard lock(invocationMutex_);
    pendingInvocation_.store(&work);
    SendMessageW(messageWindow_, InvokeMessage, 0, 0);
    pendingInvocation_.store(nullptr);
    if (!work.completed.load())
        throw std::runtime_error("The native event thread is unavailable.");
    if (work.failure)
        std::rethrow_exception(work.failure);
}
bool WindowsPlatform::dispatch(Command command) {
    if (WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0)
        return false;
    std::function<void(Command)> callback;
    {
        std::lock_guard lock(callbackMutex_);
        callback = callback_;
    }
    if (!callback)
        return false;
    try {
        callback(command);
        return true;
    } catch (...) {
        return false;
    }
}
Capabilities WindowsPlatform::capabilities() const {
    const auto helper = helperPath();
    DWORD attributes = helper.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(helper.c_str());
    bool selection = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    return {{selection, selection ? "" : "缺少 trans_selection_helper.exe，请重新安装完整 Windows 程序包。"},
            {!initializationError_, initializationError_.message},
            {true, ""},
            {true, "Windows 可能阻止后台窗口激活；焦点恢复不会强行抢占焦点。"}};
}
Error WindowsPlatform::setShortcuts(const std::string &selection, const std::string &screenshot, bool) {
    if (initializationError_)
        return initializationError_;
    std::array<Hotkey, 2> desired{};
    if (auto error = parseHotkey(selection, desired[0]))
        return error;
    if (auto error = parseHotkey(screenshot, desired[1]))
        return error;
    if (desired[0].key && desired[0] == desired[1])
        return {"截图翻译与选区翻译不能使用相同快捷键。"};
    Error error;
    invoke([&] { error = applyHotkeys(desired); });
    return error;
}
Error WindowsPlatform::applyHotkeys(const std::array<Hotkey, 2> &desired) {
    if (desired == hotkeys_)
        return {};
    const auto previous = hotkeys_;
    Error error;
    for (int i = 0; i < 2; ++i) {
        if (!hotkeys_[i].key)
            continue;
        if (!UnregisterHotKey(messageWindow_, i + 1) && GetLastError() != ERROR_HOTKEY_NOT_REGISTERED) {
            error = hotkeyError(GetLastError());
            break;
        }
        hotkeys_[i] = {};
    }
    if (!error) {
        for (int i = 0; i < 2; ++i) {
            if (desired[i].key &&
                !RegisterHotKey(messageWindow_, i + 1, desired[i].modifiers | MOD_NOREPEAT, desired[i].key)) {
                error = hotkeyError(GetLastError());
                break;
            }
            hotkeys_[i] = desired[i];
        }
    }
    if (!error)
        return {};
    for (int i = 0; i < 2; ++i) {
        if (hotkeys_[i] == previous[i])
            continue;
        if (hotkeys_[i].key && !UnregisterHotKey(messageWindow_, i + 1) &&
            GetLastError() != ERROR_HOTKEY_NOT_REGISTERED) {
            error.message += " 无法撤销新快捷键（Windows error " + std::to_string(GetLastError()) + "）。";
            continue;
        }
        hotkeys_[i] = {};
        if (previous[i].key &&
            !RegisterHotKey(messageWindow_, i + 1, previous[i].modifiers | MOD_NOREPEAT, previous[i].key))
            error.message +=
                " 原快捷键也未能恢复，当前未绑定（Windows error " + std::to_string(GetLastError()) + "）。";
        else
            hotkeys_[i] = previous[i];
    }
    return error;
}

InstanceResult WindowsPlatform::startInstance(Command initial, std::function<void(Command)> callback) {
    if (started_)
        return {InstanceResult::Failed, {"The instance channel has already been started."}};
    started_ = true;
    if (initializationError_)
        return {InstanceResult::Failed, initializationError_};
    if (unsigned(initial) > unsigned(Command::ShowSettings) || !callback)
        return {InstanceResult::Failed, {"Invalid instance command or callback."}};
    {
        std::lock_guard lock(callbackMutex_);
        callback_ = std::move(callback);
    }
    std::promise<InstanceResult> result;
    auto future = result.get_future();
    instanceThread_ = std::thread(
        [this, initial, result = std::move(result)]() mutable { instanceMain(initial, std::move(result)); });
    return future.get();
}
void WindowsPlatform::instanceMain(Command initial, std::promise<InstanceResult> result) {
    bool reported = false;
    try {
        auto fail = [&](Error error) {
            result.set_value({InstanceResult::Failed, std::move(error)});
            reported = true;
        };
        Identity identity;
        if (!readIdentity(GetCurrentProcess(), identity)) {
            fail(nativeError("Cannot determine Windows logon identity"));
            return;
        }
        auto name = endpointName(identity);
        if (name.empty()) {
            fail({"Cannot encode Windows instance identity."});
            return;
        }
        Security security;
        if (!security.initialize(identity)) {
            fail(nativeError("Cannot secure instance channel"));
            return;
        }
        const auto mutexName = L"Local\\" + name + L".mutex";
        const auto pipePath = L"\\\\.\\pipe\\" + name;
        Handle election(CreateMutexExW(security.attributes(), mutexName.c_str(), 0,
                                       SYNCHRONIZE | MUTEX_MODIFY_STATE | READ_CONTROL));
        if (!election || !verifyObjectSecurity(election.get(), identity)) {
            fail(nativeError("Cannot authenticate instance election mutex"));
            return;
        }
        const ULONGLONG deadline = GetTickCount64() + 25000;
        while (GetTickCount64() < deadline && WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
            DWORD elected = WaitForSingleObject(election.get(), 0);
            if (elected == WAIT_OBJECT_0 || elected == WAIT_ABANDONED) {
                struct Release {
                    HANDLE mutex;
                    ~Release() { ReleaseMutex(mutex); }
                } release{election.get()};
                Handle listener;
                do {
                    listener.reset(CreateNamedPipeW(
                        pipePath.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                        PIPE_UNLIMITED_INSTANCES, 128, 128, 0, security.attributes()));
                    if (listener)
                        break;
                    DWORD error = GetLastError();
                    if (error != ERROR_ACCESS_DENIED && error != ERROR_PIPE_BUSY) {
                        fail(nativeError("Cannot reserve instance pipe", error));
                        return;
                    }
                    if (WaitForSingleObject(stop_.get(), 25) == WAIT_OBJECT_0)
                        break;
                } while (GetTickCount64() < deadline);
                if (!listener) {
                    fail({"The instance pipe is already owned or is not accessible."});
                    return;
                }
                Handle connectedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!connectedEvent) {
                    fail(nativeError("Cannot initialize instance listener"));
                    return;
                }
                result.set_value({InstanceResult::Primary, {}});
                reported = true;
                serveInstances(std::move(listener), std::move(connectedEvent), pipePath, security, identity);
                return;
            }
            if (elected != WAIT_TIMEOUT) {
                fail(nativeError("Cannot elect primary instance"));
                return;
            }
            Handle pipe(CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            if (!pipe) {
                DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY && error != ERROR_PIPE_NOT_CONNECTED &&
                    error != ERROR_NO_DATA) {
                    fail(nativeError("Cannot connect to running Trans instance", error));
                    return;
                }
                WaitForSingleObject(stop_.get(), 25);
                continue;
            }
            ULONG peer = 0;
            if (!verifyPeer(pipe.get(), false, identity, &peer)) {
                DWORD error = GetLastError();
                if (error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
                    error == ERROR_INVALID_PARAMETER) {
                    WaitForSingleObject(stop_.get(), 25);
                    continue;
                }
                fail(nativeError("Cannot authenticate running Trans instance", error));
                return;
            }
            Frame incoming{};
            DWORD error = receiveFrame(pipe.get(), incoming, stop_.get(), deadline);
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
                WaitForSingleObject(stop_.get(), 25);
                continue;
            }
            std::uint32_t value = 0;
            if (error || !decodeFrame(incoming, 1, value) || value) {
                fail({"Invalid instance handshake or startup timeout."});
                return;
            }
            // A foreground launch can grant the authenticated primary permission;
            // this never simulates input or overrides the Windows foreground lock.
            AllowSetForegroundWindow(peer);
            auto outgoing = frame(2, unsigned(initial) + 1);
            error = transfer(pipe.get(), outgoing.data(), DWORD(outgoing.size()), true, stop_.get(), deadline);
            // After any attempted command write, delivery is ambiguous: no replay.
            if (error) {
                fail(nativeError("Command delivery failed; it will not be retried", error));
                return;
            }
            error = receiveFrame(pipe.get(), incoming, stop_.get(), deadline);
            if (error || !decodeFrame(incoming, 3, value)) {
                fail({"The primary did not acknowledge command dispatch; the command will not be retried."});
                return;
            }
            if (value) {
                fail({value == 2 ? "Trans did not become ready before the command timed out."
                                 : "The primary rejected the instance command."});
                return;
            }
            result.set_value({InstanceResult::Forwarded, {}});
            reported = true;
            return;
        }
        fail({"Timed out contacting the running Trans instance; any sent command will not be retried."});
    } catch (const std::exception &error) {
        if (!reported)
            result.set_value({InstanceResult::Failed, {std::string("Instance channel failed: ") + error.what()}});
    } catch (...) {
        if (!reported)
            result.set_value({InstanceResult::Failed, {"Instance channel failed."}});
    }
}
void WindowsPlatform::serveInstances(Handle listener, Handle event, const std::wstring &path, Security &security,
                                     const Identity &identity) {
    struct Client {
        std::shared_ptr<std::atomic_bool> done;
        std::jthread thread;
    };
    // Destruction joins every client before the election mutex is released.
    std::vector<Client> clients;
    while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0) {
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        ResetEvent(event.get());
        BOOL connected = ConnectNamedPipe(listener.get(), &operation);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error == ERROR_IO_PENDING) {
            HANDLE events[]{stop_.get(), event.get()};
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(listener.get(), &operation);
                DWORD ignored = 0;
                GetOverlappedResult(listener.get(), &operation, &ignored, TRUE);
                break;
            }
            DWORD ignored = 0;
            if (!GetOverlappedResult(listener.get(), &operation, &ignored, FALSE))
                error = GetLastError();
            else
                error = ERROR_SUCCESS;
        }
        if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) {
            DisconnectNamedPipe(listener.get());
            if (error == ERROR_NO_DATA)
                continue;
            break;
        }
        std::erase_if(clients, [](const Client &client) { return client.done->load(); });
        if (clients.size() >= 32 || !verifyPeer(listener.get(), true, identity)) {
            DisconnectNamedPipe(listener.get());
            continue;
        }
        Handle next(CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     PIPE_UNLIMITED_INSTANCES, 128, 128, 0, security.attributes()));
        if (!next) {
            DisconnectNamedPipe(listener.get());
            break;
        }
        auto done = std::make_shared<std::atomic_bool>(false);
        std::jthread worker([this, pipe = std::move(listener), done, identity]() mutable {
            try {
                serveClient(std::move(pipe), identity);
            } catch (...) {
            }
            done->store(true);
        });
        clients.push_back({std::move(done), std::move(worker)});
        listener = std::move(next);
    }
}
void WindowsPlatform::serveClient(Handle pipe, const Identity &identity) {
    const auto deadline = GetTickCount64() + 20000;
    auto hello = frame(1, 0);
    if (transfer(pipe.get(), hello.data(), DWORD(hello.size()), true, stop_.get(), deadline))
        return;
    Frame input{};
    std::uint32_t value = 0, reply = 1;
    DWORD error = receiveFrame(pipe.get(), input, stop_.get(), deadline);
    if (!error && decodeFrame(input, 2, value) && value >= 1 && value <= 4) {
        auto now = GetTickCount64();
        HANDLE events[]{stop_.get(), ready_.get()};
        DWORD wait = now < deadline ? WaitForMultipleObjects(2, events, FALSE, DWORD(deadline - now)) : WAIT_TIMEOUT;
        if (wait == WAIT_OBJECT_0)
            return;
        if (wait == WAIT_OBJECT_0 + 1) {
            DWORD extra = 0;
            if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &extra, nullptr))
                return;
            if (!extra && verifyPeer(pipe.get(), true, identity) && dispatch(Command(value - 1)))
                reply = 0;
        } else
            reply = 2;
    }
    auto response = frame(3, reply);
    const auto drainDeadline = GetTickCount64() + 1000;
    if (transfer(pipe.get(), response.data(), DWORD(response.size()), true, stop_.get(), drainDeadline))
        return;
    // Keep the pipe alive until the receiver drains its ACK, without a blocking
    // FlushFileBuffers that a malicious local client could hold indefinitely.
    BYTE ignored = 0;
    DWORD count = 0;
    pipeOperation(pipe.get(), &ignored, 1, count, false, stop_.get(), drainDeadline);
}

SourcePtr WindowsPlatform::captureSource() {
    auto source = std::make_shared<WindowsSource>();
    source->window = GetForegroundWindow();
    if (!source->window || !IsWindow(source->window))
        return {};
    source->thread = GetWindowThreadProcessId(source->window, &source->process);
    if (!source->thread || !source->process || source->process == GetCurrentProcessId())
        return {};
    source->lifetime.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, source->process));
    return source;
}
Selection WindowsPlatform::readSelection(const SourcePtr &captured, std::stop_token cancellation) {
    const auto source = std::dynamic_pointer_cast<const WindowsSource>(captured);
    if (!source)
        return {{}, selectionError(Protocol::Status::NoSelection)};
    auto context = [&] {
        return !cancellation.stop_requested() && source->matches(true) &&
               WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0;
    };
    if (!context())
        return {{}, selectionError(Protocol::Status::Cancelled)};
    if (!source->lifetime)
        return {{}, selectionError(Protocol::Status::PermissionDenied)};
    DWORD sourceIntegrity = 0, ownIntegrity = 0;
    if (!integrityLevel(source->lifetime.get(), sourceIntegrity) ||
        !integrityLevel(GetCurrentProcess(), ownIntegrity) || sourceIntegrity > ownIntegrity)
        return {{}, selectionError(Protocol::Status::PermissionDenied)};
    auto path = helperPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {{}, {"缺少 trans_selection_helper.exe，请重新安装完整 Windows 程序包。"}};
    Identity identity;
    Security security;
    if (!readIdentity(GetCurrentProcess(), identity) || !security.initialize(identity))
        return {{}, nativeError("Cannot secure selection helper pipes")};
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return {{}, nativeError("Cannot contain selection helper")};
    HelperPipe input, output, diagnostics;
    if (!input.create(true, security) || !output.create(false, security) || !diagnostics.create(false, security))
        return {{}, nativeError("Cannot create selection helper pipes")};
    std::array<HANDLE, 3> inherited{input.child.get(), output.child.get(), diagnostics.child.get()};
    ProcessAttributes attributes;
    if (!attributes.initialize(job.get(), inherited))
        return {{}, nativeError("Cannot isolate selection helper process")};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.child.get();
    startup.StartupInfo.hStdOutput = output.child.get();
    startup.StartupInfo.hStdError = diagnostics.child.get();
    startup.lpAttributeList = attributes.list();
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + path + L"\"";
    const auto deadline = GetTickCount64() + Protocol::TimeoutMs;
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                        &process))
        return {{}, nativeError("Cannot launch selection helper")};
    Handle processHandle(process.hProcess), threadHandle(process.hThread);
    // Closing the job kills descendants on every return path, including exceptions.
    struct Containment {
        Handle &job;
        HANDLE process;
        ~Containment() {
            job.reset();
            WaitForSingleObject(process, 1000);
        }
    } containment{job, processHandle.get()};
    input.child.reset();
    output.child.reset();
    diagnostics.child.reset();
    Protocol::Request request;
    if (!randomBytes(request.id))
        return {{}, {"Cannot generate selection request identity."}};
    request.window = reinterpret_cast<std::uintptr_t>(source->window);
    request.process = source->process;
    request.thread = source->thread;
    request.parent = GetCurrentProcessId();
    auto requestBytes = Protocol::encode(request);
    DWORD error = transfer(input.server.get(), requestBytes.data(), DWORD(requestBytes.size()), true, stop_.get(),
                           deadline, context);
    input.server.reset();
    if (error)
        return {{},
                error == ERROR_CANCELLED ? selectionError(Protocol::Status::Cancelled)
                                         : nativeError("Cannot send selection request", error)};
    std::vector<std::uint8_t> response;
    size_t stderrBytes = 0;
    auto drain = [&](HANDLE pipe, bool diagnostic) -> Error {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            DWORD failure = GetLastError();
            return failure == ERROR_BROKEN_PIPE ? Error{} : nativeError("Cannot read selection helper output", failure);
        }
        if (!available)
            return {};
        const size_t remaining =
            diagnostic ? Protocol::MaxStderrBytes - stderrBytes : Protocol::MaxResponseBytes - response.size();
        if (available > remaining)
            return {"选区读取程序返回的数据过大。"};
        std::array<std::uint8_t, 8192> buffer{};
        while (available) {
            DWORD count = 0;
            DWORD failure = pipeOperation(pipe, buffer.data(), std::min<DWORD>(available, DWORD(buffer.size())), count,
                                          false, stop_.get(), deadline, context);
            if (failure)
                return nativeError("Cannot read selection helper output", failure);
            if (!count)
                return {"Selection helper closed its output unexpectedly."};
            if (diagnostic)
                stderrBytes += count;
            else
                response.insert(response.end(), buffer.begin(), buffer.begin() + count);
            available -= count;
        }
        return {};
    };
    for (;;) {
        if (!context())
            return {{}, selectionError(Protocol::Status::Cancelled)};
        if (GetTickCount64() >= deadline)
            return {{}, {"读取选区超时。目标应用没有及时响应，请重试或使用截图翻译。"}};
        if (auto failure = drain(output.server.get(), false))
            return {{}, failure};
        if (auto failure = drain(diagnostics.server.get(), true))
            return {{}, failure};
        HANDLE events[]{processHandle.get(), stop_.get()};
        const auto now = GetTickCount64();
        DWORD wait = WaitForMultipleObjects(2, events, FALSE,
                                            now < deadline ? DWORD(std::min<ULONGLONG>(40, deadline - now)) : 0);
        if (wait == WAIT_OBJECT_0 + 1)
            return {{}, selectionError(Protocol::Status::Cancelled)};
        if (wait == WAIT_FAILED)
            return {{}, nativeError("Cannot wait for selection helper")};
        if (wait == WAIT_OBJECT_0) {
            if (auto failure = drain(output.server.get(), false))
                return {{}, failure};
            if (auto failure = drain(diagnostics.server.get(), true))
                return {{}, failure};
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(processHandle.get(), &exitCode) || exitCode)
                return {{}, selectionError(Protocol::Status::Failed)};
            break;
        }
    }
    if (!context())
        return {{}, selectionError(Protocol::Status::Cancelled)};
    Protocol::Status status = Protocol::Status::Failed;
    std::wstring selected;
    if (!Protocol::parseResponse(response, request, status, selected))
        return {{}, {"选区读取程序返回了无效数据。"}};
    if (status != Protocol::Status::Success)
        return {{}, selectionError(status)};
    std::string text;
    if (!toUtf8(selected, text))
        return {{}, {"选区读取程序返回了无效 Unicode。"}};
    return {std::move(text), {}};
}

Capture WindowsPlatform::captureScreens(std::stop_token cancellation) {
    Capture capture;
    auto cancelled = [&] {
        return cancellation.stop_requested() || WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0;
    };
    if (cancelled())
        return {{}, false, {"截图已取消。"}};
    DpiScope dpi;
    BOOL composition = FALSE;
    HRESULT result = DwmIsCompositionEnabled(&composition);
    if (FAILED(result))
        return {{}, false, {"无法确定 Windows 桌面合成状态。"}};
    if (composition && FAILED(DwmFlush()))
        return {{}, false, {"无法等待 Windows 桌面合成完成。"}};
    struct Monitors {
        std::vector<RECT> rects;
        bool failed = false;
    } monitors;
    if (!EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                auto &state = *reinterpret_cast<Monitors *>(data);
                MONITORINFO information{sizeof(information)};
                if (!GetMonitorInfoW(monitor, &information)) {
                    state.failed = true;
                    return FALSE;
                }
                try {
                    state.rects.push_back(information.rcMonitor);
                } catch (...) {
                    state.failed = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&monitors)) ||
        monitors.failed || monitors.rects.empty())
        return {{}, false, {"无法枚举 Windows 显示器。"}};
    struct ScreenDc {
        HDC value = GetDC(nullptr);
        ~ScreenDc() {
            if (value)
                ReleaseDC(nullptr, value);
        }
    } screen;
    if (!screen.value)
        return {{}, false, nativeError("Cannot access desktop pixels")};
    size_t totalBytes = 0;
    try {
        for (const auto &rectangle : monitors.rects) {
            if (cancelled())
                return {{}, false, {"截图已取消。"}};
            const auto width = std::int64_t(rectangle.right) - rectangle.left;
            const auto height = std::int64_t(rectangle.bottom) - rectangle.top;
            if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX ||
                std::uint64_t(width) * std::uint64_t(height) > 128 * 1024 * 1024)
                return {{}, false, {"显示器像素尺寸无效或超过安全限制。"}};
            const size_t bytes = size_t(width * height) * 4;
            if (bytes > 512 * 1024 * 1024 - totalBytes)
                return {{}, false, {"显示器总像素数据超过安全限制。"}};
            totalBytes += bytes;
            struct Bitmap {
                HDC dc = nullptr;
                HBITMAP bitmap = nullptr;
                HGDIOBJ previous = nullptr;
                ~Bitmap() {
                    if (previous && previous != HGDI_ERROR)
                        SelectObject(dc, previous);
                    if (bitmap)
                        DeleteObject(bitmap);
                    if (dc)
                        DeleteDC(dc);
                }
            } bitmap;
            bitmap.dc = CreateCompatibleDC(screen.value);
            if (!bitmap.dc)
                return {{}, false, nativeError("Cannot create screenshot DC")};
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = LONG(width);
            info.bmiHeader.biHeight = -LONG(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void *pixels = nullptr;
            bitmap.bitmap = CreateDIBSection(screen.value, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap.bitmap || !pixels)
                return {{}, false, nativeError("Cannot allocate screenshot bitmap")};
            bitmap.previous = SelectObject(bitmap.dc, bitmap.bitmap);
            if (!bitmap.previous || bitmap.previous == HGDI_ERROR)
                return {{}, false, nativeError("Cannot select screenshot bitmap")};
            if (!BitBlt(bitmap.dc, 0, 0, int(width), int(height), screen.value, rectangle.left, rectangle.top,
                        SRCCOPY | CAPTUREBLT) ||
                !GdiFlush())
                return {{}, false, nativeError("Cannot capture desktop pixels")};
            ScreenImage image;
            image.geometry = {rectangle.left, rectangle.top, int(width), int(height)};
            image.width = int(width);
            image.height = int(height);
            image.rgba.resize(bytes);
            const auto *bgra = static_cast<const BYTE *>(pixels);
            for (size_t offset = 0; offset < bytes; offset += 4) {
                image.rgba[offset] = bgra[offset + 2];
                image.rgba[offset + 1] = bgra[offset + 1];
                image.rgba[offset + 2] = bgra[offset];
                image.rgba[offset + 3] = 255;
            }
            capture.screens.push_back(std::move(image));
        }
    } catch (const std::bad_alloc &) {
        return {{}, false, {"没有足够内存保存截图。"}};
    }
    if (cancelled())
        return {{}, false, {"截图已取消。"}};
    return capture;
}
void WindowsPlatform::restoreSource(const SourcePtr &source) {
    auto context = std::dynamic_pointer_cast<const WindowsSource>(source);
    if (!context || !context->matches(false) || !IsWindowVisible(context->window))
        return;
    // Do not steal focus back from a third application the user has switched to.
    HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    if (foreground && foreground != context->window &&
        (!GetWindowThreadProcessId(foreground, &process) || process != GetCurrentProcessId()))
        return;
    SetForegroundWindow(context->window);
}
void WindowsPlatform::configureWindow(std::uintptr_t handle, bool popup, bool topmost, std::uintptr_t owner) {
    HWND window = reinterpret_cast<HWND>(handle);
    if (!IsWindow(window) || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        return;
    HWND ownerWindow = reinterpret_cast<HWND>(owner);
    if (ownerWindow) {
        DWORD ownerProcess = 0;
        if (ownerWindow == window || !IsWindow(ownerWindow) || !GetWindowThreadProcessId(ownerWindow, &ownerProcess) ||
            ownerProcess != GetCurrentProcessId())
            return;
    }
    if (reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_HWNDPARENT)) != ownerWindow)
        SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerWindow));
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    LONG_PTR desired = popup ? (style | WS_EX_TOOLWINDOW) & ~LONG_PTR(WS_EX_APPWINDOW)
                             : (style | WS_EX_APPWINDOW) & ~LONG_PTR(WS_EX_TOOLWINDOW);
    desired &= ~LONG_PTR(WS_EX_NOACTIVATE);
    if (style != desired)
        SetWindowLongPtrW(window, GWL_EXSTYLE, desired);
    LONG_PTR normal = GetWindowLongPtrW(window, GWL_STYLE);
    if (!(normal & WS_SYSMENU))
        SetWindowLongPtrW(window, GWL_STYLE, normal | WS_SYSMENU);
    ULONG_PTR classStyle = GetClassLongPtrW(window, GCL_STYLE);
    if (classStyle & CS_NOCLOSE)
        SetClassLongPtrW(window, GCL_STYLE, LONG_PTR(classStyle & ~ULONG_PTR(CS_NOCLOSE)));
    if (HMENU menu = GetSystemMenu(window, FALSE))
        EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
    SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    DrawMenuBar(window);
    if (popup) {
        SetWindowSubclass(window, popupProcedure, 1, 0);
        removeTaskbarTab(window);
    } else
        RemoveWindowSubclass(window, popupProcedure, 1);
}
void WindowsPlatform::activateWindow(std::uintptr_t handle) {
    HWND window = reinterpret_cast<HWND>(handle);
    if (!IsWindow(window) || !IsWindowVisible(window))
        return;
    if (IsIconic(window))
        ShowWindowAsync(window, SW_RESTORE);
    SetForegroundWindow(window);
}
Rect WindowsPlatform::cursorGeometry() {
    DpiScope dpi;
    POINT position{};
    return GetCursorPos(&position) ? Rect{position.x, position.y, 1, 1} : Rect{};
}
Rect WindowsPlatform::availableGeometry(bool atCursor) {
    DpiScope dpi;
    HMONITOR monitor = nullptr;
    if (atCursor) {
        POINT position{};
        if (GetCursorPos(&position))
            monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
    } else
        monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!monitor)
        monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{sizeof(information)};
    if (GetMonitorInfoW(monitor, &information)) {
        const auto &r = information.rcWork;
        return {r.left, r.top, r.right - r.left, r.bottom - r.top};
    }
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        return {work.left, work.top, work.right - work.left, work.bottom - work.top};
    return {};
}
Error WindowsPlatform::copyText(const std::string &text) {
    if (initializationError_)
        return initializationError_;
    std::wstring wide;
    if (!toWide(text, wide))
        return {"剪贴板文本包含无效 Unicode。"};
    Error error;
    invoke([&] {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (!memory) {
            error = nativeError("Cannot allocate clipboard text");
            return;
        }
        void *data = GlobalLock(memory);
        if (!data) {
            error = nativeError("Cannot lock clipboard text");
            GlobalFree(memory);
            return;
        }
        std::memcpy(data, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
        bool opened = false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (OpenClipboard(messageWindow_)) {
                opened = true;
                break;
            }
            if (WaitForSingleObject(stop_.get(), 15) == WAIT_OBJECT_0)
                break;
        }
        if (!opened) {
            error = nativeError("Cannot open Windows clipboard");
            GlobalFree(memory);
            return;
        }
        if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
            error = nativeError("Cannot write Windows clipboard");
            GlobalFree(memory);
        }
        CloseClipboard();
    });
    return error;
}
std::string WindowsPlatform::configDirectory() const {
    DWORD integrity = SECURITY_MANDATORY_MEDIUM_RID;
    integrityLevel(GetCurrentProcess(), integrity);
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(integrity < SECURITY_MANDATORY_MEDIUM_RID ? FOLDERID_LocalAppDataLow
                                                                              : FOLDERID_LocalAppData,
                                    KF_FLAG_DONT_VERIFY, nullptr, &path)))
        return {};
    const std::wstring directory = std::wstring(path) + L"\\trans";
    CoTaskMemFree(path);
    std::string result;
    toUtf8(directory, result);
    return result;
}
Error WindowsPlatform::writePrivateFile(const std::string &path, const std::string &contents) {
    std::wstring wide;
    if (!toWide(path, wide) || wide.empty())
        return {"配置文件路径包含无效 Unicode。"};
    Identity identity;
    Security security;
    if (!readIdentity(GetCurrentProcess(), identity) || !security.initialize(identity, true))
        return nativeError("Cannot secure configuration file");
    std::error_code ec;
    auto target = std::filesystem::absolute(std::filesystem::path(wide), ec).lexically_normal();
    if (ec || target.filename().empty())
        return {"配置文件路径无效。"};
    const auto directory = target.parent_path();
    // Never follow directory reparse points while creating a private store.
    // Hold each opened directory without FILE_SHARE_DELETE until replacement
    // completes, preventing path-component swaps during the atomic write.
    std::vector<Handle> directories;
    auto current = directory.root_path();
    for (const auto &part : directory.relative_path()) {
        current /= part;
        if (!CreateDirectoryW(current.c_str(), security.attributes()) && GetLastError() != ERROR_ALREADY_EXISTS)
            return nativeError("Cannot create configuration directory");
        bool final = current == directory;
        // SetSecurityInfo reads the existing DACL and enumerates children when
        // propagating inheritable ACEs; WRITE_DAC alone is not sufficient.
        const DWORD access = FILE_READ_ATTRIBUTES | (final ? READ_CONTROL | WRITE_DAC | FILE_LIST_DIRECTORY : 0);
        Handle opened(CreateFileW(current.c_str(), access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!opened)
            return nativeError("Cannot inspect configuration directory");
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(opened.get(), &information))
            return nativeError("Cannot inspect configuration directory");
        if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return {"拒绝通过重解析点保存含凭据的配置。"};
        if (final) {
            PACL acl = nullptr;
            BOOL present = FALSE, defaulted = FALSE;
            if (!GetSecurityDescriptorDacl(security.descriptor(), &present, &acl, &defaulted) || !present)
                return nativeError("Cannot obtain configuration access policy");
            DWORD error = SetSecurityInfo(opened.get(), SE_FILE_OBJECT,
                                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                          nullptr, acl, nullptr);
            if (error != ERROR_SUCCESS)
                return nativeError("Cannot restrict configuration directory access", error);
        }
        directories.push_back(std::move(opened));
    }
    DWORD existing = GetFileAttributesW(target.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES && (existing & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return {"拒绝覆盖目录或重解析点配置文件。"};
    auto nonce = randomName();
    if (nonce.empty())
        return {"Cannot generate a private temporary filename."};
    const auto temporary = directory / (L".settings-" + nonce + L".tmp");
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, security.attributes(), CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return nativeError("Cannot create private configuration file");
    struct RemoveTemporary {
        const std::filesystem::path &path;
        Handle &file;
        ~RemoveTemporary() {
            file.reset();
            DeleteFileW(path.c_str());
        }
    } cleanup{temporary, file};
    size_t offset = 0;
    while (offset < contents.size()) {
        DWORD written = 0;
        DWORD count = DWORD(std::min<size_t>(contents.size() - offset, MAXDWORD));
        if (!WriteFile(file.get(), contents.data() + offset, count, &written, nullptr))
            return nativeError("Cannot write private configuration");
        if (!written)
            return nativeError("Cannot write private configuration", ERROR_WRITE_FAULT);
        offset += written;
    }
    if (!FlushFileBuffers(file.get()))
        return nativeError("Cannot flush private configuration");
    HANDLE raw = file.release();
    if (!CloseHandle(raw))
        return nativeError("Cannot close private configuration");
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return nativeError("Cannot atomically replace private configuration");
    return {};
}
} // namespace
std::unique_ptr<Platform> createPlatform() { return std::make_unique<WindowsPlatform>(); }
} // namespace Trans::Native
