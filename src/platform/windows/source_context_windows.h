#pragma once

#include "platform/window_integration.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Trans {

// Captured once; consumers must revalidate all three values before using the HWND.
class WindowsSourceContext final : public SourceContext {
public:
    WindowsSourceContext(HWND window, DWORD processId, DWORD threadId)
        : window(window), processId(processId), threadId(threadId) {}

    const HWND window;
    const DWORD processId;
    const DWORD threadId;
};

} // namespace Trans
