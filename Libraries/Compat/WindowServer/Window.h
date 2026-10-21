#pragma once

// Serenity Desktop Environment host compat: the Taskbar only needs
// WindowServer::WMEventMask from the WindowServer service's Window.h. That
// header pulls in kernel/framebuffer headers (ScreenBackend.h and friends) that
// do not exist on the host, so this shadows it for the Taskbar build.
// The values mirror Userland/Services/WindowServer/Window.h.
namespace WindowServer {

enum WMEventMask {
    WindowRectChanges = 1 << 0,
    WindowStateChanges = 1 << 1,
    WindowIconChanges = 1 << 2,
    WindowRemovals = 1 << 3,
    WorkspaceChanges = 1 << 4,
    KeymapChanged = 1 << 5,
};

}
