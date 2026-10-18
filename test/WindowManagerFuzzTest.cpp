/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A structured fuzzer for the WindowManagerServer protocol: connect as a Taskbar
// would and send boundary/random values for every request the shell can make.
// The compositor must ignore unknown ids and clamp/reject geometry, never crash.
//
// Usage: libwm-wm-fuzz-test [--cases N]
// Env:   SDE_FUZZ_SEED   deterministic seed (default 1)
//
// The driver starts a compositor with a private WM portal and a real window, then
// checks the compositor is still alive and can still map a client afterwards.

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibCore/EventLoop.h>
#include <LibGUI/ConnectionToWindowManagerServer.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibMain/Main.h>
#include <stdlib.h>
#include <unistd.h>

static u64 s_rng = 1;

static u32 next_u32()
{
    // xorshift64*, enough for a deterministic stream without <random>.
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return static_cast<u32>((s_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static i32 next_i32()
{
    return static_cast<i32>(next_u32());
}

static Gfx::IntRect next_rect()
{
    return { next_i32(), next_i32(), next_i32(), next_i32() };
}

static ByteString next_keymap()
{
    switch (next_u32() % 4) {
    case 0:
        return {};
    case 1:
        return "us"sv;
    case 2:
        return ByteString::formatted("fuzz-{}", next_u32());
    default:
        return ByteString::formatted("{}\n{}", next_u32(), next_u32());
    }
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    unsigned cases = 500;
    if (auto const* env = getenv("SDE_FUZZ_CASES"); env && *env)
        cases = static_cast<unsigned>(strtoul(env, nullptr, 10));
    if (cases == 0)
        cases = 1;
    if (auto const* seed = getenv("SDE_FUZZ_SEED"); seed && *seed)
        s_rng = strtoull(seed, nullptr, 10) | 1;

    auto& wm = GUI::ConnectionToWindowManagerServer::the();

    for (unsigned i = 0; i < cases; ++i) {
        switch (next_u32() % 12) {
        case 0:
            wm.async_set_active_window(next_i32(), next_i32());
            break;
        case 1:
            wm.async_set_window_minimized(next_i32(), next_i32(), (next_u32() & 1) != 0);
            break;
        case 2:
            wm.async_set_window_taskbar_rect(next_i32(), next_i32(), next_rect());
            break;
        case 3:
            wm.async_set_workspace(next_u32(), next_u32());
            break;
        case 4:
            wm.async_set_applet_area_position({ next_i32(), next_i32() });
            break;
        case 5:
            wm.async_popup_window_menu(next_i32(), next_i32(), { next_i32(), next_i32() });
            break;
        case 6:
            wm.async_start_window_resize(next_i32(), next_i32(), next_i32());
            break;
        case 7:
            wm.async_toggle_show_desktop();
            break;
        case 8:
            wm.async_set_keymap(next_keymap());
            break;
        case 9:
            wm.async_set_manager_window(next_i32());
            break;
        case 10:
            wm.async_set_event_mask(next_u32());
            break;
        default:
            // A valid target too: wm_id 1 is the test driver's window.
            wm.async_set_window_taskbar_rect(0, 1, next_rect());
            wm.async_set_window_minimized(0, 1, (next_u32() & 1) != 0);
            wm.async_set_active_window(0, 1);
            break;
        }
    }

    // Let the compositor process the stream before the connection closes.
    usleep(300 * 1000);
    warnln("WM-FUZZ: sent {} messages (seed state {})", cases, s_rng);
    return 0;
}
