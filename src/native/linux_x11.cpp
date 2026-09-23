#include "linux_dbus.h"
#include "linux_internal.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

namespace Trans::Native::Linux {
namespace {
struct Free {
    void operator()(void *p) const { std::free(p); }
};
template <class T> using XPtr = std::unique_ptr<T, Free>;
class XConnection {
  public:
    xcb_connection_t *connection = nullptr;
    xcb_screen_t *screen = nullptr;
    Wake wake;
    std::stop_token stop;
    std::stop_callback<std::function<void()>> cancel;
    explicit XConnection(std::stop_token token = {}) : stop(token), cancel(token, [this] { wake.signal(); }) {
        int number = 0;
        connection = xcb_connect(nullptr, &number);
        if (!connection || xcb_connection_has_error(connection)) {
            if (connection)
                xcb_disconnect(connection);
            connection = nullptr;
            throw std::runtime_error("无法连接 X11 显示服务。");
        }
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
        while (number-- && screens.rem)
            xcb_screen_next(&screens);
        if (!screens.rem) {
            xcb_disconnect(connection);
            connection = nullptr;
            throw std::runtime_error("未找到 X11 屏幕。");
        }
        screen = screens.data;
    }
    ~XConnection() {
        if (connection)
            xcb_disconnect(connection);
    }
    XConnection(const XConnection &) = delete;
    template <class T, class Cookie>
    XPtr<T> reply(Cookie cookie, Clock::time_point deadline = Clock::now() + std::chrono::seconds(3)) {
        xcb_flush(connection);
        for (;;) {
            if (stop.stop_requested()) {
                xcb_discard_reply(connection, cookie.sequence);
                throw std::runtime_error("操作已取消。");
            }
            void *raw = nullptr;
            xcb_generic_error_t *error = nullptr;
            if (xcb_poll_for_reply(connection, cookie.sequence, &raw, &error)) {
                XPtr<xcb_generic_error_t> failure(error);
                if (error || !raw) {
                    std::free(raw);
                    throw std::runtime_error("X11 请求失败（窗口可能已关闭）。");
                }
                return XPtr<T>(static_cast<T *>(raw));
            }
            if (xcb_connection_has_error(connection))
                throw std::runtime_error("X11 显示连接已断开。");
            if (!waitReadable(xcb_get_file_descriptor(connection), wake, deadline, stop)) {
                xcb_discard_reply(connection, cookie.sequence);
                throw std::runtime_error(stop.stop_requested() ? "操作已取消。" : "X11 请求超时。");
            }
        }
    }
    xcb_atom_t atom(const char *name) {
        return reply<xcb_intern_atom_reply_t>(xcb_intern_atom(connection, false, std::strlen(name), name))->atom;
    }
    XPtr<xcb_get_property_reply_t> property(xcb_window_t window, xcb_atom_t name, bool remove = false,
                                            std::uint32_t maxBytes = 8 * 1024 * 1024) {
        auto result = reply<xcb_get_property_reply_t>(
            xcb_get_property(connection, remove, window, name, XCB_GET_PROPERTY_TYPE_ANY, 0, maxBytes / 4));
        if (result->bytes_after)
            throw std::runtime_error("X11 属性超过安全大小限制。");
        return result;
    }
    XPtr<xcb_generic_event_t> event(Clock::time_point deadline) {
        for (;;) {
            if (stop.stop_requested())
                throw std::runtime_error("操作已取消。");
            if (auto *event = xcb_poll_for_event(connection))
                return XPtr<xcb_generic_event_t>(event);
            if (xcb_connection_has_error(connection))
                throw std::runtime_error("X11 显示连接已断开。");
            xcb_flush(connection);
            if (!waitReadable(xcb_get_file_descriptor(connection), wake, deadline, stop))
                throw std::runtime_error(stop.stop_requested() ? "操作已取消。" : "选区读取超时，请重新选择文字。");
        }
    }
};
struct XSource final : Source {
    xcb_window_t window;
    explicit XSource(xcb_window_t id) : window(id) {}
};
std::vector<Rect> monitors(XConnection &x) {
    auto geometry = x.reply<xcb_get_geometry_reply_t>(xcb_get_geometry(x.connection, x.screen->root));
    std::vector<Rect> result;
    const auto *extension = xcb_get_extension_data(x.connection, &xcb_randr_id);
    if (extension && extension->present) {
        auto version = x.reply<xcb_randr_query_version_reply_t>(xcb_randr_query_version(x.connection, 1, 5));
        if (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 5)) {
            auto reply =
                x.reply<xcb_randr_get_monitors_reply_t>(xcb_randr_get_monitors(x.connection, x.screen->root, true));
            for (auto monitor = xcb_randr_get_monitors_monitors_iterator(reply.get()); monitor.rem;
                 xcb_randr_monitor_info_next(&monitor)) {
                const auto &m = *monitor.data;
                if (m.width && m.height)
                    result.push_back({m.x, m.y, m.width, m.height});
            }
        } else if (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 3)) {
            auto resources = x.reply<xcb_randr_get_screen_resources_current_reply_t>(
                xcb_randr_get_screen_resources_current(x.connection, x.screen->root));
            const auto *crtcs = xcb_randr_get_screen_resources_current_crtcs(resources.get());
            const int count = xcb_randr_get_screen_resources_current_crtcs_length(resources.get());
            for (int i = 0; i < count; ++i) {
                auto crtc = x.reply<xcb_randr_get_crtc_info_reply_t>(
                    xcb_randr_get_crtc_info(x.connection, crtcs[i], resources->config_timestamp));
                if (crtc->width && crtc->height) {
                    Rect r{crtc->x, crtc->y, crtc->width, crtc->height};
                    if (std::none_of(result.begin(), result.end(), [&](auto a) {
                            return a.x == r.x && a.y == r.y && a.width == r.width && a.height == r.height;
                        }))
                        result.push_back(r);
                }
            }
        }
    }
    if (result.empty())
        result.push_back({0, 0, geometry->width, geometry->height});
    return result;
}
void clientMessage(XConnection &x, xcb_window_t window, xcb_atom_t type, const std::array<std::uint32_t, 5> &data) {
    xcb_client_message_event_t message{};
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = window;
    message.type = type;
    std::copy(data.begin(), data.end(), message.data.data32);
    xcb_send_event(x.connection, false, x.screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&message));
    // Drain the outgoing request before the short-lived connection is closed.
    x.reply<xcb_get_input_focus_reply_t>(xcb_get_input_focus(x.connection));
}
std::string decodeSelection(const xcb_get_property_reply_t *property, xcb_atom_t utf8, xcb_atom_t plain) {
    if (property->format != 8)
        throw std::runtime_error("选区所有者返回了非文本数据。");
    const auto *bytes = static_cast<const unsigned char *>(xcb_get_property_value(property));
    const int size = xcb_get_property_value_length(property);
    if (property->type == utf8 || property->type == plain) {
        if (!dbus_validate_utf8(std::string(reinterpret_cast<const char *>(bytes), size).c_str(), nullptr))
            throw std::runtime_error("选区所有者返回了无效的 UTF-8 文本。");
        return std::string(reinterpret_cast<const char *>(bytes), size);
    }
    if (property->type != XCB_ATOM_STRING)
        throw std::runtime_error("选区所有者没有提供 UTF-8 或 Latin-1 文本。");
    std::string result;
    result.reserve(size);
    for (int i = 0; i < size; ++i) {
        if (bytes[i] < 128)
            result.push_back(char(bytes[i]));
        else {
            result.push_back(char(0xc0 | (bytes[i] >> 6)));
            result.push_back(char(0x80 | (bytes[i] & 63)));
        }
    }
    return result;
}
std::uint8_t channel(std::uint32_t pixel, std::uint32_t mask) {
    if (!mask)
        return 0;
    const unsigned shift = std::countr_zero(mask);
    const auto maximum = mask >> shift;
    return std::uint8_t((std::uint64_t((pixel & mask) >> shift) * 255 + maximum / 2) / maximum);
}
} // namespace

