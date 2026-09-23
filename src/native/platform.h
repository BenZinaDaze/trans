#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace Trans::Native {
enum class Command { ShowTranslation, TranslateSelection, TranslateScreenshot, ShowSettings };
struct Error {
    std::string message;
    explicit operator bool() const { return !message.empty(); }
};
struct Source {
    virtual ~Source() = default;
};
using SourcePtr = std::shared_ptr<const Source>;
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};
struct ScreenImage {
    Rect geometry;
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
struct Selection {
    std::string text;
    Error error;
};
struct Capture {
    std::vector<ScreenImage> screens;
    bool regionSelected = false;
    Error error;
};
struct Capability {
    bool available = false;
    std::string reason;
};
struct Capabilities {
    Capability selection, shortcuts, screenshot, focus;
};
struct InstanceResult {
    enum Role { Primary, Forwarded, Failed } role = Failed;
    Error error;
};
// Platform owns its native event threads. Callbacks must be marshalled to the UI by the caller.
class Platform {
  public:
    virtual ~Platform() = default;
    virtual Capabilities capabilities() const = 0;
    virtual InstanceResult startInstance(Command initial, std::function<void(Command)> callback) = 0;
    virtual void setReady() = 0;
    virtual Error setShortcuts(const std::string &selection, const std::string &screenshot,
                               bool replaceExisting = false) = 0;
    virtual SourcePtr captureSource() = 0;
    virtual Selection readSelection(const SourcePtr &, std::stop_token) = 0;
    virtual Capture captureScreens(std::stop_token) = 0;
    virtual void restoreSource(const SourcePtr &) = 0;
    virtual void configureWindow(std::uintptr_t handle, bool popup, bool topmost, std::uintptr_t owner = 0) = 0;
    virtual void activateWindow(std::uintptr_t handle) = 0;
    virtual Rect availableGeometry(bool atCursor) = 0;
    virtual Rect cursorGeometry() = 0;
    virtual Error copyText(const std::string &) = 0;
    virtual std::string configDirectory() const = 0;
    virtual Error writePrivateFile(const std::string &path, const std::string &contents) = 0;
};
std::unique_ptr<Platform> createPlatform();
} // namespace Trans::Native
