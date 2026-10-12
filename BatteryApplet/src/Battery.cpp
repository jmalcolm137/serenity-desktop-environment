/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Battery.h"

#include <AK/ByteString.h>
#include <AK/IterationDecision.h>
#include <AK/StdLibExtras.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>

namespace Battery {

static ErrorOr<Optional<ByteString>> read_value(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    // An absent attribute (for example `current_now` on some batteries) is a
    // normal condition, not an error.
    if (file.is_error())
        return Optional<ByteString> {};

    auto contents = TRY(file.value()->read_until_eof());
    auto value = StringView { contents }.trim_whitespace();
    if (value.is_empty())
        return Optional<ByteString> {};
    return value.to_byte_string();
}

static ByteString attribute_path(ByteString const& directory, StringView attribute)
{
    return ByteString::formatted("{}/{}", directory, attribute);
}

static ErrorOr<Optional<i64>> read_number(ByteString const& directory, StringView attribute)
{
    auto value = TRY(read_value(attribute_path(directory, attribute)));
    if (!value.has_value())
        return Optional<i64> {};
    return value->to_number<i64>();
}

static State parse_state(StringView status)
{
    if (status == "Charging"sv)
        return State::Charging;
    if (status == "Discharging"sv)
        return State::Discharging;
    if (status == "Full"sv)
        return State::Full;
    if (status == "Not charging"sv)
        return State::NotCharging;
    return State::Unknown;
}

static Optional<int> convert_to_seconds(i64 amount, i64 rate)
{
    if (rate <= 0 || amount <= 0)
        return {};
    return static_cast<int>((amount * 3600) / rate);
}

// The kernel exposes either charge (uAh) + current (uA) or energy (uWh) +
// power (uW); try charge first, then energy.
static ErrorOr<Optional<int>> estimate_seconds_remaining(ByteString const& directory, State state)
{
    if (state != State::Charging && state != State::Discharging)
        return Optional<int> {};

    auto current_now = TRY(read_number(directory, "current_now"sv));
    if (current_now.has_value()) {
        auto charge_now = TRY(read_number(directory, "charge_now"sv));
        auto charge_full = TRY(read_number(directory, "charge_full"sv));
        if (state == State::Discharging && charge_now.has_value())
            return convert_to_seconds(charge_now.value(), current_now.value());
        if (state == State::Charging && charge_now.has_value() && charge_full.has_value())
            return convert_to_seconds(charge_full.value() - charge_now.value(), current_now.value());
    }

    auto power_now = TRY(read_number(directory, "power_now"sv));
    if (power_now.has_value()) {
        auto energy_now = TRY(read_number(directory, "energy_now"sv));
        auto energy_full = TRY(read_number(directory, "energy_full"sv));
        if (state == State::Discharging && energy_now.has_value())
            return convert_to_seconds(energy_now.value(), power_now.value());
        if (state == State::Charging && energy_now.has_value() && energy_full.has_value())
            return convert_to_seconds(energy_full.value() - energy_now.value(), power_now.value());
    }

    return Optional<int> {};
}

static ErrorOr<Optional<Status>> read_one(ByteString const& directory, ByteString const& name)
{
    auto type = TRY(read_value(attribute_path(directory, "type"sv)));
    // Only batteries and UPSes are of interest; mains adapters and USB ports
    // also live here.
    if (!type.has_value() || (type.value() != "Battery"sv && type.value() != "UPS"sv))
        return Optional<Status> {};

    auto present = TRY(read_number(directory, "present"sv));
    if (present.has_value() && present.value() == 0)
        return Optional<Status> {};

    Status status;
    status.name = name;
    status.model = TRY(read_value(attribute_path(directory, "model_name"sv))).value_or(ByteString {});
    status.state = parse_state(TRY(read_value(attribute_path(directory, "status"sv))).value_or(ByteString {}));
    status.percentage = clamp(static_cast<int>(TRY(read_number(directory, "capacity"sv)).value_or(0)), 0, 100);
    status.present = true;
    status.seconds_remaining = TRY(estimate_seconds_remaining(directory, status.state));
    return status;
}

ErrorOr<Vector<Status>> read_all(StringView root)
{
    Vector<Status> batteries;
    TRY(Core::Directory::for_each_entry(root, Core::DirIterator::SkipDots, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        auto directory = ByteString::formatted("{}/{}", root, entry.name);
        if (auto status = TRY(read_one(directory, entry.name)); status.has_value())
            batteries.append(status.release_value());
        return IterationDecision::Continue;
    }));
    return batteries;
}

Summary summarize(Vector<Status> const& batteries)
{
    Summary summary;
    int percentage_total = 0;

    for (auto const& battery : batteries) {
        if (!battery.present)
            continue;

        summary.has_battery = true;
        summary.battery_count++;
        percentage_total += battery.percentage;

        // A charging battery wins over a discharging one.
        if (battery.state == State::Charging)
            summary.state = State::Charging;
        else if (battery.state == State::Discharging && summary.state != State::Charging)
            summary.state = State::Discharging;
        else if (battery.state == State::Full && summary.state == State::Unknown)
            summary.state = State::Full;
        else if (battery.state == State::NotCharging && summary.state == State::Unknown)
            summary.state = State::NotCharging;

        if (battery.seconds_remaining.has_value() && (!summary.seconds_remaining.has_value() || battery.seconds_remaining.value() < summary.seconds_remaining.value()))
            summary.seconds_remaining = battery.seconds_remaining;
    }

    if (summary.battery_count > 0)
        summary.percentage = (percentage_total + static_cast<int>(summary.battery_count) / 2) / static_cast<int>(summary.battery_count);

    return summary;
}

StringView state_name(State state)
{
    switch (state) {
    case State::Charging:
        return "Charging"sv;
    case State::Discharging:
        return "Discharging"sv;
    case State::Full:
        return "Fully charged"sv;
    case State::NotCharging:
        return "Plugged in, not charging"sv;
    case State::Unknown:
        return "Unknown"sv;
    }
    VERIFY_NOT_REACHED();
}

ByteString format_duration(Optional<int> seconds)
{
    if (!seconds.has_value())
        return "Unknown";
    auto total = max(0, seconds.value());
    return ByteString::formatted("{}:{:02}", total / 3600, (total % 3600) / 60);
}

ByteString format_time_remaining(State state, Optional<int> seconds)
{
    if (!seconds.has_value())
        return ByteString::formatted("{}", state_name(state));
    switch (state) {
    case State::Charging:
        return ByteString::formatted("{} until full", format_duration(seconds));
    case State::Discharging:
        return ByteString::formatted("{} remaining", format_duration(seconds));
    default:
        return ByteString::formatted("{}", state_name(state));
    }
}

}
