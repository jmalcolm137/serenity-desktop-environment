/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// What every compositor subsystem shares: the model, the config, the log level
// and a few process-wide values. Each subsystem's entry points are in its own
// header, and its state is a group of Server (see Model.h).
//
//   main.cpp              startup: backend, renderer, scene, seat; initialize()
//                         calls each subsystem's initialize_*()
//   Output, OutputManagement, Wallpaper, Performance
//                         outputs, wlr-output-management, desktop background,
//                         frame timing
//   XdgShell, LayerShell  xdg toplevels/popups/decorations, layer surfaces
//   Frame, WindowMenu, Animations
//                         server-side frames, the window menu, outline animations
//   Focus                 activation and stacking
//   InputDevices, Keyboard, Pointer, Tablet, DragAndDrop, CursorHighlight,
//   VirtualInput          input
//   XWayland, XWaylandFrame
//                         X11 clients and their frames
//   SerenityWindow        the serenity-window protocol (LibWindowServer chrome)
//   Interop               ext-workspace, foreign toplevels, activation, icons, bell
//   Idle, SessionLock     the R4.1 safety net
//   Appearance, Resources theme/font/cursor brokers, shared icons
//
// The namespaced modules are separate: WindowManager, WindowSwitcher and
// AppletManager (window-management policy), and SessionManager, InputMethod and
// InputCapture (protocols with their own state).

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
