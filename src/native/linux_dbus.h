#pragma once
#include "platform.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dbus/dbus.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>

namespace Trans::Native::Linux {
using Clock = std::chrono::steady_clock;
struct MessageDelete {
    void operator()(DBusMessage *p) const {
        if (p)
            dbus_message_unref(p);
    }
};
using Message = std::unique_ptr<DBusMessage, MessageDelete>;
struct BusDelete {
    void operator()(DBusConnection *p) const {
        if (p) {
            dbus_connection_close(p);
            dbus_connection_unref(p);
        }
    }
};
using Bus = std::unique_ptr<DBusConnection, BusDelete>;
struct BusError {
    DBusError value;
    BusError() { dbus_error_init(&value); }
    ~BusError() { dbus_error_free(&value); }
    std::string text() const { return value.message ? value.message : "D-Bus request failed"; }
};
struct BusFailure : std::runtime_error {
    std::string name;
    explicit BusFailure(const BusError &error)
        : std::runtime_error(error.text()), name(error.value.name ? error.value.name : "") {}
};
inline Bus openBus() {
    static const bool initialized = dbus_threads_init_default();
    if (!initialized)
        throw std::runtime_error("Cannot initialize D-Bus threading");
    BusError error;
    Bus bus(dbus_bus_get_private(DBUS_BUS_SESSION, &error.value));
    if (!bus)
        throw std::runtime_error(error.text());
    dbus_connection_set_exit_on_disconnect(bus.get(), false);
    return bus;
}
inline Message method(const char *service, const char *path, const char *interface, const char *name) {
    Message result(dbus_message_new_method_call(service, path, interface, name));
    if (!result)
        throw std::bad_alloc();
    dbus_message_set_auto_start(result.get(), false);
    return result;
}
inline Message call(DBusConnection *bus, DBusMessage *request, int timeout = 2500) {
    BusError error;
    Message reply(dbus_connection_send_with_reply_and_block(bus, request, timeout, &error.value));
    if (!reply)
        throw BusFailure(error);
    return reply;
}
inline void appendString(DBusMessageIter &iter, const char *text, int type = DBUS_TYPE_STRING) {
    if (!dbus_message_iter_append_basic(&iter, type, &text))
        throw std::bad_alloc();
}
inline void appendString(DBusMessageIter &iter, const std::string &text, int type = DBUS_TYPE_STRING) {
    appendString(iter, text.c_str(), type);
}
inline DBusMessageIter writer(DBusMessage *message) {
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    return iter;
}
inline std::string basicString(DBusMessageIter iter) {
    const auto type = dbus_message_iter_get_arg_type(&iter);
    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        throw std::runtime_error("Invalid D-Bus string response");
    const char *value = nullptr;
    dbus_message_iter_get_basic(&iter, &value);
    return value ? value : "";
}
inline std::string stringReply(DBusMessage *reply) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter))
        throw std::runtime_error("Empty D-Bus response");
    return basicString(iter);
}
inline std::string serviceOwner(DBusConnection *bus, const char *service) {
    auto request = method(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
    auto iter = writer(request.get());
    appendString(iter, service);
    return stringReply(call(bus, request.get()).get());
}
inline void addMatch(DBusConnection *bus, const std::string &rule) {
    auto request = method(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
    auto iter = writer(request.get());
    appendString(iter, rule);
    call(bus, request.get());
}
inline void appendOption(DBusMessageIter &dict, const char *key, const char *signature, int type, const void *value) {
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry))
        throw std::bad_alloc();
    appendString(entry, key);
    if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant) ||
        !dbus_message_iter_append_basic(&variant, type, value) ||
        !dbus_message_iter_close_container(&entry, &variant) || !dbus_message_iter_close_container(&dict, &entry))
        throw std::bad_alloc();
}
inline bool dictionaryValue(DBusMessageIter dictionary, const char *name, DBusMessageIter &value) {
    if (dbus_message_iter_get_arg_type(&dictionary) != DBUS_TYPE_ARRAY)
        return false;
    DBusMessageIter item;
    dbus_message_iter_recurse(&dictionary, &item);
    while (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&item, &entry);
        if (basicString(entry) == name && dbus_message_iter_next(&entry) &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &value);
            return true;
        }
        dbus_message_iter_next(&item);
    }
    return false;
}
inline unsigned uintProperty(DBusMessageIter dictionary, const char *name) {
    DBusMessageIter value;
    if (!dictionaryValue(dictionary, name, value) || dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_UINT32)
        return 0;
    dbus_uint32_t number = 0;
    dbus_message_iter_get_basic(&value, &number);
    return number;
}
class Wake {
    int fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

  public:
    Wake() {
        if (fd_ < 0)
            throw std::runtime_error("Cannot create I/O wake descriptor");
    }
    ~Wake() { close(fd_); }
    Wake(const Wake &) = delete;
    int fd() const { return fd_; }
    void signal() const {
        const std::uint64_t one = 1;
        (void)::write(fd_, &one, sizeof(one));
    }
    void drain() const {
        std::uint64_t value;
        while (::read(fd_, &value, sizeof(value)) > 0) {
        }
    }
};
inline int remaining(Clock::time_point deadline) {
    return int(std::clamp<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count(), 0, 2147483647));
}
inline bool waitReadable(int fd, const Wake &wake, Clock::time_point deadline, std::stop_token stop) {
    while (!stop.stop_requested()) {
        const int timeout = remaining(deadline);
        if (!timeout)
            return false;
        pollfd fds[]{{fd, POLLIN, 0}, {wake.fd(), POLLIN, 0}};
        const int result = poll(fds, 2, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        if (fds[1].revents) {
            wake.drain();
            if (stop.stop_requested())
                return false;
        }
        if (fds[0].revents)
            return true;
    }
    return false;
}
} // namespace Trans::Native::Linux
