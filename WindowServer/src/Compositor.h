/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// What every compositor subsystem (Keyboard, Pointer, XdgShell, XWayland, ...)
// shares: the model, the config, the log level and a few process-wide values.
// Each subsystem's own entry points are in its header.

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>

#include "Config.h"
#include "Model.h"

#include <xkbcommon/xkbcommon.h>

// R6/N8: the compositor's verbosity ([Log] Level in WindowServer.ini, overridden
// by SDE_LOG_LEVEL). It sets wlroots' log level and gates our own messages.
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

Optional<LogLevel> log_level_from_string(StringView name);
void apply_log_level(LogLevel level, StringView name);

template<typename T>
inline void remove_owned(Vector<NonnullOwnPtr<T>>& list, T* object)
{
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].ptr() == object) {
            list.remove(index);
            return;
        }
    }
}

extern i32 g_next_wm_id;
extern u64 g_focus_order_counter;
extern Gfx::Palette* g_palette;
extern size_t s_layout_offset;