Capability x11FocusCapability() {
    try {
        XConnection x;
        const auto active = x.atom("_NET_ACTIVE_WINDOW");
        auto supported = x.property(x.screen->root, x.atom("_NET_SUPPORTED"), false, 16384);
        if (supported->type == XCB_ATOM_ATOM && supported->format == 32) {
            const auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(supported.get()));
            const auto count = xcb_get_property_value_length(supported.get()) / 4;
            if (std::find(atoms, atoms + count, active) != atoms + count)
                return {true, {}};
        }
        return {false, "当前 X11 窗口管理器不支持 EWMH 焦点恢复。"};
    } catch (const std::exception &error) {
        return {false, error.what()};
    }
}

Selection readPrimary(std::stop_token stop) {
    try {
        XConnection x(stop);
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        const auto utf8 = x.atom("UTF8_STRING"), targets = x.atom("TARGETS"), incr = x.atom("INCR");
        const auto plain = x.atom("text/plain;charset=utf-8"), property = x.atom("_TRANS_PRIMARY");
        const auto owner =
            x.reply<xcb_get_selection_owner_reply_t>(xcb_get_selection_owner(x.connection, XCB_ATOM_PRIMARY))->owner;
        if (!owner)
            return {{}, {"请先在其他应用中选中文字，再按翻译快捷键。"}};
        const xcb_window_t window = xcb_generate_id(x.connection);
        const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_create_window(x.connection, XCB_COPY_FROM_PARENT, window, x.screen->root, 0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, x.screen->root_visual, XCB_CW_EVENT_MASK, &mask);
        const std::uint32_t ownerMask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(x.connection, owner, XCB_CW_EVENT_MASK, &ownerMask);
        auto convert = [&](xcb_atom_t target) -> XPtr<xcb_get_property_reply_t> {
            xcb_delete_property(x.connection, window, property);
            xcb_convert_selection(x.connection, window, XCB_ATOM_PRIMARY, target, property, XCB_CURRENT_TIME);
            xcb_flush(x.connection);
            for (;;) {
                auto event = x.event(deadline);
                const auto kind = event->response_type & 0x7f;
                if (kind == XCB_DESTROY_NOTIFY &&
                    reinterpret_cast<xcb_destroy_notify_event_t *>(event.get())->window == owner)
                    throw std::runtime_error("选区来源窗口已关闭。");
                if (kind != XCB_SELECTION_NOTIFY)
                    continue;
                const auto &notify = *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
                if (notify.requestor != window || notify.selection != XCB_ATOM_PRIMARY || notify.target != target)
                    continue;
                if (!notify.property)
                    return {};
                if (notify.property != property)
                    throw std::runtime_error("选区所有者返回了无效属性。");
                return x.property(window, property);
            }
        };
        auto offered = convert(targets);
        xcb_atom_t target = utf8;
        if (offered && offered->type == XCB_ATOM_ATOM && offered->format == 32) {
            const auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(offered.get()));
            const auto count = xcb_get_property_value_length(offered.get()) / 4;
            const auto has = [&](xcb_atom_t a) { return std::find(atoms, atoms + count, a) != atoms + count; };
            if (has(utf8))
                target = utf8;
            else if (has(plain))
                target = plain;
            else if (has(XCB_ATOM_STRING))
                target = XCB_ATOM_STRING;
            else
                throw std::runtime_error("当前选区不包含可读取的文字。");
        }
        auto value = convert(target);
        if (!value && target != XCB_ATOM_STRING)
            value = convert(XCB_ATOM_STRING);
        if (!value)
            throw std::runtime_error("选区所有者拒绝了文本请求。");
        std::string text;
        if (value->type == incr) {
            if (value->format != 32 || xcb_get_property_value_length(value.get()) != 4)
                throw std::runtime_error("无效的增量选区协议。");
            const auto advertised = *static_cast<const std::uint32_t *>(xcb_get_property_value(value.get()));
            if (advertised > 8 * 1024 * 1024)
                throw std::runtime_error("选区过大，请缩小选择范围。");
            // Discard queued PropertyNotify from the initial conversion before acknowledging INCR.
            while (auto *pending = xcb_poll_for_event(x.connection))
                std::free(pending);
            xcb_delete_property(x.connection, window, property);
            xcb_flush(x.connection);
            xcb_atom_t chunkType = XCB_ATOM_NONE;
            for (;;) {
                auto event = x.event(deadline);
                const auto kind = event->response_type & 0x7f;
                if (kind == XCB_DESTROY_NOTIFY &&
                    reinterpret_cast<xcb_destroy_notify_event_t *>(event.get())->window == owner)
                    throw std::runtime_error("选区来源窗口已关闭。");
                if (kind != XCB_PROPERTY_NOTIFY)
                    continue;
                const auto &notify = *reinterpret_cast<xcb_property_notify_event_t *>(event.get());
                if (notify.window != window || notify.atom != property || notify.state != XCB_PROPERTY_NEW_VALUE)
                    continue;
                auto chunk = x.property(window, property, true);
                const auto size = xcb_get_property_value_length(chunk.get());
                if (chunk->format != 8 || (chunkType && chunk->type != chunkType))
                    throw std::runtime_error("无效的增量选区数据。");
                chunkType = chunk->type;
                if (!size)
                    break;
                if (text.size() + std::size_t(size) > 8 * 1024 * 1024)
                    throw std::runtime_error("选区过大，请缩小选择范围。");
                const auto *bytes = static_cast<const char *>(xcb_get_property_value(chunk.get()));
                text.append(bytes, size);
                xcb_flush(x.connection);
            }
            // UTF-8 code points may be split between chunks; validate only after reassembly.
            if (chunkType == XCB_ATOM_STRING) {
                std::string latin;
                latin.reserve(text.size());
                for (unsigned char c : text) {
                    if (c < 128)
                        latin.push_back(char(c));
                    else {
                        latin.push_back(char(0xc0 | (c >> 6)));
                        latin.push_back(char(0x80 | (c & 63)));
                    }
                }
                text = std::move(latin);
            } else if (chunkType != utf8 && chunkType != plain)
                throw std::runtime_error("不支持选区文本编码。");
        } else
            text = decodeSelection(value.get(), utf8, plain);
        if (text.find('\0') != std::string::npos || !dbus_validate_utf8(text.c_str(), nullptr))
            throw std::runtime_error("选区所有者返回了无效的 UTF-8 文本。");
        if (text.empty())
            return {{}, {"请先在其他应用中选中文字，再按翻译快捷键。"}};
        return {std::move(text), {}};
    } catch (const std::exception &error) {
        return {{}, {error.what()}};
    }
}

