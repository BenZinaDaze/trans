#pragma once
#include "platform.h"

namespace Trans::Native::Linux {
Selection readPrimary(std::stop_token stop);
Capture captureX11(std::stop_token stop);
Capture capturePortal(std::stop_token stop);
Capability portalCapability();
SourcePtr captureX11Source(const std::vector<std::uintptr_t> &owned);
void activateX11(std::uintptr_t window, std::uint32_t timestamp);
void restoreX11Source(const SourcePtr &source, std::uint32_t timestamp);
void configureX11(std::uintptr_t window, bool popup, bool topmost, std::uintptr_t owner);
Rect x11AvailableGeometry(bool atCursor);
Rect x11CursorGeometry();
Capability x11FocusCapability();
int parseShortcut(const std::string &text); // KGlobalAccel wire key; -1 means invalid.
class Clipboard {
  public:
    Clipboard();
    ~Clipboard();
    int descriptor() const;
    int timeout() const;
    void dispatch();
    Error copy(const std::string &text);

  private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Trans::Native::Linux
