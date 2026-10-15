/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "CursorHighlight.h"
#include "Frame.h"
#include "Resources.h"
#include "WindowManager.h"
#include "XWaylandFrame.h"

#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>

#include "Wlroots.h"

FrameIcons g_frame_icons;

// The default window icon (SerenityOS WindowServer's default: window.png), used
// for toplevels whose client has not supplied one, so every window has an icon
// (titlebar, switcher, Taskbar) and the Taskbar never receives an invalid icon.
RefPtr<Gfx::Bitmap> g_default_window_icon;

// A distinct default icon for **X11 (Xwayland)** clients that supply none: a big
// monochrome "X" (in the spirit of the X11 logo), so an X11 window's titlebar is
// recognisable at a glance (vs. the generic window icon). Drawn in the titlebar's
// text colour at 64x64 and downscaled to 16x16 (the titlebar/switcher size) for
// crisp, anti-aliased diagonals.
RefPtr<Gfx::Bitmap> g_x11_default_icon;

RefPtr<Gfx::Bitmap> make_x11_default_icon(Gfx::Color ink)
{
    constexpr int big = 64;
    constexpr int small = 16;

    auto canvas = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { big, big });
    if (canvas.is_error())
        return nullptr;
    auto source = canvas.release_value();
    Gfx::Painter painter(*source);
    painter.clear_rect(source->rect(), Gfx::Color::Transparent);

    // A big "X" (no window frame), taller than it is wide like the X11 logo.
    Gfx::Path cross;
    cross.move_to({ 19, 5 });
    cross.line_to({ 45, 59 });
    cross.move_to({ 45, 5 });
    cross.line_to({ 19, 59 });
    painter.stroke_path(cross, ink, 10);

    auto result = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { small, small });
    if (result.is_error())
        return nullptr;
    auto downscaled = result.release_value();
    Gfx::Painter downscaled_painter(*downscaled);
    downscaled_painter.clear_rect(downscaled->rect(), Gfx::Color::Transparent);
    downscaled_painter.draw_scaled_bitmap(downscaled->rect(), *source, source->rect(), 1.0f, Gfx::ScalingMode::BilinearBlend);
    return downscaled;
}

RefPtr<Gfx::Bitmap> load_icon(ByteString const& resource_root, StringView name, StringView fallback)
{
    auto path = ByteString::formatted("{}/icons/16x16/{}.png", resource_root, name);
    if (auto bitmap = Gfx::Bitmap::load_from_file(path); !bitmap.is_error())
        return bitmap.release_value();
    if (!fallback.is_empty()) {
        auto fallback_path = ByteString::formatted("{}/icons/16x16/{}.png", resource_root, fallback);
        if (auto bitmap = Gfx::Bitmap::load_from_file(fallback_path); !bitmap.is_error())
            return bitmap.release_value();
    }
    return nullptr;
}

// The theme's overlay rect shadow (a 9-slice bitmap) behind the window switcher,
// like SerenityOS WindowManager::overlay_rect_shadow().
ByteString g_resource_root;

static RefPtr<Gfx::Bitmap> g_overlay_shadow;

void load_overlay_shadow()
{
    g_overlay_shadow = nullptr;
    if (!g_palette)
        return;
    auto path = g_palette->overlay_rect_shadow_path();
    if (path.is_empty())
        return;
    ByteString resolved = path;
    if (path.starts_with("/res"sv))
        resolved = ByteString::formatted("{}{}", g_resource_root, path.substring_view(4));
    if (auto bitmap = Gfx::Bitmap::load_from_file(resolved); !bitmap.is_error()) {
        g_overlay_shadow = bitmap.release_value();
        dbgln("WindowServer: loaded overlay rect shadow '{}' ({}x{})", resolved, g_overlay_shadow->width(), g_overlay_shadow->height());
    } else {
        warnln("WindowServer: could not load the overlay rect shadow '{}': {}", resolved, bitmap.error());
    }
}

Gfx::Bitmap const* overlay_shadow_hook()
{
    return g_overlay_shadow.ptr();
}

// SerenityOS WindowManager::reload_icon_bitmaps_after_scale_change(): re-read the
// config and re-render the chrome bitmaps, which are drawn at the output scale
// (the window frames carry the titlebar icon). The window icons are re-reported so
// the Taskbar refreshes its buttons too.
void reload_icon_bitmaps_after_scale_change(Server& server)
{
    Config::load();
    for (auto& toplevel : server.toplevels) {
        toplevel_update_frame(*toplevel);
        WindowManager::emit_window_icon(*toplevel);
    }
    for (auto& toplevel : server.xwayland.toplevels) {
        x11_update_frame(*toplevel);
        WindowManager::emit_window_icon(*toplevel);
    }
    render_cursor_highlight(server);
    dbgln("WindowServer: reloaded icon bitmaps ({} toplevels, {} x11 windows)",
        server.toplevels.size(), server.xwayland.toplevels.size());
}
