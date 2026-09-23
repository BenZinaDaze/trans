#include "linux_dbus.h"
#include "linux_internal.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <png.h>
#include <sys/stat.h>

namespace Trans::Native::Linux {
namespace {
constexpr const char *portalService = "org.freedesktop.portal.Desktop";
constexpr const char *portalPath = "/org/freedesktop/portal/desktop";
constexpr const char *screenshotInterface = "org.freedesktop.portal.Screenshot";
constexpr const char *requestInterface = "org.freedesktop.portal.Request";
void probe(DBusConnection *bus, const char *owner) {
    auto request = method(owner, portalPath, "org.freedesktop.DBus.Properties", "GetAll");
    auto iter = writer(request.get());
    appendString(iter, screenshotInterface);
    auto reply = call(bus, request.get());
    DBusMessageIter properties;
    if (!dbus_message_iter_init(reply.get(), &properties) || uintProperty(properties, "version") < 3 ||
        !(uintProperty(properties, "AvailableTargets") & 4))
        throw std::runtime_error("当前桌面 Portal 不支持区域截图。请使用 Plasma X11，或升级到支持区域截图的桌面后端。");
}
void closeRequest(DBusConnection *bus, const std::string &owner, const std::string &path) {
    if (path.empty())
        return;
    auto message = method(owner.c_str(), path.c_str(), requestInterface, "Close");
    dbus_message_set_no_reply(message.get(), true);
    dbus_connection_send(bus, message.get(), nullptr);
    dbus_connection_read_write(bus, 0);
}
std::string localPath(const std::string &uri) {
    constexpr std::string_view scheme = "file://";
    if (!uri.starts_with(scheme))
        throw std::runtime_error("截图服务返回了无效的本地图片地址。");
    auto path = uri.substr(scheme.size());
    if (path.starts_with("localhost/"))
        path.erase(0, 9);
    if (path.empty() || path[0] != '/' || path.find_first_of("?#") != std::string::npos)
        throw std::runtime_error("截图服务返回了非本地图片地址。");
    const auto hex = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        char value = path[i];
        if (value == '%') {
            if (i + 2 >= path.size() || hex(path[i + 1]) < 0 || hex(path[i + 2]) < 0)
                throw std::runtime_error("截图服务返回了无效的文件 URL。");
            value = char(hex(path[i + 1]) * 16 + hex(path[i + 2]));
            i += 2;
        }
        if (!value)
            throw std::runtime_error("截图文件地址包含无效字符。");
        result.push_back(value);
    }
    return result;
}
ScreenImage readPng(const std::string &uri, std::stop_token stop) {
    const auto path = localPath(uri);
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        throw std::runtime_error("无法打开 Portal 返回的截图文件。");
    struct stat status{};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        status.st_size > 64 * 1024 * 1024) {
        close(descriptor);
        throw std::runtime_error("无法读取截图，或截图文件过大，请缩小截图范围。");
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(fdopen(descriptor, "rb"), std::fclose);
    if (!file) {
        close(descriptor);
        throw std::runtime_error("无法读取截图文件。");
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    struct Guard {
        png_image *image;
        ~Guard() { png_image_free(image); }
    } guard{&image};
    if (!png_image_begin_read_from_stdio(&image, file.get()))
        throw std::runtime_error("无法读取截图 PNG 图像。");
    if (std::min(image.width, image.height) < 15 || std::max(image.width, image.height) > 8192)
        throw std::runtime_error("截图短边须至少 15 像素，长边最多 8192 像素，请重新框选。");
    if (stop.stop_requested())
        throw std::runtime_error("截图已取消。");
    image.format = PNG_FORMAT_RGBA;
    ScreenImage output{{0, 0, int(image.width), int(image.height)},
                       int(image.width),
                       int(image.height),
                       std::vector<std::uint8_t>(PNG_IMAGE_SIZE(image))};
    if (!png_image_finish_read(&image, nullptr, output.rgba.data(), 0, nullptr))
        throw std::runtime_error("无法解码系统返回的 PNG 截图。");
    if (stop.stop_requested())
        throw std::runtime_error("截图已取消。");
    // The returned file belongs to the portal, not to Trans. Never remove it.
    return output;
}
struct ResponseQueue {
    std::string owner;
    std::deque<Message> messages;
    static DBusHandlerResult receive(DBusConnection *, DBusMessage *message, void *data) {
        auto &self = *static_cast<ResponseQueue *>(data);
        if (!dbus_message_is_signal(message, requestInterface, "Response") ||
            !dbus_message_has_sender(message, self.owner.c_str()))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        if (self.messages.size() < 32)
            self.messages.emplace_back(dbus_message_ref(message));
        return DBUS_HANDLER_RESULT_HANDLED;
    }
};
class Filter {
    DBusConnection *bus;

