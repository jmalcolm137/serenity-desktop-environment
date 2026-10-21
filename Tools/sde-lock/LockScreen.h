/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

namespace SDELock {

struct UiState {
    ByteString clock;
    ByteString date;
    ByteString username;
    size_t password_length { 0 };
    ByteString message;
    bool authenticating { false };
    bool caps_lock { false };
    bool failed { false };
    bool caret_visible { true };
    bool button_hovered { false };
};

// Where the pieces sit for one output size. The renderer and the pointer hit-test
// both use this, so a click always lands on what was drawn.
struct Layout {
    Gfx::IntRect panel;
    Gfx::IntRect clock;
    Gfx::IntRect date;
    Gfx::IntRect avatar;
    Gfx::IntRect name;
    Gfx::IntRect field;
    Gfx::IntRect message;
    Gfx::IntRect button;
};

// The lock screen: the session's wallpaper, dimmed, with an XFCE-style centred
// panel (clock, date, avatar, user, password field, Unlock button). Drawn with
// LibGfx onto a bitmap the caller uploads to a wl_shm buffer; no LibGUI, so the
// client stays a plain ext-session-lock-v1 client (docs/11 §10).
class LockRenderer {
public:
    LockRenderer();
    ~LockRenderer();

    ErrorOr<void> initialize(ByteString const& resource_root, ByteString const& username);

    void set_wallpaper(RefPtr<Gfx::Bitmap> wallpaper) { m_wallpaper = move(wallpaper); }

    Layout layout_for(Gfx::IntSize) const;
    void render(Gfx::Bitmap& target, UiState const&) const;

    static RefPtr<Gfx::Bitmap> load_wallpaper(ByteString const& path);
    static ByteString default_wallpaper_path();

private:
    RefPtr<Gfx::Font> font(ByteString const& family, float size, unsigned weight) const;

    RefPtr<Gfx::Bitmap> m_wallpaper;
    RefPtr<Gfx::Bitmap> m_avatar;
    RefPtr<Gfx::Font> m_clock_font;
    RefPtr<Gfx::Font> m_date_font;
    RefPtr<Gfx::Font> m_name_font;
    RefPtr<Gfx::Font> m_field_font;
    RefPtr<Gfx::Font> m_button_font;
    RefPtr<Gfx::Font> m_message_font;
};

}
