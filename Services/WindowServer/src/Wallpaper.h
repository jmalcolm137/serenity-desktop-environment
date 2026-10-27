/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

extern RefPtr<Gfx::Bitmap> g_wallpaper;
extern ByteString g_wallpaper_mode;

void write_sde_wallpaper(RefPtr<Gfx::Bitmap> const& bitmap);
void update_desktop_background(Server& server, Output& output);
void apply_wallpaper_to_outputs(Server& server);
void ignore_next_wallpaper_state_event();
void load_wallpaper_mode_and_color();
void load_wallpaper_state();
void initialize_wallpaper(ByteString const& resource_root);
void watch_wallpaper_state(Server& server);
