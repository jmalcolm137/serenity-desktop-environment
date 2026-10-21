/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Unit tests for the config validation rules (ConfigRules.h): what a keybinding
// combo means, which output scales and booleans are legal, and how input settings
// are clamped to their documented ranges.
//
// WindowServer.ini is a user-facing contract (docs/14-r4.4-config.md), and a rule
// that is only checked by starting a session is a rule that gets it wrong
// quietly. These are the rules, with no config file and no compositor: the loader
// calls exactly these functions and turns a rejection into a diagnostic.

#include <AK/Format.h>
#include <LibMain/Main.h>
#include <xkbcommon/xkbcommon.h>

#include "ConfigRules.h"

using namespace ConfigRules;
using Config::ModAlt;
using Config::ModCtrl;
using Config::ModShift;
using Config::ModSuper;

namespace {

int g_failures = 0;

void expect(bool condition, char const* what)
{
    if (condition) {
        outln("  ok: {}", what);
    } else {
        outln("  FAIL: {}", what);
        ++g_failures;
    }
}

// A combo that parses: check the keysym and modifier bits it produced.
void expect_combo(StringView combo, xkb_keysym_t keysym, u32 modifiers)
{
    u32 got_keysym = 0;
    u32 got_modifiers = 0;
    bool const parsed = parse_key_combo(combo, got_keysym, got_modifiers);
    if (parsed && got_keysym == static_cast<u32>(keysym) && got_modifiers == modifiers) {
        outln("  ok: '{}' -> keysym {} mods {}", combo, keysym, modifiers);
    } else {
        outln("  FAIL: '{}' -> parsed={} keysym {} (want {}) mods {} (want {})", combo, parsed,
            got_keysym, static_cast<u32>(keysym), got_modifiers, modifiers);
        ++g_failures;
    }
}

// A combo that must be rejected.
void expect_bad_combo(StringView combo)
{
    u32 keysym = 0;
    u32 modifiers = 0;
    if (!parse_key_combo(combo, keysym, modifiers)) {
        outln("  ok: '{}' is rejected", combo);
    } else {
        outln("  FAIL: '{}' was accepted (keysym {} mods {})", combo, keysym, modifiers);
        ++g_failures;
    }
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    outln("=== keybinding combos ===");
    expect_combo("Ctrl+Alt+Left"sv, XKB_KEY_Left, ModCtrl | ModAlt);
    expect_combo("Super+L"sv, XKB_KEY_l, ModSuper);
    expect_combo("Shift+Ctrl+Tab"sv, XKB_KEY_Tab, ModShift | ModCtrl);
    // The key name is resolved case-insensitively, so the spelling in the file
    // does not have to match the keysym table.
    expect_combo("Ctrl+Alt+LEFT"sv, XKB_KEY_Left, ModCtrl | ModAlt);
    expect_combo("Ctrl+Alt+lEfT"sv, XKB_KEY_Left, ModCtrl | ModAlt);
    // Super has three spellings.
    expect_combo("Logo+Left"sv, XKB_KEY_Left, ModSuper);
    expect_combo("Meta+Left"sv, XKB_KEY_Left, ModSuper);
    // Whitespace around the parts is trimmed.
    expect_combo(" Ctrl + Alt + Left "sv, XKB_KEY_Left, ModCtrl | ModAlt);
    // A digit key and an arrow both resolve.
    expect_combo("Super+1"sv, XKB_KEY_1, ModSuper);

    // The modifier names are spelled exactly as documented ("Ctrl", "Alt",
    // "Shift", "Super"), while the key name is case-insensitive. A token that is
    // not a spelled modifier is taken as a key name, so a miscased modifier does
    // not silently change what the binding does: it is rejected (two keys).
    expect_bad_combo("ctrl+Left"sv);
    expect_bad_combo("CTRL+Left"sv);
    expect_bad_combo("ctrl+Alt+Left"sv);
    expect_bad_combo("ctrl"sv);

    // Rejections: no modifier, no key, two keys, an unknown key, empty.
    expect_bad_combo("Left"sv);
    expect_bad_combo("Ctrl"sv);
    expect_bad_combo("Ctrl+Alt"sv);
    expect_bad_combo("Ctrl+Left+Right"sv);
    expect_bad_combo("Ctrl+NotAKey"sv);
    expect_bad_combo(""sv);
    expect_bad_combo("+"sv);

    outln("=== output scales ===");
    expect(parse_output_scale("0"sv) == Optional<double>(0.0), "'0' is legal (automatic)");
    expect(parse_output_scale("1"sv) == Optional<double>(1.0), "'1' is legal");
    expect(parse_output_scale("2"sv) == Optional<double>(2.0), "'2' is legal");
    expect(parse_output_scale("1.5"sv) == Optional<double>(1.5), "'1.5' is legal (fractional)");
    expect(parse_output_scale("0.5"sv) == Optional<double>(0.5), "'0.5' is the low end");
    expect(parse_output_scale("4"sv) == Optional<double>(4.0), "'4' is the high end");
    expect(parse_output_scale(" 2 "sv) == Optional<double>(2.0), "surrounding whitespace is trimmed");

    expect(!parse_output_scale("0.4"sv).has_value(), "'0.4' is below the range");
    expect(!parse_output_scale("4.1"sv).has_value(), "'4.1' is above the range");
    expect(!parse_output_scale("-1"sv).has_value(), "a negative scale is rejected");
    expect(!parse_output_scale("abc"sv).has_value(), "a non-number is rejected");
    // strtod would read "2x" and "2x" as 2; the whole value must be the number.
    expect(!parse_output_scale("2x"sv).has_value(), "'2x' is rejected, not read as 2");
    expect(!parse_output_scale("1 2"sv).has_value(), "'1 2' is rejected");
    expect(!parse_output_scale(""sv).has_value(), "an empty value is rejected");

    outln("=== booleans ===");
    expect(parse_bool("true"sv) == Optional<bool>(true), "'true' is true");
    expect(parse_bool("1"sv) == Optional<bool>(true), "'1' is true");
    expect(parse_bool("yes"sv) == Optional<bool>(true), "'yes' is true");
    expect(parse_bool("on"sv) == Optional<bool>(true), "'on' is true");
    expect(parse_bool("false"sv) == Optional<bool>(false), "'false' is false");
    expect(parse_bool("0"sv) == Optional<bool>(false), "'0' is false");
    expect(parse_bool("no"sv) == Optional<bool>(false), "'no' is false");
    expect(parse_bool("off"sv) == Optional<bool>(false), "'off' is false");
    expect(parse_bool("TRUE"sv) == Optional<bool>(true), "case does not matter");
    expect(parse_bool(" On "sv) == Optional<bool>(true), "surrounding whitespace is trimmed");
    expect(!parse_bool("maybe"sv).has_value(), "an unknown word is rejected");
    expect(!parse_bool(""sv).has_value(), "an empty value is rejected");
    expect(!parse_bool("2"sv).has_value(), "'2' is rejected");

    outln("=== input ranges ===");
    Config::InputRanges const defaults;
    expect(clamp_acceleration(1.0, defaults) == 1.0, "an in-range acceleration is kept");
    expect(clamp_acceleration(0.1, defaults) == defaults.accel_min, "a too-low acceleration is raised to the minimum");
    expect(clamp_acceleration(9.0, defaults) == defaults.accel_max, "a too-high acceleration is lowered to the maximum");
    expect(clamp_scroll_step(0, defaults) == defaults.scroll_step_min, "a zero scroll step is raised to the minimum");
    expect(clamp_scroll_step(7, defaults) == 7, "a valid scroll step is kept (no upper bound)");
    expect(clamp_double_click_speed(50, defaults) == defaults.double_click_min, "a too-fast double-click is slowed to the minimum");
    expect(clamp_double_click_speed(5000, defaults) == defaults.double_click_max, "a too-slow double-click is sped up to the maximum");

    // A range written the wrong way round in the file is ignored, not enforced:
    // clamping to it would force every value to the same (bogus) bound.
    Config::InputRanges const backwards { 3.5, 0.5, 1, 900, 100 };
    expect(clamp_acceleration(1.0, backwards) == 1.0, "a backwards acceleration range leaves the value alone");
    expect(clamp_double_click_speed(250, backwards) == 250, "a backwards double-click range leaves the value alone");

    if (g_failures != 0) {
        outln("==> config rules test FAILED ({} check(s))", g_failures);
        return 1;
    }
    outln("==> config rules test PASSED");
    return 0;
}