Capture captureX11(std::stop_token stop) {
    try {
        XConnection x(stop);
        Wake delay;
        std::stop_callback cancel(stop, [&] { delay.signal(); });
        pollfd fd{delay.fd(), POLLIN, 0};
        poll(&fd, 1, 150); // Let the compositor consume the UI's hide before capturing any screen.
        if (stop.stop_requested())
            throw std::runtime_error("截图已取消。");
        const auto layout = monitors(x);
        const auto *setup = xcb_get_setup(x.connection);
        std::uint32_t red = 0, green = 0, blue = 0;
        for (auto depth = xcb_screen_allowed_depths_iterator(x.screen); depth.rem; xcb_depth_next(&depth)) {
            for (auto visual = xcb_depth_visuals_iterator(depth.data); visual.rem; xcb_visualtype_next(&visual)) {
                if (visual.data->visual_id == x.screen->root_visual &&
                    visual.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                    red = visual.data->red_mask;
                    green = visual.data->green_mask;
                    blue = visual.data->blue_mask;
                }
            }
        }
        if (!red || !green || !blue)
            throw std::runtime_error("当前 X11 显示不使用受支持的 TrueColor 像素格式。");
        Capture result;
        std::size_t totalBytes = 0;
        for (const auto rect : layout) {
            if (rect.width <= 0 || rect.height <= 0 || rect.width > 32767 || rect.height > 32767 || rect.x < -32768 ||
                rect.x > 32767 || rect.y < -32768 || rect.y > 32767)
                throw std::runtime_error("屏幕尺寸超出截图范围。");
            const auto bytes = std::size_t(rect.width) * rect.height * 4;
            totalBytes += bytes;
            if (totalBytes > 512 * 1024 * 1024)
                throw std::runtime_error("屏幕图像过大，无法安全截图。");
            auto image = x.reply<xcb_get_image_reply_t>(
                xcb_get_image(x.connection, XCB_IMAGE_FORMAT_Z_PIXMAP, x.screen->root, std::int16_t(rect.x),
                              std::int16_t(rect.y), std::uint16_t(rect.width), std::uint16_t(rect.height), ~0u));
            int bpp = 0, pad = 0;
            for (auto format = xcb_setup_pixmap_formats_iterator(setup); format.rem; xcb_format_next(&format)) {
                if (format.data->depth == image->depth) {
                    bpp = format.data->bits_per_pixel;
                    pad = format.data->scanline_pad;
                    break;
                }
            }
            if ((bpp != 16 && bpp != 24 && bpp != 32) || (pad != 8 && pad != 16 && pad != 32))
                throw std::runtime_error("不支持当前 X11 截图像素格式。");
            const auto stride = ((std::size_t(rect.width) * bpp + pad - 1) / pad) * (pad / 8);
            if (stride * rect.height > std::size_t(xcb_get_image_data_length(image.get())))
                throw std::runtime_error("X11 返回了不完整的屏幕图像。");
            ScreenImage screen{rect, rect.width, rect.height, std::vector<std::uint8_t>(bytes)};
            const auto *source = xcb_get_image_data(image.get());
            const int step = bpp / 8;
            for (int row = 0; row < rect.height; ++row) {
                if (stop.stop_requested())
                    throw std::runtime_error("截图已取消。");
                const auto *pixel = source + row * stride;
                auto *dest = screen.rgba.data() + std::size_t(row) * rect.width * 4;
                for (int column = 0; column < rect.width; ++column, pixel += step, dest += 4) {
                    std::uint32_t packed = 0;
                    for (int byte = 0; byte < step; ++byte) {
                        const int shift =
                            setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ? byte * 8 : (step - 1 - byte) * 8;
                        packed |= std::uint32_t(pixel[byte]) << shift;
                    }
                    dest[0] = channel(packed, red);
                    dest[1] = channel(packed, green);
                    dest[2] = channel(packed, blue);
                    dest[3] = 255;
                }
            }
            result.screens.push_back(std::move(screen));
        }
        const auto after = monitors(x);
        if (after.size() != layout.size() ||
            !std::equal(after.begin(), after.end(), layout.begin(), [](auto a, auto b) {
                return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
            }))
            throw std::runtime_error("截图过程中显示器配置已改变，请重新截图。");
        return result;
    } catch (const std::exception &error) {
        return {{}, false, {error.what()}};
    }
}

