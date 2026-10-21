/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConfigRules.h"

#include <AK/Math.h>
#include <AK/StringUtils.h>
#include <xkbcommon/xkbcommon.h>

namespace ConfigRules {

using Config::ModAlt;
using Config::ModCtrl;
using Config::ModShift;
using Config::ModSuper;

bool parse_key_combo(StringView combo, u32& keysym, u32& modifiers)
{
    keysym = 0;
    modifiers = 0;
    bool saw_key = false;
    for (auto part : combo.split_view('+')) {
        auto token = part.trim_whitespace();
        if (token.is_empty())
            continue;
        if (token == "Ctrl"sv)
            modifiers |= ModCtrl;
        else if (token == "Alt"sv)
            modifiers |= ModAlt;
        else if (token == "Shift"sv)
            modifiers |= ModShift;
        else if (token == "Super"sv || token == "Logo"sv || token == "Meta"sv)
            modifiers |= ModSuper;
        else {
            if (saw_key)
                return false;
            auto key_name = token.to_byte_string();
            keysym = xkb_keysym_from_name(key_name.characters(), XKB_KEYSYM_CASE_INSENSITIVE);
            if (keysym == XKB_KEY_NoSymbol)
                return false;
            saw_key = true;
        }
    }
    return saw_key && modifiers != 0;
}

Optional<double> parse_output_scale(StringView text)
{
    auto trimmed = text.trim_whitespace();
    if (trimmed.is_empty())
        return {};
    // strtod would accept "2x" (and " 2"), so require the number to be the whole
    // value before range-checking it.
    char* end = nullptr;
    auto text_bytes = trimmed.to_byte_string();
    double const value = strtod(text_bytes.characters(), &end);
    if (end == text_bytes.characters() || *end != '\0')
        return {};
    if (value < 0.0 || (value != 0.0 && (value < 0.5 || value > 4.0)))
        return {};
    return value;
}

Optional<bool> parse_bool(StringView text)
{
    auto lower = text.trim_whitespace().to_lowercase_string();
    if (lower.is_empty())
        return {};
    if (lower == "true"sv || lower == "1"sv || lower == "yes"sv || lower == "on"sv)
        return true;
    if (lower == "false"sv || lower == "0"sv || lower == "no"sv || lower == "off"sv)
        return false;
    return {};
}

double clamp_acceleration(double value, Config::InputRanges const& ranges)
{
    if (ranges.accel_min > ranges.accel_max)
        return value; // A backwards range is ignored, not enforced.
    return clamp(value, ranges.accel_min, ranges.accel_max);
}

int clamp_scroll_step(int value, Config::InputRanges const& ranges)
{
    return max(value, ranges.scroll_step_min);
}

int clamp_double_click_speed(int value, Config::InputRanges const& ranges)
{
    if (ranges.double_click_min > ranges.double_click_max)
        return value;
    return clamp(value, ranges.double_click_min, ranges.double_click_max);
}

}
