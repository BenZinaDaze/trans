#pragma once

#include "selection_reader.h"
#include "shortcut_service.h"
#include "screenshot_service.h"
#include "window_integration.h"
#include <memory>

namespace Trans {

class PlatformServices final {
public:
    PlatformServices(std::unique_ptr<SelectionReader> selection,
                     std::unique_ptr<ShortcutService> shortcuts,
                     std::unique_ptr<ScreenshotService> screenshots,
                     std::unique_ptr<WindowIntegration> windows);
    ~PlatformServices();
    SelectionReader &selection() const { return *m_selection; }
    ShortcutService &shortcuts() const { return *m_shortcuts; }
    ScreenshotService &screenshots() const { return *m_screenshots; }
    WindowIntegration &windows() const { return *m_windows; }
private:
    std::unique_ptr<SelectionReader> m_selection;
    std::unique_ptr<ShortcutService> m_shortcuts;
    std::unique_ptr<ScreenshotService> m_screenshots;
    std::unique_ptr<WindowIntegration> m_windows;
};

std::unique_ptr<PlatformServices> createPlatformServices();

} // namespace Trans