SourcePtr captureX11Source(const std::vector<std::uintptr_t> &owned) {
    try {
        XConnection x;
        auto active = x.property(x.screen->root, x.atom("_NET_ACTIVE_WINDOW"), false, 4);
        if (active->format != 32 || xcb_get_property_value_length(active.get()) != 4)
            return {};
        const auto window = *static_cast<const xcb_window_t *>(xcb_get_property_value(active.get()));
        if (!window || std::find(owned.begin(), owned.end(), window) != owned.end())
            return {};
        return std::make_shared<XSource>(window);
    } catch (...) {
        return {};
    }
}

void activateX11(std::uintptr_t handle, std::uint32_t timestamp) {
    if (!handle || handle > std::numeric_limits<xcb_window_t>::max())
        return;
    try {
        XConnection x;
        const auto window = xcb_window_t(handle);
        x.reply<xcb_get_window_attributes_reply_t>(xcb_get_window_attributes(x.connection, window));
        auto desktop = x.property(window, x.atom("_NET_WM_DESKTOP"), false, 4);
        if (desktop->format == 32 && xcb_get_property_value_length(desktop.get()) == 4) {
            const auto number = *static_cast<const std::uint32_t *>(xcb_get_property_value(desktop.get()));
            if (number != 0xffffffff)
                clientMessage(x, x.screen->root, x.atom("_NET_CURRENT_DESKTOP"), {number, timestamp, 0, 0, 0});
        }
        clientMessage(x, window, x.atom("_NET_ACTIVE_WINDOW"), {1, timestamp, 0, 0, 0});
    } catch (...) { /* EWMH requests are advisory; a closed source cannot be restored. */
    }
}

