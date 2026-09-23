#include "linux_dbus.h"
#include "linux_internal.h"
#include "platform.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <pwd.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>

namespace Trans::Native {
namespace {
using namespace Linux;
constexpr const char *instanceService = "io.github.trans.Trans";
constexpr const char *instancePath = "/Trans";
constexpr const char *shortcutService = "org.kde.kglobalaccel";
constexpr const char *shortcutInterface = "org.kde.KGlobalAccel";
constexpr const char *shortcutPath = "/kglobalaccel";
constexpr std::array<const char *, 2> actionNames{"translate-selection", "translate-screenshot"};
constexpr std::array<const char *, 2> actionLabels{"选区翻译", "截图翻译"};
constexpr std::array<const char *, 4> commandNames{"ShowTranslation", "TranslateSelection", "TranslateScreenshot",
                                                   "ShowSettings"};
using KeySequence = std::vector<dbus_int32_t>;
using Keys = std::vector<KeySequence>;
void actionId(DBusMessageIter &parent, std::size_t index) {
    DBusMessageIter array;
    if (!dbus_message_iter_open_container(&parent, DBUS_TYPE_ARRAY, "s", &array))
        throw std::bad_alloc();
    appendString(array, "trans");
    appendString(array, actionNames[index]);
    appendString(array, "Trans");
    appendString(array, actionLabels[index]);
    if (!dbus_message_iter_close_container(&parent, &array))
        throw std::bad_alloc();
}
void keySequence(DBusMessageIter &parent, const KeySequence &keys) {
    DBusMessageIter structure, array;
    if (!dbus_message_iter_open_container(&parent, DBUS_TYPE_STRUCT, nullptr, &structure) ||
        !dbus_message_iter_open_container(&structure, DBUS_TYPE_ARRAY, "i", &array))
        throw std::bad_alloc();
    for (const auto key : keys)
        if (!dbus_message_iter_append_basic(&array, DBUS_TYPE_INT32, &key))
            throw std::bad_alloc();
    if (!dbus_message_iter_close_container(&structure, &array) ||
        !dbus_message_iter_close_container(&parent, &structure))
        throw std::bad_alloc();
}
void keyList(DBusMessageIter &parent, const Keys &keys) {
    DBusMessageIter array;
    if (!dbus_message_iter_open_container(&parent, DBUS_TYPE_ARRAY, "(ai)", &array))
        throw std::bad_alloc();
    for (const auto &key : keys)
        keySequence(array, key);
    if (!dbus_message_iter_close_container(&parent, &array))
        throw std::bad_alloc();
}
Keys readKeys(DBusMessageIter parent) {
    if (dbus_message_iter_get_arg_type(&parent) != DBUS_TYPE_ARRAY)
        throw std::runtime_error("KDE 快捷键服务返回了无效按键列表。");
    Keys result;
    DBusMessageIter item;
    dbus_message_iter_recurse(&parent, &item);
    while (dbus_message_iter_get_arg_type(&item) != DBUS_TYPE_INVALID) {
        if (result.size() >= 128 || dbus_message_iter_get_arg_type(&item) != DBUS_TYPE_STRUCT)
            throw std::runtime_error("无效的 KDE 快捷键数据。");
        DBusMessageIter structure, integer;
        dbus_message_iter_recurse(&item, &structure);
        if (dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_ARRAY)
            throw std::runtime_error("无效的 KDE 快捷键序列。");
        dbus_message_iter_recurse(&structure, &integer);
        KeySequence sequence;
        while (dbus_message_iter_get_arg_type(&integer) != DBUS_TYPE_INVALID) {
            if (sequence.size() >= 4 || dbus_message_iter_get_arg_type(&integer) != DBUS_TYPE_INT32)
                throw std::runtime_error("无效的 KDE 快捷键编码。");
            dbus_int32_t value = 0;
            dbus_message_iter_get_basic(&integer, &value);
            sequence.push_back(value);
            dbus_message_iter_next(&integer);
        }
        while (!sequence.empty() && sequence.back() == 0)
            sequence.pop_back();
        if (!sequence.empty())
            result.push_back(std::move(sequence));
        dbus_message_iter_next(&item);
    }
    return result;
}
Keys replyKeys(DBusMessage *reply) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter))
        throw std::runtime_error("KDE 快捷键服务返回了空响应。");
    return readKeys(iter);
}
std::vector<std::string> strings(DBusMessageIter parent) {
    if (dbus_message_iter_get_arg_type(&parent) != DBUS_TYPE_ARRAY)
        return {};
    DBusMessageIter item;
    dbus_message_iter_recurse(&parent, &item);
    std::vector<std::string> result;
    while (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_STRING && result.size() < 16) {
        result.push_back(basicString(item));
        dbus_message_iter_next(&item);
    }
    return result;
}
std::string environment(const char *name) {
    const auto *value = std::getenv(name);
    return value ? value : "";
}
bool waylandSession() {
    const auto backend = environment("SLINT_BACKEND");
    if (backend.find("x11") != std::string::npos || environment("WINIT_UNIX_BACKEND") == "x11")
        return false;
    return !environment("WAYLAND_DISPLAY").empty() || environment("XDG_SESSION_TYPE") == "wayland";
}
std::string systemError(const char *operation) { return std::string(operation) + ": " + std::strerror(errno); }

