/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

// Reads the Linux power-supply class (sysfs) directly, so the applet needs no
// UPower or D-Bus dependency. The SerenityOS build has no such interface; this
// is the SDE's host integration for the battery applet.
namespace Battery {

enum class State {
    Unknown,
    Charging,
    Discharging,
    Full,
    NotCharging,
};

struct Status {
    ByteString name;
    ByteString model;
    State state { State::Unknown };
    int percentage { 0 };
    bool present { false };
    Optional<int> seconds_remaining;
};

// A summary of every battery, for the compact (tray) representation.
struct Summary {
    bool has_battery { false };
    size_t battery_count { 0 };
    int percentage { 0 };
    State state { State::Unknown };
    Optional<int> seconds_remaining;
};

// Reads every battery under `root` (default: /sys/class/power_supply). The root
// is overridable so the reader can be pointed at a fixture directory in tests.
ErrorOr<Vector<Status>> read_all(StringView root = "/sys/class/power_supply"sv);

Summary summarize(Vector<Status> const&);

StringView state_name(State);

// Formats a duration as "h:mm" (for example "2:14"), or "Unknown" when absent.
ByteString format_duration(Optional<int> seconds);

// Formats the time after which a battery is empty (discharging) or full
// (charging), in a form suitable for the tooltip and the detail popup.
ByteString format_time_remaining(State, Optional<int> seconds);

}