  public:
    Filter(DBusConnection *connection, ResponseQueue &responses) : bus(connection) {
        if (!dbus_connection_add_filter(bus, ResponseQueue::receive, &responses, nullptr))
            throw std::bad_alloc();
        data = &responses;
    }
    ~Filter() { dbus_connection_remove_filter(bus, ResponseQueue::receive, data); }

  private:
    ResponseQueue *data;
};
void dispatch(DBusConnection *bus) {
    if (!dbus_connection_read_write(bus, 0))
        throw std::runtime_error("桌面 D-Bus 连接已断开。");
    while (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS)
        dbus_connection_dispatch(bus);
}
} // namespace

Capability portalCapability() {
    try {
        auto bus = openBus();
        // The properties call is allowed to activate an installed Portal service.
        auto request = method(portalService, portalPath, "org.freedesktop.DBus.Properties", "GetAll");
        dbus_message_set_auto_start(request.get(), true);
        auto iter = writer(request.get());
        appendString(iter, screenshotInterface);
        auto reply = call(bus.get(), request.get());
        DBusMessageIter properties;
        if (!dbus_message_iter_init(reply.get(), &properties) || uintProperty(properties, "version") < 3 ||
            !(uintProperty(properties, "AvailableTargets") & 4))
            return {false, "当前桌面 Portal 不支持区域截图。请使用 Plasma X11，或升级桌面截图后端。"};
        return {true, {}};
    } catch (const std::exception &error) {
        return {false, std::string("桌面区域截图服务不可用：") + error.what()};
    }
}

Capture capturePortal(std::stop_token stop) {
    try {
        Wake wake;
        std::stop_callback cancel(stop, [&] { wake.signal(); });
        pollfd pause{wake.fd(), POLLIN, 0};
        poll(&pause, 1, 150);
        if (stop.stop_requested())
            throw std::runtime_error("截图已取消。");
        auto bus = openBus();
        const auto capability = portalCapability();
        if (!capability.available)
            throw std::runtime_error(capability.reason);
        const auto owner = serviceOwner(bus.get(), portalService);
        probe(bus.get(), owner.c_str());
        if (stop.stop_requested())
            throw std::runtime_error("截图已取消。");
        static std::atomic<std::uint64_t> counter = 0;
        const auto token = "trans_" + std::to_string(getpid()) + '_' + std::to_string(++counter);
        std::string sender = dbus_bus_get_unique_name(bus.get());
        sender.erase(0, 1);
        std::replace(sender.begin(), sender.end(), '.', '_');
        const auto predicted = std::string(portalPath) + "/request/" + sender + '/' + token;
        ResponseQueue responses{owner, {}};
        Filter filter(bus.get(), responses);
        // Subscribe before Screenshot and retain early responses, including non-predicted handles.
        addMatch(bus.get(),
                 "type='signal',sender='" + owner + "',interface='org.freedesktop.portal.Request',member='Response'");
        auto request = method(owner.c_str(), portalPath, screenshotInterface, "Screenshot");
        auto iter = writer(request.get());
        appendString(iter, "");
        DBusMessageIter options;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &options))
            throw std::bad_alloc();
        const char *tokenString = token.c_str();
        const dbus_bool_t interactive = true, modal = false;
        const dbus_uint32_t target = 4;
        appendOption(options, "handle_token", "s", DBUS_TYPE_STRING, &tokenString);
        appendOption(options, "interactive", "b", DBUS_TYPE_BOOLEAN, &interactive);
        appendOption(options, "modal", "b", DBUS_TYPE_BOOLEAN, &modal);
        appendOption(options, "target", "u", DBUS_TYPE_UINT32, &target);
        if (!dbus_message_iter_close_container(&iter, &options))
            throw std::bad_alloc();
        DBusPendingCall *raw = nullptr;
        if (!dbus_connection_send_with_reply(bus.get(), request.get(), &raw, 10000) || !raw)
            throw std::runtime_error("无法启动桌面截图请求。");
        std::unique_ptr<DBusPendingCall, decltype(&dbus_pending_call_unref)> pending(raw, dbus_pending_call_unref);
        dbus_connection_read_write(bus.get(), 0);
        std::string actual = predicted;
        struct RequestGuard {
            DBusConnection *bus;
            const std::string &owner;
            std::string &path;
            bool finished = false;
            ~RequestGuard() {
                if (!finished) {
                    try {
                        closeRequest(bus, owner, path);
                    } catch (...) {
                    }
                }
            }
        } guard{bus.get(), owner, actual};
        int descriptor = -1;
        if (!dbus_connection_get_unix_fd(bus.get(), &descriptor))
            throw std::runtime_error("无法监听桌面截图请求。");
        const auto methodDeadline = Clock::now() + std::chrono::seconds(10);
        bool cancelled = false;
        while (!dbus_pending_call_get_completed(pending.get())) {
            dispatch(bus.get());
            if (stop.stop_requested() && !cancelled) {
                cancelled = true;
                closeRequest(bus.get(), owner, predicted);
            }
            if (dbus_pending_call_get_completed(pending.get()))
                break;
            if (!remaining(methodDeadline)) {
                dbus_pending_call_cancel(pending.get());
                throw std::runtime_error(cancelled ? "截图已取消。" : "桌面截图请求超时。");
            }
            // After cancellation, still receive a late method handle so it can be closed too.
            pollfd fds[]{{descriptor, POLLIN, 0}, {wake.fd(), POLLIN, 0}};
            const int result = poll(fds, 2, remaining(methodDeadline));
            if (result < 0 && errno != EINTR)
                throw std::runtime_error("桌面截图 I/O 失败。");
            if (fds[1].revents)
                wake.drain();
        }
        Message reply(dbus_pending_call_steal_reply(pending.get()));
        if (!reply)
            throw std::runtime_error("桌面截图服务没有返回请求句柄。");
        if (dbus_message_get_type(reply.get()) == DBUS_MESSAGE_TYPE_ERROR) {
            BusError error;
            dbus_set_error_from_message(&error.value, reply.get());
            throw std::runtime_error(cancelled ? "截图已取消。" : error.text());
        }
        const auto handle = stringReply(reply.get());
        if (!handle.starts_with(std::string(portalPath) + "/request/"))
            throw std::runtime_error("桌面截图服务返回了无效请求句柄。");
        actual = handle;
        if (cancelled || stop.stop_requested())
            throw std::runtime_error("截图已取消。");
        const auto deadline = Clock::now() + std::chrono::seconds(180);
        for (;;) {
            dispatch(bus.get());
            if (stop.stop_requested())
                throw std::runtime_error("截图已取消。");
            while (!responses.messages.empty()) {
                auto response = std::move(responses.messages.front());
                responses.messages.pop_front();
                if (!dbus_message_has_path(response.get(), actual.c_str()))
                    continue;
                if (!dbus_message_has_signature(response.get(), "ua{sv}"))
                    throw std::runtime_error("桌面截图服务返回了无效结果。");
                DBusMessageIter values;
                dbus_message_iter_init(response.get(), &values);
                dbus_uint32_t code = 0;
                dbus_message_iter_get_basic(&values, &code);
                if (code != 0)
                    throw std::runtime_error(code == 1 ? "截图已取消。" : "系统截图失败，请重新截图。");
                dbus_message_iter_next(&values);
                DBusMessageIter uri;
                if (!dictionaryValue(values, "uri", uri))
                    throw std::runtime_error("桌面截图结果没有图片地址。");
                auto image = readPng(basicString(uri), stop);
                guard.finished = true;
                Capture result;
                result.regionSelected = true;
                result.screens.push_back(std::move(image));
                return result;
            }
            if (!waitReadable(descriptor, wake, deadline, stop))
                throw std::runtime_error(stop.stop_requested() ? "截图已取消。" : "截图请求超时，请重试。");
        }
    } catch (const std::exception &error) {
        return {{}, false, {error.what()}};
    }
}
} // namespace Trans::Native::Linux
