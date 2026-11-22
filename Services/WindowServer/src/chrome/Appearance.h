/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"
#include "Config.h"

void apply_theme_path(Server& server, ByteString const& theme_path);
ByteString high_contrast_theme_path();
// The theme in effect: the chosen one, or the high-contrast one while that is on.
Config::Theme effective_theme();
void publish_theme_to_broker(ByteString const& theme_path);
void watch_theme_broker(Server& server);
void publish_fonts_to_broker(Config::Fonts const& fonts);
void watch_fonts_broker(Server& server);
void apply_cursor_state(Server& server);
void watch_cursor_state(Server& server);
