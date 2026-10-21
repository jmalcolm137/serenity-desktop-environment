/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

void apply_theme_path(Server& server, ByteString const& theme_path);
ByteString high_contrast_theme_path();
void publish_theme_to_broker(ByteString const& theme_path);
void watch_theme_broker(Server& server);
void watch_fonts_broker(Server& server);
void apply_cursor_state(Server& server);
void watch_cursor_state(Server& server);
