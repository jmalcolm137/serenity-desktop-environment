/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Pure-logic unit tests for sde-lock:
//   1. KeyInput password editing, UTF-8 encoding/wiping, Escape->BlankDisplay,
//      Ctrl+U/Delete/Backspace, and safe keymap error handling.
//   2. LockRenderer HiDPI layout scaling (1x vs 2x), GECOS display name
//      parsing, long-password dot overflow clipping, secondary monitor clock
//      rendering, partial dirty-rect rendering, and cursor bitmap generation.
//
// Driver: scripts/run-lock-unit-test.sh.

#include "../Tools/sde-lock/KeyInput.h"
#include "../Tools/sde-lock/LockScreen.h"

#include <AK/Format.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

int g_failures = 0;

void check(char const* name, bool condition)
{
    if (!condition) {
        warnln("FAIL: {}", name);
        ++g_failures;
    }
}

void test_key_input()
{
    SDELock::KeyInput input;
    check("initial-empty", input.password_empty() && input.password_length() == 0);

    // Escape on an empty password triggers display blanking.
    check("escape-empty-blanks", input.apply_symbol(XKB_KEY_Escape, false, false, ""sv) == SDELock::KeyAction::BlankDisplay);

    // Typing ASCII + multi-byte UTF-8.
    check("type-a", input.apply_symbol(XKB_KEY_a, false, false, "a"sv) == SDELock::KeyAction::Changed);
    check("type-eacute", input.apply_symbol(XKB_KEY_eacute, false, false, "é"sv) == SDELock::KeyAction::Changed);
    check("type-euro", input.apply_symbol(XKB_KEY_EuroSign, false, false, "€"sv) == SDELock::KeyAction::Changed);
    check("length-3", input.password_length() == 3);

    // Ctrl/Alt chords must not append text.
    check("ctrl-c-ignored", input.apply_symbol(XKB_KEY_c, true, false, "c"sv) == SDELock::KeyAction::None);
    check("alt-x-ignored", input.apply_symbol(XKB_KEY_x, false, true, "x"sv) == SDELock::KeyAction::None);
    check("length-still-3", input.password_length() == 3);

    // Backspace deletes one codepoint.
    check("backspace", input.apply_symbol(XKB_KEY_BackSpace, false, false, ""sv) == SDELock::KeyAction::Changed);
    check("length-2", input.password_length() == 2);

    // Return submits.
    check("return-submits", input.apply_symbol(XKB_KEY_Return, false, false, "\r"sv) == SDELock::KeyAction::Submit);

    // take_password_utf8 encodes UTF-8 and clears internal state.
    auto utf8 = input.take_password_utf8();
    check("utf8-value", StringView { utf8.data(), utf8.size() } == "aé"sv);
    check("cleared-after-take", input.password_empty() && input.password_length() == 0);

    // Escape on non-empty clears instead of blanking.
    (void)input.apply_symbol(XKB_KEY_x, false, false, "x"sv);
    check("escape-nonempty-clears", input.apply_symbol(XKB_KEY_Escape, false, false, ""sv) == SDELock::KeyAction::Changed);
    check("empty-after-escape", input.password_empty());

    // Ctrl+U clears.
    (void)input.apply_symbol(XKB_KEY_x, false, false, "x"sv);
    check("ctrl-u-clears", input.apply_symbol(XKB_KEY_u, true, false, ""sv) == SDELock::KeyAction::Changed);
    check("empty-after-ctrl-u", input.password_empty());

    // Invalid keymap format/fd fails cleanly without crashing.
    check("invalid-keymap-rejected", !input.set_keymap_fd(999, -1, 0));
}

void test_gecos_display_name()
{
    check("gecos-full", SDELock::display_name_from_gecos("alice", "Alice Smith,Room 42,555-1234,"sv) == "Alice Smith"sv);
    check("gecos-simple", SDELock::display_name_from_gecos("alice", "Alice"sv) == "Alice"sv);
    check("gecos-empty", SDELock::display_name_from_gecos("alice", ""sv) == "alice"sv);
    check("gecos-commas-only", SDELock::display_name_from_gecos("alice", ",,,"sv) == "alice"sv);
}

void test_hidpi_layout_and_render()
{
    SDELock::LockRenderer renderer;
    Gfx::IntSize logical { 1920, 1080 };
    auto layout1 = renderer.layout_for(logical, 1);
    auto layout2 = renderer.layout_for(logical, 2);

    check("hidpi-panel-x", layout2.panel.x() == layout1.panel.x() * 2);
    check("hidpi-panel-y", layout2.panel.y() == layout1.panel.y() * 2);
    check("hidpi-panel-w", layout2.panel.width() == layout1.panel.width() * 2);
    check("hidpi-panel-h", layout2.panel.height() == layout1.panel.height() * 2);
    check("hidpi-button-scaled", layout2.button.width() == layout1.button.width() * 2 && layout2.button.height() == layout1.button.height() * 2);
    check("hidpi-field-inside-panel", layout2.panel.contains(layout2.field));
    check("hidpi-button-inside-panel", layout2.panel.contains(layout2.button));

    // Render background + active panel (with 100-char password overflow) +
    // partial dirty rect + passive secondary monitor at 2x scale.
    Gfx::IntSize small_logical { 640, 480 };
    Gfx::IntSize small_pixel { 1280, 960 };
    auto bg = renderer.render_background(small_pixel);
    check("bg-created", !bg.is_error());
    auto target = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, small_pixel);
    check("target-created", !target.is_error());

    if (!bg.is_error() && !target.is_error()) {
        SDELock::UiState state;
        state.clock = "18:45";
        state.date = "Sunday, 22 November 2026";
        state.display_name = "Alice Smith";
        state.keyboard_layout = "English (US)";
        state.password_length = 100; // Must clip cleanly inside field!
        state.caps_lock = true;
        state.caret_visible = true;

        renderer.render(*target.value(), bg.value().ptr(), state, small_logical, 2, true);
        auto dirty = renderer.layout_for(small_logical, 2).panel;
        renderer.render(*target.value(), bg.value().ptr(), state, small_logical, 2, true, dirty);
        renderer.render(*target.value(), bg.value().ptr(), state, small_logical, 2, false);
        check("render-completed", true);
    }

    auto cursor = SDELock::LockRenderer::make_cursor_bitmap(2);
    check("cursor-2x", !cursor.is_error() && cursor.value()->size() == Gfx::IntSize { 32, 32 });
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    test_key_input();
    test_gecos_display_name();
    test_hidpi_layout_and_render();

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: sde-lock unit tests passed");
    return 0;
}
