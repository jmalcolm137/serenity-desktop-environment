/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "CursorHighlight.h"

#include <LibGfx/Painter.h>

#include "Wlroots.h"

// --- R4.7: cursor highlight -------------------------------------------------
//
// A translucent ellipse behind the cursor, from cursor.ini [Highlight] (a radius
// of at least 20 enables it, matching Mouse Settings). Drawn as an overlay scene
// buffer so it composites over windows but under the cursor itself.
int g_cursor_highlight_radius { 0 };

Gfx::Color g_cursor_highlight_color { Gfx::Color::Transparent };

static OwnPtr<ChromeNode> s_cursor_highlight;

// SerenityOS's WindowManager defaults, used when Super+H turns the highlight on
// and Mouse Settings have not configured one.
static constexpr int cursor_highlight_default_radius { 25 };

// Super+H toggles the highlight; until it is used, the settings decide.
static Optional<bool> s_cursor_highlight_override;

static bool cursor_highlight_configured()
{
    return g_cursor_highlight_radius >= 20 && g_cursor_highlight_color.alpha() != 0;
}

static bool cursor_highlight_enabled()
{
    return s_cursor_highlight_override.value_or(cursor_highlight_configured());
}

static int cursor_highlight_radius()
{
    if (g_cursor_highlight_radius >= 20)
        return g_cursor_highlight_radius;
    return s_cursor_highlight_override.value_or(false) ? cursor_highlight_default_radius : 0;
}

static Gfx::Color cursor_highlight_color()
{
    if (g_cursor_highlight_color.alpha() != 0)
        return g_cursor_highlight_color;
    // SerenityOS's defaults: a red highlight at alpha 110.
    return Gfx::Color { 255, 0, 0, 110 };
}

void render_cursor_highlight(Server& server)
{
    bool const enabled = cursor_highlight_enabled() && cursor_highlight_radius() > 0;
    if (!enabled) {
        if (s_cursor_highlight)
            s_cursor_highlight->set_visible(false);
        return;
    }
    if (!server.overlay_tree || !server.cursor)
        return;
    if (!s_cursor_highlight)
        s_cursor_highlight = make<ChromeNode>(*server.overlay_tree);
    int const radius = cursor_highlight_radius();
    s_cursor_highlight->set_rect({ static_cast<int>(server.cursor->x) - radius, static_cast<int>(server.cursor->y) - radius, radius * 2, radius * 2 });
    auto color = cursor_highlight_color();
    s_cursor_highlight->render([radius, color](Gfx::Painter& painter) {
        painter.fill_ellipse({ 0, 0, radius * 2, radius * 2 }, color);
    });
    s_cursor_highlight->set_visible(true);
    s_cursor_highlight->raise_to_top();
}

void move_cursor_highlight(Server& server)
{
    if (!s_cursor_highlight || !s_cursor_highlight->is_visible() || !server.cursor)
        return;
    int const radius = cursor_highlight_radius();
    s_cursor_highlight->set_rect({ static_cast<int>(server.cursor->x) - radius, static_cast<int>(server.cursor->y) - radius, radius * 2, radius * 2 });
}

// SerenityOS: Super+H toggles the cursor highlight.
void toggle_cursor_highlight(Server& server)
{
    s_cursor_highlight_override = !cursor_highlight_enabled();
    render_cursor_highlight(server);
    dbgln("WindowServer: cursor highlight {}", cursor_highlight_enabled() ? "on" : "off");
}