class LinuxPlatform final : public Platform {
    Bus bus_;
    std::string busError_;
    const bool wayland_ = waylandSession();
    bool x11_ = false;
    std::unique_ptr<Clipboard> clipboard_;
    Wake wake_;
    mutable std::mutex taskMutex_;
    mutable std::deque<std::function<void()>> tasks_;
    std::jthread io_;
    bool started_ = false, primary_ = false, ready_ = false;
    std::function<void(Command)> callback_;
    struct Pending {
        Command command;
        Message request;
        Clock::time_point deadline;
    };
    std::deque<Pending> pending_;
    std::string shortcutOwner_, shortcutComponent_, shortcutError_;
    bool shortcutsWanted_ = false;
    std::array<bool, 2> registered_{}, initialized_{};
    std::array<Keys, 2> keys_, fallback_;
    std::atomic<std::uint32_t> activationTimestamp_ = 0;
    std::mutex windowMutex_;
    std::vector<std::uintptr_t> windows_;

    template <class Function> auto invoke(Function &&function) const -> std::invoke_result_t<Function> {
        using Result = std::invoke_result_t<Function>;
        if (std::this_thread::get_id() == io_.get_id())
            return std::forward<Function>(function)();
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard lock(taskMutex_);
            tasks_.emplace_back([task] { (*task)(); });
        }
        wake_.signal();
        return future.get();
    }
    void send(Message response) {
        if (response && bus_ && dbus_connection_get_is_connected(bus_.get()))
            dbus_connection_send(bus_.get(), response.get(), nullptr);
    }
    void fail(DBusMessage *request, const char *name, const char *text) {
        send(Message(dbus_message_new_error(request, name, text)));
    }
    void acknowledge(DBusMessage *request) { send(Message(dbus_message_new_method_return(request))); }
    void deliver(Command command, DBusMessage *request = nullptr) {
        try {
            if (!callback_)
                throw std::runtime_error("Trans has no command handler.");
            callback_(command);
            if (request)
                acknowledge(request);
        } catch (const std::exception &error) {
            if (request)
                fail(request, DBUS_ERROR_FAILED, error.what());
        } catch (...) {
            if (request)
                fail(request, DBUS_ERROR_FAILED, "Trans command handler failed.");
        }
    }
    void commandMessage(DBusMessage *message) {
        if (!dbus_message_has_path(message, instancePath)) {
            fail(message, DBUS_ERROR_UNKNOWN_OBJECT, "Unknown Trans object path.");
            return;
        }
        if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
            static constexpr const char *xml =
                "<node><interface name='io.github.trans.Trans'><method name='ShowTranslation'/><method "
                "name='TranslateSelection'/>"
                "<method name='TranslateScreenshot'/><method name='ShowSettings'/></interface>"
                "<interface name='org.freedesktop.DBus.Introspectable'><method name='Introspect'><arg type='s' "
                "direction='out'/></method></interface>"
                "<interface name='org.freedesktop.DBus.Peer'><method name='Ping'/></interface></node>";
            Message reply(dbus_message_new_method_return(message));
            auto iter = writer(reply.get());
            appendString(iter, xml);
            send(std::move(reply));
            return;
        }
        if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Peer", "Ping")) {
            acknowledge(message);
            return;
        }
        for (std::size_t i = 0; i < commandNames.size(); ++i) {
            if (!dbus_message_is_method_call(message, instanceService, commandNames[i]))
                continue;
            if (!dbus_message_has_signature(message, "")) {
                fail(message, DBUS_ERROR_INVALID_ARGS, "Trans commands take no arguments.");
                return;
            }
            if (!primary_) {
                fail(message, DBUS_ERROR_FAILED, "Trans has not acquired the instance name.");
                return;
            }
            if (ready_)
                deliver(Command(i), message);
            else if (pending_.size() >= 64)
                fail(message, DBUS_ERROR_LIMITS_EXCEEDED, "Too many commands are waiting for Trans startup.");
            else
                pending_.push_back(
                    {Command(i), Message(dbus_message_ref(message)), Clock::now() + std::chrono::seconds(20)});
            return;
        }
        fail(message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown Trans method.");
    }
    void signalMessage(DBusMessage *message) {
        if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
            dbus_message_has_sender(message, DBUS_SERVICE_DBUS) && dbus_message_has_signature(message, "sss")) {
            DBusMessageIter iter;
            dbus_message_iter_init(message, &iter);
            const auto name = basicString(iter);
            dbus_message_iter_next(&iter);
            dbus_message_iter_next(&iter);
            const auto owner = basicString(iter);
            if (name == shortcutService && owner != shortcutOwner_) {
                shortcutOwner_ = owner;
                shortcutComponent_.clear();
                registered_.fill(false);
                initialized_.fill(false);
                keys_ = {};
                if (owner.empty())
                    shortcutError_ = "KDE 全局快捷键服务已停止。";
                else if (shortcutsWanted_) {
                    try {
                        registerShortcuts();
                    } catch (const std::exception &error) {
                        shortcutError_ = error.what();
                    }
                } else
                    shortcutError_.clear();
            }
            return;
        }
        if (shortcutOwner_.empty() || !dbus_message_has_sender(message, shortcutOwner_.c_str()))
            return;
        if ((dbus_message_is_signal(message, "org.kde.kglobalaccel.Component", "globalShortcutPressed") ||
             dbus_message_is_signal(message, "org.kde.kglobalaccel.Component", "globalShortcutRepeated")) &&
            dbus_message_has_path(message, shortcutComponent_.c_str()) && dbus_message_has_signature(message, "ssx")) {
            DBusMessageIter iter;
            dbus_message_iter_init(message, &iter);
            if (basicString(iter) != "trans")
                return;
            dbus_message_iter_next(&iter);
            const auto action = basicString(iter);
            dbus_message_iter_next(&iter);
            dbus_int64_t timestamp = 0;
            dbus_message_iter_get_basic(&iter, &timestamp);
            activationTimestamp_.store(std::uint32_t(timestamp), std::memory_order_relaxed);
            if (ready_) {
                if (action == actionNames[0])
                    deliver(Command::TranslateSelection);
                else if (action == actionNames[1])
                    deliver(Command::TranslateScreenshot);
            }
        } else if (dbus_message_is_signal(message, shortcutInterface, "yourShortcutsChanged") &&
                   dbus_message_has_signature(message, "asa(ai)")) {
            DBusMessageIter iter;
            dbus_message_iter_init(message, &iter);
            const auto id = strings(iter);
            if (id.size() < 2 || id[0] != "trans")
                return;
            dbus_message_iter_next(&iter);
            for (std::size_t i = 0; i < 2; ++i)
                if (id[1] == actionNames[i])
                    fallback_[i] = keys_[i] = readKeys(iter);
        }
    }
    void expirePending() {
        while (!pending_.empty() && pending_.front().deadline <= Clock::now()) {
            fail(pending_.front().request.get(), DBUS_ERROR_TIMED_OUT,
                 "Trans did not become ready before the command timed out.");
            pending_.pop_front();
        }
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::deque<std::function<void()>> tasks;
            {
                std::lock_guard lock(taskMutex_);
                tasks.swap(tasks_);
            }
            for (auto &task : tasks)
                task();
            if (bus_ && dbus_connection_get_is_connected(bus_.get())) {
                dbus_connection_read_write(bus_.get(), 0);
                while (auto *raw = dbus_connection_pop_message(bus_.get())) {
                    Message message(raw);
                    try {
                        if (dbus_message_get_type(raw) == DBUS_MESSAGE_TYPE_METHOD_CALL)
                            commandMessage(raw);
                        else if (dbus_message_get_type(raw) == DBUS_MESSAGE_TYPE_SIGNAL)
                            signalMessage(raw);
                    } catch (const std::exception &error) {
                        if (dbus_message_get_type(raw) == DBUS_MESSAGE_TYPE_METHOD_CALL)
                            fail(raw, DBUS_ERROR_FAILED, error.what());
                        else
                            shortcutError_ = error.what();
                    }
                }
            }
            if (clipboard_) {
                try {
                    clipboard_->dispatch();
                } catch (const std::exception &) {
                    clipboard_.reset();
                    x11_ = false;
                }
            }
            expirePending();
            int timeout = pending_.empty() ? -1 : remaining(pending_.front().deadline);
            if (clipboard_) {
                const int value = clipboard_->timeout();
                if (value >= 0 && (timeout < 0 || value < timeout))
                    timeout = value;
            }
            std::array<pollfd, 3> descriptors{};
            descriptors[0] = {wake_.fd(), POLLIN, 0};
            descriptors[1].fd = -1;
            descriptors[2].fd = -1;
            if (bus_ && dbus_connection_get_is_connected(bus_.get())) {
                int fd = -1;
                dbus_connection_get_unix_fd(bus_.get(), &fd);
                descriptors[1] = {fd, short(POLLIN | (dbus_connection_has_messages_to_send(bus_.get()) ? POLLOUT : 0)),
                                  0};
            }
            if (clipboard_)
                descriptors[2] = {clipboard_->descriptor(), POLLIN, 0};
            if (poll(descriptors.data(), descriptors.size(), timeout) < 0 && errno != EINTR)
                break;
            if (descriptors[0].revents)
                wake_.drain();
        }
    }
    Message accelerator(const char *name, std::size_t index) {
        auto request = method(shortcutOwner_.c_str(), shortcutPath, shortcutInterface, name);
        auto iter = writer(request.get());
        actionId(iter, index);
        return request;
    }
    Keys setKeys(std::size_t index, const Keys &keys, dbus_uint32_t flags) {
        auto request = accelerator("setShortcutKeys", index);
        auto iter = writer(request.get());
        keyList(iter, keys);
        if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &flags))
            throw std::bad_alloc();
        return replyKeys(call(bus_.get(), request.get()).get());
    }
    void registerShortcuts() {
        if (!bus_ || !dbus_connection_get_is_connected(bus_.get()))
            throw std::runtime_error("会话 D-Bus 不可用。");
        const auto owner = serviceOwner(bus_.get(), shortcutService);
        if (owner != shortcutOwner_) {
            shortcutOwner_ = owner;
            registered_.fill(false);
            initialized_.fill(false);
            shortcutComponent_.clear();
        }
        // Wire flags from KGlobalAccel's SetShortcutFlag: SetPresent=2, NoAutoloading=4, IsDefault=8.
        for (std::size_t i = 0; i < 2; ++i) {
            if (initialized_[i])
                continue;
            auto registration = accelerator("doRegister", i);
            call(bus_.get(), registration.get());
            registered_[i] = true;
            const Keys defaults{{parseShortcut(i == 0 ? "Meta+Shift+T" : "Meta+Shift+O")}};
            setKeys(i, defaults, 8);
            fallback_[i] = keys_[i] = setKeys(i, fallback_[i], 2); // Autoload preserves the user's KDE binding.
            initialized_[i] = true;
        }
        auto component = method(shortcutOwner_.c_str(), shortcutPath, shortcutInterface, "getComponent");
        auto iter = writer(component.get());
        appendString(iter, "trans");
        shortcutComponent_ = stringReply(call(bus_.get(), component.get()).get());
        shortcutError_.clear();
    }
    Error applyShortcuts(const std::array<int, 2> &requested, bool replaceExisting) {
        if (!primary_)
            return {"只有主实例可注册全局快捷键。"};
        if (!shortcutsWanted_) {
            for (std::size_t i = 0; i < 2; ++i)
                fallback_[i] = requested[i] ? Keys{{requested[i]}} : Keys{};
        }
        shortcutsWanted_ = true;
        try {
            registerShortcuts();
            if (!replaceExisting)
                return {}; // The first registration never rewrites saved desktop bindings.
            std::array<Keys, 2> previous;
            for (std::size_t i = 0; i < 2; ++i) {
                auto request = accelerator("shortcutKeys", i);
                previous[i] = replyKeys(call(bus_.get(), request.get()).get());
            }
            std::array<Keys, 2> desired;
            for (std::size_t i = 0; i < 2; ++i) {
                if (!requested[i])
                    continue;
                desired[i] = Keys{{requested[i]}};
                // The daemon considers an action's own active shortcut occupied. Both old
                // bindings are exempt here so swapping our two actions remains possible.
                if (std::any_of(previous.begin(), previous.end(), [&](const auto &keys) {
                        return std::find(keys.begin(), keys.end(), KeySequence{requested[i]}) != keys.end();
                    }))
                    continue;
                auto available =
                    method(shortcutOwner_.c_str(), shortcutPath, shortcutInterface, "globalShortcutAvailable");
                auto iter = writer(available.get());
                keySequence(iter, KeySequence{requested[i]});
                appendString(iter, "trans");
                auto reply = call(bus_.get(), available.get());
                dbus_bool_t free = false;
                if (!dbus_message_get_args(reply.get(), nullptr, DBUS_TYPE_BOOLEAN, &free, DBUS_TYPE_INVALID))
                    throw std::runtime_error("KDE 返回了无效的快捷键可用状态。");
                if (!free)
                    return {"此快捷键已被其他应用占用，请录制另一个组合。"};
            }
            if (previous == desired) {
                fallback_ = keys_ = previous;
                return {};
            }
            try {
                // Release both before applying either: a direct swap must not conflict with itself.
                for (std::size_t i = 0; i < 2; ++i)
                    if (!setKeys(i, {}, 6).empty())
                        throw std::runtime_error("KDE 无法释放原有快捷键。");
                for (std::size_t i = 0; i < 2; ++i) {
                    keys_[i] = setKeys(i, desired[i], 6);
                    if (keys_[i] != desired[i])
                        throw std::runtime_error("KDE 未能应用快捷键，可能在保存时发生了冲突。");
                }
            } catch (const std::exception &error) {
                const std::string failure = error.what();
                bool restored = true;
                for (std::size_t i = 0; i < 2; ++i) {
                    try {
                        setKeys(i, {}, 6);
                    } catch (...) {
                        restored = false;
                    }
                }
                for (std::size_t i = 0; i < 2; ++i) {
                    try {
                        keys_[i] = setKeys(i, previous[i], 6);
                        if (keys_[i] != previous[i])
                            restored = false;
                    } catch (...) {
                        restored = false;
                    }
                }
                fallback_ = keys_;
                return {failure + (restored ? " 原有快捷键已恢复。" : " 原有快捷键恢复失败，请检查 KDE 快捷键设置。")};
            }
            fallback_ = keys_;
            shortcutError_.clear();
            return {};
        } catch (const std::exception &error) {
            shortcutError_ = error.what();
            return {shortcutError_};
        }
    }

  public:
    LinuxPlatform() {
        try {
            bus_ = openBus();
        } catch (const std::exception &error) {
            busError_ = error.what();
        }
        if (!wayland_) {
            try {
                clipboard_ = std::make_unique<Clipboard>();
                x11_ = true;
            } catch (...) {
                x11_ = false;
            }
        }
        if (bus_) {
            try {
                addMatch(bus_.get(), "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                                     "member='NameOwnerChanged',arg0='org.kde.kglobalaccel'");
                addMatch(bus_.get(),
                         "type='signal',sender='org.kde.kglobalaccel',interface='org.kde.kglobalaccel.Component'");
                addMatch(bus_.get(), "type='signal',sender='org.kde.kglobalaccel',interface='org.kde.KGlobalAccel',"
                                     "member='yourShortcutsChanged'");
                try {
                    shortcutOwner_ = serviceOwner(bus_.get(), shortcutService);
                } catch (...) {
                    shortcutError_ = "KDE 全局快捷键服务不可用，请检查桌面服务。";
                }
            } catch (const std::exception &error) {
                busError_ = error.what();
            }
        }
        io_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~LinuxPlatform() override {
        invoke([this] {
            ready_ = false;
            callback_ = {};
            for (const auto &pending : pending_)
                fail(pending.request.get(), DBUS_ERROR_FAILED, "Trans stopped before it could handle the command.");
            pending_.clear();
            if (bus_ && !shortcutOwner_.empty()) {
                for (std::size_t i = 0; i < 2; ++i)
                    if (registered_[i]) {
                        auto inactive = accelerator("setInactive", i);
                        dbus_message_set_no_reply(inactive.get(), true);
                        send(std::move(inactive));
                    }
                // Never unregister: that would erase the user's persisted desktop shortcuts.
                dbus_connection_read_write(bus_.get(), 0);
            }
            clipboard_.reset();
        });
        io_.request_stop();
        wake_.signal();
        io_.join();
    }
    Capabilities capabilities() const override {
        auto result = invoke([this] {
            Capabilities value;
            value.selection = {x11_, wayland_ ? "当前 Wayland 会话不允许读取其他应用选区，请使用截图翻译。"
                                              : "X11 选区服务不可用。"};
            if (value.selection.available)
                value.selection.reason.clear();
            const bool shortcut = bus_ && dbus_connection_get_is_connected(bus_.get()) && !shortcutOwner_.empty() &&
                                  shortcutError_.empty();
            value.shortcuts = {shortcut, shortcut                 ? ""
                                         : shortcutError_.empty() ? "KDE 全局快捷键服务不可用。"
                                                                  : shortcutError_};
            value.screenshot = {x11_, x11_ ? "" : "未找到可截图的 X11 屏幕。"};
            value.focus = {x11_, x11_ ? "" : "当前窗口系统不允许应用恢复其他窗口的焦点。"};
            return value;
        });
        if (wayland_)
            result.screenshot = portalCapability();
        else if (result.focus.available)
            result.focus = x11FocusCapability();
        return result;
    }
    InstanceResult startInstance(Command initial, std::function<void(Command)> callback) override {
        return invoke([this, initial, callback = std::move(callback)]() mutable -> InstanceResult {
            if (started_)
                return {InstanceResult::Failed, {"The instance channel has already been started."}};
            started_ = true;
            callback_ = std::move(callback);
            if (!bus_ || !dbus_connection_get_is_connected(bus_.get()))
                return {InstanceResult::Failed,
                        {busError_.empty() ? "Cannot connect to the session D-Bus." : busError_}};
            const auto command = std::size_t(initial);
            if (command >= commandNames.size())
                return {InstanceResult::Failed, {"Unknown application command."}};
            const auto deadline = Clock::now() + std::chrono::seconds(25);
            try {
                while (remaining(deadline) > 0) {
                    auto registration = method(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "RequestName");
                    auto iter = writer(registration.get());
                    appendString(iter, instanceService);
                    const dbus_uint32_t flags = DBUS_NAME_FLAG_DO_NOT_QUEUE;
                    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &flags);
                    auto reply = call(bus_.get(), registration.get(), remaining(deadline));
                    dbus_uint32_t role = 0;
                    if (!dbus_message_get_args(reply.get(), nullptr, DBUS_TYPE_UINT32, &role, DBUS_TYPE_INVALID))
                        throw std::runtime_error("Invalid D-Bus name election response.");
                    if (role == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
                        role == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
                        primary_ = true;
                        return {InstanceResult::Primary, {}};
                    }
                    try {
                        const auto owner = serviceOwner(bus_.get(), instanceService);
                        auto request = method(owner.c_str(), instancePath, instanceService, commandNames[command]);
                        if (!remaining(deadline))
                            break;
                        call(bus_.get(), request.get(), remaining(deadline));
                        return {InstanceResult::Forwarded, {}};
                    } catch (const BusFailure &error) {
                        // Retry only proven non-delivery. A timeout is ambiguous and must never duplicate a command.
                        if (error.name == DBUS_ERROR_NAME_HAS_NO_OWNER || error.name == DBUS_ERROR_SERVICE_UNKNOWN)
                            continue;
                        throw;
                    }
                }
                return {InstanceResult::Failed, {"Timed out while contacting the running Trans instance."}};
            } catch (const std::exception &error) {
                return {InstanceResult::Failed, {error.what()}};
            }
        });
    }
    void setReady() override {
        invoke([this] {
            if (!primary_ || !callback_)
                return;
            ready_ = true;
            expirePending();
            auto pending = std::move(pending_);
            pending_.clear();
            for (auto &request : pending) {
                if (request.deadline <= Clock::now())
                    fail(request.request.get(), DBUS_ERROR_TIMED_OUT, "Trans startup timed out.");
                else
                    deliver(request.command, request.request.get());
            }
        });
    }
    Error setShortcuts(const std::string &selection, const std::string &screenshot, bool replaceExisting) override {
        const std::array<int, 2> keys{parseShortcut(selection), parseShortcut(screenshot)};
        if (keys[0] < 0 || keys[1] < 0)
            return {"快捷键应包含 Ctrl、Alt 或 Meta，以及一个普通按键。清空可禁用快捷键。"};
        if (keys[0] && keys[0] == keys[1])
            return {"选区翻译与截图翻译不能使用相同的快捷键。"};
        return invoke([this, keys, replaceExisting] { return applyShortcuts(keys, replaceExisting); });
    }
    SourcePtr captureSource() override {
        if (wayland_)
            return {};
        std::vector<std::uintptr_t> owned;
        {
            std::lock_guard lock(windowMutex_);
            owned = windows_;
        }
        return captureX11Source(owned);
    }
    Selection readSelection(const SourcePtr &, std::stop_token stop) override {
        if (wayland_)
            return {{}, {"当前 Wayland 会话不允许读取其他应用选区，请使用截图翻译。"}};
        return readPrimary(stop);
    }
    Capture captureScreens(std::stop_token stop) override { return wayland_ ? capturePortal(stop) : captureX11(stop); }
    void restoreSource(const SourcePtr &source) override {
        if (!wayland_)
            restoreX11Source(source, activationTimestamp_.load(std::memory_order_relaxed));
    }
    void configureWindow(std::uintptr_t handle, bool popup, bool topmost, std::uintptr_t owner) override {
        if (!handle || wayland_)
            return;
        {
            std::lock_guard lock(windowMutex_);
            if (std::find(windows_.begin(), windows_.end(), handle) == windows_.end())
                windows_.push_back(handle);
        }
        configureX11(handle, popup, topmost, owner);
    }
    void activateWindow(std::uintptr_t handle) override {
        if (!wayland_)
            activateX11(handle, activationTimestamp_.load(std::memory_order_relaxed));
    }
    Rect availableGeometry(bool atCursor) override { return wayland_ ? Rect{} : x11AvailableGeometry(atCursor); }
    Rect cursorGeometry() override { return wayland_ ? Rect{} : x11CursorGeometry(); }
    Error copyText(const std::string &text) override {
        return invoke([this, &text]() -> Error {
            if (wayland_)
                return {"Wayland 剪贴板必须由拥有输入焦点的 Slint 窗口提供。"};
            if (!clipboard_)
                return {"X11 剪贴板服务不可用。"};
            return clipboard_->copy(text);
        });
    }
    std::string configDirectory() const override {
        auto base = environment("XDG_CONFIG_HOME");
        if (base.empty() || !std::filesystem::path(base).is_absolute()) {
            auto home = environment("HOME");
            if (home.empty()) {
                std::array<char, 16384> buffer{};
                passwd user{}, *result = nullptr;
                if (getpwuid_r(getuid(), &user, buffer.data(), buffer.size(), &result) == 0 && result && result->pw_dir)
                    home = result->pw_dir;
            }
            if (home.empty() || !std::filesystem::path(home).is_absolute())
                return {};
            base = (std::filesystem::path(home) / ".config").string();
        }
        return (std::filesystem::path(base) / "trans").string();
    }
    Error writePrivateFile(const std::string &path, const std::string &contents) override {
        int directory = -1, file = -1;
        std::string temporary;
        const auto cleanup = [&] {
            if (file >= 0)
                close(file);
            if (directory >= 0) {
                if (!temporary.empty())
                    unlinkat(directory, temporary.c_str(), 0);
                close(directory);
            }
        };
        try {
            const std::filesystem::path destination(path);
            if (destination.filename().empty() || destination.filename() == "." || destination.filename() == "..")
                throw std::runtime_error("无效的配置文件路径。");
            const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
                throw std::runtime_error("无法创建配置目录：" + error.message());
            directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (directory < 0)
                throw std::runtime_error(systemError("无法打开配置目录"));
            if (parent.lexically_normal() == std::filesystem::path(configDirectory()).lexically_normal()) {
                struct stat status{};
                if (fstat(directory, &status) != 0 || status.st_uid != geteuid() || fchmod(directory, 0700) != 0)
                    throw std::runtime_error("无法确保配置目录仅当前用户可访问。");
            }
            std::array<unsigned char, 16> random{};
            std::size_t received = 0;
            while (received < random.size()) {
                const auto count = getrandom(random.data() + received, random.size() - received, 0);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    throw std::runtime_error(systemError("无法生成安全临时文件名"));
                received += std::size_t(count);
            }
            temporary = ".trans-";
            for (auto byte : random) {
                temporary += "0123456789abcdef"[byte >> 4];
                temporary += "0123456789abcdef"[byte & 15];
            }
            file = openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (file < 0)
                throw std::runtime_error(systemError("无法创建私有配置文件"));
            if (fchmod(file, 0600) != 0)
                throw std::runtime_error(systemError("无法保护配置文件权限"));
            std::size_t written = 0;
            while (written < contents.size()) {
                const auto count = ::write(file, contents.data() + written, contents.size() - written);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    throw std::runtime_error(systemError("配置文件写入失败"));
                written += std::size_t(count);
            }
            if (fsync(file) != 0)
                throw std::runtime_error(systemError("配置文件同步失败"));
            if (close(file) != 0) {
                file = -1;
                throw std::runtime_error(systemError("配置文件关闭失败"));
            }
            file = -1;
            if (renameat(directory, temporary.c_str(), directory, destination.filename().c_str()) != 0)
                throw std::runtime_error(systemError("配置文件原子替换失败"));
            temporary.clear();
            if (fsync(directory) != 0)
                throw std::runtime_error(systemError("配置目录同步失败"));
            cleanup();
            return {};
        } catch (const std::exception &error) {
            cleanup();
            return {error.what()};
        }
    }
};
} // namespace
std::unique_ptr<Platform> createPlatform() { return std::make_unique<LinuxPlatform>(); }
} // namespace Trans::Native