void restoreX11Source(const SourcePtr &source, std::uint32_t timestamp) {
    if (const auto x = std::dynamic_pointer_cast<const XSource>(source))
        activateX11(x->window, timestamp);
}

void configureX11(std::uintptr_t handle, bool popup, bool topmost, std::uintptr_t owner) {
    if (!handle || handle > std::numeric_limits<xcb_window_t>::max())
        return;
    try {
        XConnection x;
        const auto window = xcb_window_t(handle);
        if (owner && owner != handle && owner <= std::numeric_limits<xcb_window_t>::max()) {
            const auto parent = xcb_window_t(owner);
            xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW,
                                32, 1, &parent);
        } else {
            xcb_delete_property(x.connection, window, XCB_ATOM_WM_TRANSIENT_FOR);
        }
        const auto type = x.atom(popup ? "_NET_WM_WINDOW_TYPE_UTILITY" : "_NET_WM_WINDOW_TYPE_NORMAL");
        xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, window, x.atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM,
                            32, 1, &type);
        const auto state = x.atom("_NET_WM_STATE");
        const auto taskbar = x.atom("_NET_WM_STATE_SKIP_TASKBAR"), pager = x.atom("_NET_WM_STATE_SKIP_PAGER"),
                   above = x.atom("_NET_WM_STATE_ABOVE");
        auto attributes = x.reply<xcb_get_window_attributes_reply_t>(xcb_get_window_attributes(x.connection, window));
        if (attributes->map_state == XCB_MAP_STATE_UNMAPPED) {
            auto previous = x.property(window, state);
            std::vector<xcb_atom_t> values;
            if (previous->format == 32 && previous->type == XCB_ATOM_ATOM) {
                const auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(previous.get()));
                values.assign(atoms, atoms + xcb_get_property_value_length(previous.get()) / 4);
            }
            for (const auto atom : {taskbar, pager, above})
                std::erase(values, atom);
            if (popup) {
                values.push_back(taskbar);
                values.push_back(pager);
            }
            if (topmost)
                values.push_back(above);
            xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, window, state, XCB_ATOM_ATOM, 32, values.size(),
                                values.data());
        } else {
            clientMessage(x, window, state, {popup ? 1u : 0u, taskbar, pager, 1, 0});
            clientMessage(x, window, state, {topmost ? 1u : 0u, above, 0, 1, 0});
        }
        x.reply<xcb_get_input_focus_reply_t>(xcb_get_input_focus(x.connection));
    } catch (...) { /* The supplied native window may already have been destroyed. */
    }
}

