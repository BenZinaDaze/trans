#include "platform/platform_services.h"

namespace Trans {

std::unique_ptr<PlatformServices> createPlatformServices()
{
    return std::make_unique<PlatformServices>(
        std::unique_ptr<SelectionReader>(createSelectionReader()),
        std::unique_ptr<ShortcutService>(createShortcutService()),
        std::unique_ptr<ScreenshotService>(createScreenshotService()),
        std::unique_ptr<WindowIntegration>(createWindowIntegration()));
}

} // namespace Trans
