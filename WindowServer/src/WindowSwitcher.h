/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

#include <AK/NonnullRefPtr.h>
#include <LibGfx/Forward.h>

// The Alt+Tab window switcher (SerenityOS WindowSwitcher). Compositor-side: it
// lists the current workspace's windows (Super+Tab: all of them), shows a
// Serenity-style overlay of live thumbnails, and activates the selection when the
// modifier is released. See docs/05-r3-plan.md §4 WS3.
namespace WindowSwitcher {

enum class Mode {
    CurrentWorkspace,
    AllWorkspaces,
};

// Operations the switcher calls back into the compositor.
struct HostHooks {
    Gfx::Palette const& (*palette)(Server&) { nullptr };
    void (*focus_toplevel)(Server&, Toplevel*) { nullptr };
    // Switch stacks (all-workspaces mode): WindowsManager's switch_workspace.
    void (*switch_workspace)(Server&, u32 row, u32 column, bool carry_focused) { nullptr };
};
void set_host_hooks(HostHooks);

bool is_visible();

// Handle a key. Alt+Tab / Super+Tab advance (Shift reverses); releasing the
// modifier activates the selection; Escape cancels. Returns true when consumed.
bool handle_key(Server&, u32 keysym, u32 modifiers, bool pressed);

// Re-snapshot the thumbnails while the switcher is open (e.g. on damage).
void refresh(Server&);

void hide();

}