Rect x11CursorGeometry() {
    try {
        XConnection x;
        const auto pointer = x.reply<xcb_query_pointer_reply_t>(xcb_query_pointer(x.connection, x.screen->root));
        if (!pointer->same_screen)
            return {};
        return {pointer->root_x, pointer->root_y, 1, 1};
    } catch (...) {
        return {};
    }
}

Rect x11AvailableGeometry(bool atCursor) {
    try {
        XConnection x;
        const auto screens = monitors(x);
        Rect chosen = screens.front();
        if (atCursor) {
            auto pointer = x.reply<xcb_query_pointer_reply_t>(xcb_query_pointer(x.connection, x.screen->root));
            for (const auto rect : screens) {
                if (pointer->root_x >= rect.x && pointer->root_x < rect.x + rect.width && pointer->root_y >= rect.y &&
                    pointer->root_y < rect.y + rect.height) {
                    chosen = rect;
                    break;
                }
            }
        } else {
            const auto *extension = xcb_get_extension_data(x.connection, &xcb_randr_id);
            if (extension && extension->present) {
                auto primary = x.reply<xcb_randr_get_output_primary_reply_t>(
                    xcb_randr_get_output_primary(x.connection, x.screen->root));
                if (primary->output) {
                    auto output = x.reply<xcb_randr_get_output_info_reply_t>(
                        xcb_randr_get_output_info(x.connection, primary->output, XCB_CURRENT_TIME));
                    if (output->crtc) {
                        auto crtc = x.reply<xcb_randr_get_crtc_info_reply_t>(
                            xcb_randr_get_crtc_info(x.connection, output->crtc, XCB_CURRENT_TIME));
                        if (crtc->width && crtc->height)
                            chosen = {crtc->x, crtc->y, crtc->width, crtc->height};
                    }
                }
            }
        }
        unsigned desktop = 0;
        auto current = x.property(x.screen->root, x.atom("_NET_CURRENT_DESKTOP"), false, 4);
        if (current->format == 32 && xcb_get_property_value_length(current.get()) == 4)
            desktop = *static_cast<const std::uint32_t *>(xcb_get_property_value(current.get()));
        auto workarea = x.property(x.screen->root, x.atom("_NET_WORKAREA"), false, 16384);
        if (workarea->format == 32 &&
            std::uint64_t(desktop) * 4 + 4 <= std::uint64_t(xcb_get_property_value_length(workarea.get()) / 4)) {
            const auto *values =
                static_cast<const std::uint32_t *>(xcb_get_property_value(workarea.get())) + desktop * 4;
            const auto left = std::max<std::int64_t>(chosen.x, std::int32_t(values[0]));
            const auto top = std::max<std::int64_t>(chosen.y, std::int32_t(values[1]));
            const auto right = std::min<std::int64_t>(std::int64_t(chosen.x) + chosen.width,
                                                      std::int64_t(std::int32_t(values[0])) + values[2]);
            const auto bottom = std::min<std::int64_t>(std::int64_t(chosen.y) + chosen.height,
                                                       std::int64_t(std::int32_t(values[1])) + values[3]);
            if (right > left && bottom > top)
                chosen = {int(left), int(top), int(right - left), int(bottom - top)};
        }
        return chosen;
    } catch (...) {
        return {};
    }
}

