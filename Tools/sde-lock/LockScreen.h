/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace SDELock {

struct UiState {
    ByteString clock;
    ByteString date;
    ByteString display_name;
    ByteString keyboard_layout;
    size_t password_length { 0 };
    ByteString message;
    bool authenticating { false };
    bool caps_lock { false };
    bool failed { false };
    bool caret_visible { true };
    bool button_hovered { false };
};

// Where the pieces sit for one output size (in logical coordinates when
// scale == 1, or physical pixels when scaled). Both the renderer and the
// pointer hit-test use this, so clicks always land on what was drawn.
struct Layout {
    Gfx::IntRect panel;
    Gfx::IntRect shadow;
    Gfx::IntRect clock;
    Gfx::IntRect date;
    Gfx::IntRect avatar;
    Gfx::IntRect name;
    Gfx::IntRect field;
    Gfx::IntRect button;
    Gfx::IntRect message;
    Gfx::IntRect layout_badge;
};

// Pure helper: extracts the human display name from a passwd GECOS field
// (the substring before the first comma), falling back to `username`.
ByteString display_name_from_gecos(ByteString const& username, StringView gecos);

// The lock screen: the session's wallpaper, dimmed, with an XFCE-style centred
// panel (clock, date, avatar, user, password field, Unlock button, status line,
// and optional keyboard layout badge). Drawn with LibGfx onto a bitmap the
// caller uploads to a wl_shm buffer; no LibGUI, so the client stays a plain
// ext-session-lock-v1 client (docs/11 §10).
class LockRenderer {
public:
    LockRenderer();
    ~LockRenderer();

    ErrorOr<void> initialize(ByteString const& resource_root, ByteString const& username, ByteString const& display_name);

    void set_wallpaper(RefPtr<Gfx::Bitmap> wallpaper) { m_wallpaper = move(wallpaper); }

    Layout layout_for(Gfx::IntSize logical_size, int scale = 1) const;

    // Pre-renders the aspect-cropped, dimmed wallpaper (or gradient fallback)
    // for one output's physical pixel size. Call once on configure/scale change.
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> render_background(Gfx::IntSize pixel_size) const;

    // Draws the lock screen onto `target`, blitting from `cached_bg` (if
    // provided) and optionally restricting updates to `dirty_rect` (in physical
    // pixels). Secondary monitors pass `show_panel = false` to display only the
    // dimmed wallpaper and clock/date.
    void render(Gfx::Bitmap& target, Gfx::Bitmap const* cached_bg, UiState const&, Gfx::IntSize logical_size, int scale, bool show_panel, Optional<Gfx::IntRect> dirty_rect = {}) const;

    // Loads either the compositor's "PWDS" wallpaper.raw cache or any standard
    // image format supported by LibGfx (PNG, JPEG, BMP, ...).
    static RefPtr<Gfx::Bitmap> load_wallpaper(ByteString const& path);
    static ByteString default_wallpaper_path();

    // Renders a crisp arrow cursor bitmap at `scale` for wl_pointer_set_cursor.
    static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> make_cursor_bitmap(int scale);

private:
    struct FontSet {
        RefPtr<Gfx::Font> clock;
        RefPtr<Gfx::Font> date;
        RefPtr<Gfx::Font> name;
        RefPtr<Gfx::Font> field;
        RefPtr<Gfx::Font> button;
        RefPtr<Gfx::Font> message;
        RefPtr<Gfx::Font> badge;
    };

    RefPtr<Gfx::Font> load_font(ByteString const& family, float size, unsigned weight) const;
    FontSet const& fonts_for_scale(int scale) const;

    RefPtr<Gfx::Bitmap> m_wallpaper;
    RefPtr<Gfx::Bitmap> m_avatar;
    mutable HashMap<int, FontSet> m_fonts_by_scale;
};

}
