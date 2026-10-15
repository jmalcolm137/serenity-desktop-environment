/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

struct FrameIcons {
    RefPtr<Gfx::Bitmap> close;
    RefPtr<Gfx::Bitmap> minimize;
    RefPtr<Gfx::Bitmap> maximize;
    RefPtr<Gfx::Bitmap> restore;
    RefPtr<Gfx::Bitmap> move;
    RefPtr<Gfx::Bitmap> pin;
};

extern FrameIcons g_frame_icons;
extern RefPtr<Gfx::Bitmap> g_default_window_icon;
extern RefPtr<Gfx::Bitmap> g_x11_default_icon;
extern ByteString g_resource_root;

RefPtr<Gfx::Bitmap> make_x11_default_icon(Gfx::Color ink);
RefPtr<Gfx::Bitmap> load_icon(ByteString const& resource_root, StringView name, StringView fallback);
void load_overlay_shadow();
Gfx::Bitmap const* overlay_shadow_hook();
void reload_icon_bitmaps_after_scale_change(Server& server);
