#include "platform_services.h"

namespace Trans {

PlatformServices::PlatformServices(std::unique_ptr<SelectionReader> selection,
                                   std::unique_ptr<ShortcutService> shortcuts,
                                   std::unique_ptr<ScreenshotService> screenshots,
                                   std::unique_ptr<WindowIntegration> windows)
    : m_selection(std::move(selection)), m_shortcuts(std::move(shortcuts)),
      m_screenshots(std::move(screenshots)), m_windows(std::move(windows))
{
    Q_ASSERT(m_selection && m_shortcuts && m_screenshots && m_windows);
}

PlatformServices::~PlatformServices() = default;

} // namespace Trans
