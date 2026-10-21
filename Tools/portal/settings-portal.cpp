/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-settings-portal — the SDE backend for the freedesktop Settings portal
// (R4.4). It implements org.freedesktop.impl.portal.Settings (the interface
// xdg-desktop-portal calls) and answers the appearance subset — color-scheme,
// accent-color, contrast — from the active SDE theme, so GTK/Qt/Flatpak apps
// follow the Serenity theme.
//
// It reads the theme broker file ($XDG_RUNTIME_DIR/sde-theme, written by the
// session) and re-reads it on change, emitting SettingChanged. It is an
// ordinary Serenity-style service: AK types over the `dbus-1` transport (see
// dbus.h), a LibCore event loop, and no GLib/GIO.

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/StringUtils.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibMain/Main.h>

#include "dbus.h"
#include "introspection.h"

namespace {

constexpr StringView bus_name = "org.freedesktop.impl.portal.desktop.sde"sv;
constexpr StringView object_path = "/org/freedesktop/portal/desktop"sv;
constexpr StringView settings_interface = "org.freedesktop.impl.portal.Settings"sv;
constexpr StringView appearance_namespace = "org.freedesktop.appearance"sv;

// What the theme says the appearance should be. Defaults are used when there is
// no readable theme: a light scheme with the classic blue accent.
struct Appearance {
    u32 color_scheme { 2 }; // 1 = prefer-dark, 2 = prefer-light
    double accent_r { 0.29 };
    double accent_g { 0.43 };
    double accent_b { 0.67 };
};

ByteString runtime_dir()
{
    if (auto value = Core::Environment::get("XDG_RUNTIME_DIR"sv); value.has_value())
        return value->to_byte_string();
    return ByteString { "/tmp" };
}

ByteString broker_path()
{
    return ByteString::formatted("{}/sde-theme", runtime_dir());
}

Optional<ByteString> read_first_line(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    auto bytes = contents.release_value();
    StringView text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    size_t end = 0;
    while (end < text.length() && text[end] != '\n')
        ++end;
    auto line = text.substring_view(0, end).trim_whitespace();
    if (line.is_empty())
        return {};
    return line.to_byte_string();
}

Optional<Array<double, 3>> parse_color(StringView value)
{
    if (!value.starts_with('#'))
        return {};
    if (value.length() < 7)
        return {};
    auto packed = AK::StringUtils::convert_to_uint_from_hex(value.substring_view(1, 6));
    if (!packed.has_value())
        return {};
    Array<double, 3> color {
        ((packed.value() >> 16) & 0xff) / 255.0,
        ((packed.value() >> 8) & 0xff) / 255.0,
        (packed.value() & 0xff) / 255.0,
    };
    return color;
}

Appearance read_appearance()
{
    Appearance appearance;
    auto theme_path = read_first_line(broker_path());
    if (!theme_path.has_value())
        return appearance;

    auto config = Core::ConfigFile::open(theme_path.value());
    if (config.is_error())
        return appearance;

    if (auto window = parse_color(config.value()->read_entry("Colors"sv, "Window"sv)); window.has_value()) {
        auto const& color = window.value();
        double luminance = 0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2];
        appearance.color_scheme = luminance < 0.5 ? 1 : 2;
    }
    if (auto accent = parse_color(config.value()->read_entry("Colors"sv, "Accent"sv)); accent.has_value()) {
        appearance.accent_r = accent.value()[0];
        appearance.accent_g = accent.value()[1];
        appearance.accent_b = accent.value()[2];
    } else if (auto selection = parse_color(config.value()->read_entry("Colors"sv, "Selection"sv)); selection.has_value()) {
        appearance.accent_r = selection.value()[0];
        appearance.accent_g = selection.value()[1];
        appearance.accent_b = selection.value()[2];
    }
    return appearance;
}

Optional<Portal::Value> appearance_value(StringView key, Appearance const& appearance)
{
    if (key == "color-scheme"sv)
        return Portal::Value::make_uint32(appearance.color_scheme);
    if (key == "contrast"sv)
        return Portal::Value::make_uint32(0);
    if (key == "accent-color"sv)
        return Portal::Value::make_struct({ Portal::Value::make_double(appearance.accent_r), Portal::Value::make_double(appearance.accent_g), Portal::Value::make_double(appearance.accent_b) });
    return {};
}

void emit_setting_changed(Portal::Service& service)
{
    auto appearance = read_appearance();
    for (auto key : { "color-scheme"sv, "accent-color"sv, "contrast"sv }) {
        auto value = appearance_value(key, appearance);
        if (!value.has_value())
            continue;
        service.emit_signal(settings_interface, "SettingChanged"sv,
            { Portal::Value::make_string(appearance_namespace), Portal::Value::make_string(key), Portal::Value::make_variant(value.release_value()) });
    }
}

void handle_settings(Portal::MethodCall& call)
{
    auto appearance = read_appearance();

    if (call.member() == "Read"sv) {
        auto namespace_name = call.string_argument(0);
        auto key = call.string_argument(1);
        if (namespace_name != appearance_namespace) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "unknown namespace"sv);
            return;
        }
        if (auto value = appearance_value(key, appearance); value.has_value()) {
            call.reply({ Portal::Value::make_variant(value.release_value()) });
            return;
        }
        call.reply_error(Portal::ErrorName::InvalidArgs, "unknown key"sv);
        return;
    }

    if (call.member() == "ReadAll"sv) {
        auto namespace_name = call.string_argument(0);
        Portal::Dictionary results;
        if (namespace_name == appearance_namespace) {
            for (auto key : { "color-scheme"sv, "accent-color"sv, "contrast"sv }) {
                auto value = appearance_value(key, appearance);
                if (value.has_value())
                    results.append(Portal::Value::Entry { key, Portal::Value::make_variant(value.release_value()) });
            }
        }
        call.reply({ Portal::Value::make_dict(move(results)) });
        return;
    }

    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto service = TRY(Portal::Service::create(bus_name.to_byte_string(), object_path.to_byte_string()));
    service->register_interface(
        settings_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_settings(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(2);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(settings_interface).value_or(StringView {}));

    // Re-read the theme broker when the session switches themes.
    auto watcher = TRY(Core::FileWatcher::create());
    watcher->on_change = [service = service.ptr()](Core::FileWatcherEvent const&) {
        emit_setting_changed(*service);
    };
    auto broker = broker_path();
    auto watch_result = watcher->add_watch(broker, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
    if (watch_result.is_error())
        dbgln("sde-settings-portal: theme watch failed for {}: {}", broker, watch_result.error());

    dbgln("sde-settings-portal: serving {} on {}", object_path, bus_name);
    return event_loop.exec();
}