class Clipboard::Impl {
  public:
    XConnection x;
    xcb_window_t window = xcb_generate_id(x.connection);
    xcb_atom_t clipboard = x.atom("CLIPBOARD"), utf8 = x.atom("UTF8_STRING"), targets = x.atom("TARGETS");
    xcb_atom_t text = x.atom("TEXT"), plain = x.atom("text/plain;charset=utf-8"), incr = x.atom("INCR");
    xcb_atom_t timestamp = x.atom("TIMESTAMP"), marker = x.atom("_TRANS_CLIPBOARD_TIME");
    xcb_timestamp_t acquired = XCB_CURRENT_TIME;
    std::shared_ptr<const std::string> contents;
    struct Transfer {
        xcb_window_t requestor;
        xcb_atom_t property, type;
        std::shared_ptr<const std::string> bytes;
        std::size_t offset = 0;
        Clock::time_point deadline;
    };
    std::vector<Transfer> transfers;
    Impl() {
        const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_create_window(x.connection, XCB_COPY_FROM_PARENT, window, x.screen->root, 0, 0, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, x.screen->root_visual, XCB_CW_EVENT_MASK, &mask);
        xcb_flush(x.connection);
    }
    void handle(xcb_generic_event_t *event) {
        const auto kind = event->response_type & 0x7f;
        if (kind == XCB_SELECTION_CLEAR) {
            const auto *clear = reinterpret_cast<xcb_selection_clear_event_t *>(event);
            if (clear->selection == clipboard && std::int32_t(clear->time - acquired) >= 0)
                contents.reset();
        } else if (kind == XCB_DESTROY_NOTIFY) {
            const auto destroyed = reinterpret_cast<xcb_destroy_notify_event_t *>(event)->window;
            std::erase_if(transfers, [&](const auto &transfer) { return transfer.requestor == destroyed; });
        } else if (kind == XCB_PROPERTY_NOTIFY) {
            const auto &property = *reinterpret_cast<xcb_property_notify_event_t *>(event);
            if (property.state != XCB_PROPERTY_DELETE)
                return;
            for (auto it = transfers.begin(); it != transfers.end(); ++it) {
                if (it->requestor != property.window || it->property != property.atom)
                    continue;
                const auto count = std::min<std::size_t>(65536, it->bytes->size() - it->offset);
                xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, it->requestor, it->property, it->type, 8,
                                    count, it->bytes->data() + it->offset);
                it->offset += count;
                it->deadline = Clock::now() + std::chrono::seconds(30);
                if (!count)
                    transfers.erase(it);
                break;
            }
        } else if (kind == XCB_SELECTION_REQUEST) {
            const auto &request = *reinterpret_cast<xcb_selection_request_event_t *>(event);
            xcb_selection_notify_event_t response{};
            response.response_type = XCB_SELECTION_NOTIFY;
            response.time = request.time;
            response.requestor = request.requestor;
            response.selection = request.selection;
            response.target = request.target;
            const auto property = request.property ? request.property : request.target;
            if (request.selection == clipboard && contents) {
                if (request.target == targets) {
                    const xcb_atom_t supported[]{targets, timestamp, utf8, plain, text, XCB_ATOM_STRING};
                    xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, request.requestor, property, XCB_ATOM_ATOM,
                                        32, std::size(supported), supported);
                    response.property = property;
                } else if (request.target == timestamp) {
                    xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, request.requestor, property,
                                        XCB_ATOM_INTEGER, 32, 1, &acquired);
                    response.property = property;
                } else if (request.target == utf8 || request.target == plain || request.target == text ||
                           request.target == XCB_ATOM_STRING) {
                    auto bytes = contents;
                    const auto type = request.target == text ? utf8 : request.target;
                    if (type == XCB_ATOM_STRING) {
                        auto latin = std::make_shared<std::string>();
                        latin->reserve(contents->size());
                        for (std::size_t i = 0; i < contents->size();) {
                            const auto lead = static_cast<unsigned char>((*contents)[i++]);
                            if (lead < 128) {
                                latin->push_back(char(lead));
                                continue;
                            }
                            unsigned value = lead & (lead < 0xe0 ? 31 : lead < 0xf0 ? 15 : 7);
                            const int extra = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
                            for (int j = 0; j < extra; ++j)
                                value = (value << 6) | (static_cast<unsigned char>((*contents)[i++]) & 63);
                            latin->push_back(value <= 255 ? char(value) : '?');
                        }
                        bytes = std::move(latin);
                    }
                    if (bytes->size() <= 65536) {
                        xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, request.requestor, property, type, 8,
                                            bytes->size(), bytes->data());
                        response.property = property;
                    } else if (transfers.size() < 16 &&
                               std::none_of(transfers.begin(), transfers.end(), [&](const auto &t) {
                                   return t.requestor == request.requestor && t.property == property;
                               })) {
                        const std::uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
                        xcb_change_window_attributes(x.connection, request.requestor, XCB_CW_EVENT_MASK, &events);
                        const auto count = std::uint32_t(bytes->size());
                        xcb_change_property(x.connection, XCB_PROP_MODE_REPLACE, request.requestor, property, incr, 32,
                                            1, &count);
                        transfers.push_back({request.requestor, property, type, std::move(bytes), 0,
                                             Clock::now() + std::chrono::seconds(30)});
                        response.property = property;
                    }
                }
            }
            xcb_send_event(x.connection, false, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                           reinterpret_cast<const char *>(&response));
        }
    }
};

