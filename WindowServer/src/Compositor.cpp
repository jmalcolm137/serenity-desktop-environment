/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"

#include <LibGfx/Palette.h>

#include "Wlroots.h"

static LogLevel g_log_level { LogLevel::Error };

static wlr_log_importance wlr_importance_for(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
    case LogLevel::Warn: // wlroots has no separate warn level
        return WLR_ERROR;
    case LogLevel::Info:
        return WLR_INFO;
    case LogLevel::Debug:
        return WLR_DEBUG;
    }
    return WLR_ERROR;
}

Optional<LogLevel> log_level_from_string(StringView name)
{
    if (name == "error"sv)
        return LogLevel::Error;
    if (name == "warn"sv)
        return LogLevel::Warn;
    if (name == "info"sv)
        return LogLevel::Info;
    if (name == "debug"sv)
        return LogLevel::Debug;
    return {};
}

static bool should_log(LogLevel level) { return static_cast<int>(level) <= static_cast<int>(g_log_level); }

void apply_log_level(LogLevel level, StringView name)
{
    g_log_level = level;
    wlr_log_init(wlr_importance_for(level), nullptr);
    if (should_log(LogLevel::Info))
        dbgln("WindowServer: log level '{}'", name);
}

i32 g_next_wm_id { 1 };

u64 g_focus_order_counter { 0 };

Gfx::Palette* g_palette { nullptr };

// The active entry in [Keyboard] Layouts (Alt+Shift cycles it); the keymap is
// rebuilt with the list rotated by this offset. Reset when the config reloads.
size_t s_layout_offset { 0 };
