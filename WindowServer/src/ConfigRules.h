/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Config.h" // the Modifier bits, Config::InputRanges

#include <AK/Optional.h>
#include <AK/StringView.h>

// The pure validation rules behind WindowServer.ini: what a keybinding combo
// means, which output scales are legal, and what counts as a boolean. They take
// a string and answer with a value or "no" — no config file, no diagnostics
// list, no compositor — so the answers can be unit-tested
// (test/ConfigRulesTest.cpp) and callers decide what to do about a rejection.
//
// The string forms are the user-facing contract (docs/14-r4.4-config.md), which
// is exactly why they should not be verified only by starting a session.
namespace ConfigRules {

// "Ctrl+Alt+Left" -> keysym XKB_KEY_Left and ModCtrl|ModAlt. Returns false unless
// the combo names at least one modifier and exactly one key. The key name is
// resolved case-insensitively by xkbcommon, so "left", "Left" and "LEFT" agree.
bool parse_key_combo(StringView combo, u32& keysym, u32& modifiers);

// An output scale: 0 means automatic (an integer HiDPI scale), and otherwise
// 0.5..4.0. Anything else (negative, 0.4, 4.1, "abc", "2x") is rejected.
Optional<double> parse_output_scale(StringView text);

// A boolean as the config file spells it: true/1/yes/on and false/0/no/off,
// case-insensitively. Anything else is rejected.
Optional<bool> parse_bool(StringView text);

// Clamp an input setting to the ranges the [Input] group documents, so a
// hand-edited input.ini cannot put the pointer in a state the settings UI cannot
// get it out of. A range whose bounds are the wrong way round is left alone (the
// default), which is what the loader does.
double clamp_acceleration(double value, Config::InputRanges const& ranges);
int clamp_scroll_step(int value, Config::InputRanges const& ranges);
int clamp_double_click_speed(int value, Config::InputRanges const& ranges);

}