Clipboard::Clipboard() : impl(std::make_unique<Impl>()) {}
Clipboard::~Clipboard() = default;
int Clipboard::descriptor() const { return xcb_get_file_descriptor(impl->x.connection); }
int Clipboard::timeout() const {
    int result = -1;
    for (const auto &transfer : impl->transfers) {
        const auto value = remaining(transfer.deadline);
        if (result < 0 || value < result)
            result = value;
    }
    return result;
}
void Clipboard::dispatch() {
    if (xcb_connection_has_error(impl->x.connection))
        throw std::runtime_error("X11 剪贴板连接已断开。");
    while (auto *event = xcb_poll_for_event(impl->x.connection)) {
        XPtr<xcb_generic_event_t> owned(event);
        impl->handle(event);
    }
    std::erase_if(impl->transfers, [](const auto &transfer) { return Clock::now() >= transfer.deadline; });
    xcb_flush(impl->x.connection);
}
Error Clipboard::copy(const std::string &text) {
    if (text.size() > 16 * 1024 * 1024 || text.find('\0') != std::string::npos ||
        !dbus_validate_utf8(text.c_str(), nullptr))
        return {"复制文本过大或包含无效 UTF-8 字符。"};
    try {
        auto contents = std::make_shared<const std::string>(text);
        const std::uint8_t value = 1;
        xcb_change_property(impl->x.connection, XCB_PROP_MODE_REPLACE, impl->window, impl->marker, XCB_ATOM_INTEGER, 8,
                            1, &value);
        xcb_flush(impl->x.connection);
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        for (;;) {
            auto event = impl->x.event(deadline);
            if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
                const auto &property = *reinterpret_cast<xcb_property_notify_event_t *>(event.get());
                if (property.window == impl->window && property.atom == impl->marker &&
                    property.state == XCB_PROPERTY_NEW_VALUE) {
                    impl->acquired = property.time;
                    break;
                }
            }
            impl->handle(event.get());
        }
        xcb_set_selection_owner(impl->x.connection, impl->window, impl->clipboard, impl->acquired);
        const auto owner = impl->x.reply<xcb_get_selection_owner_reply_t>(
            xcb_get_selection_owner(impl->x.connection, impl->clipboard));
        if (owner->owner != impl->window)
            return {"无法取得 X11 剪贴板所有权。"};
        impl->contents = std::move(contents);
        return {};
    } catch (const std::exception &error) {
        return {error.what()};
    }
}
} // namespace Trans::Native::Linux
