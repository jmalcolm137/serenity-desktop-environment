/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// xdg-desktop-portal-serenity — the SDE backend for the freedesktop portals
// (R5). It implements org.freedesktop.impl.portal.* on
// /org/freedesktop/portal/desktop and owns
// org.freedesktop.impl.portal.desktop.serenity, so the xdg-desktop-portal
// frontend can route requests to it (see contrib/xdg-desktop-portal/portals.conf
// and the .portal file).
//
// It is a portable host D-Bus service (standard protocols only, AK over the
// dbus-1 transport — see dbus.h): the Serenity-specific bits are reached through
// the session launcher/notifier, with portable fallbacks, so it also runs under
// other compositors.
//
// The interfaces with their own capture/input machinery live in their own
// modules: ScreenCast (screencast.cpp), RemoteDesktop (remote-desktop.cpp),
// InputCapture (input-capture.cpp), GlobalShortcuts (global-shortcuts.cpp).

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibMain/Main.h>

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#include "dbus.h"
#include "global-shortcuts.h"
#include "introspection.h"
#include "input-capture.h"
#include "remote-desktop.h"
#include "screencast.h"

namespace {

constexpr StringView bus_name = "org.freedesktop.impl.portal.desktop.serenity"sv;
constexpr StringView object_path = "/org/freedesktop/portal/desktop"sv;
constexpr StringView appearance_namespace = "org.freedesktop.appearance"sv;

constexpr StringView settings_interface = "org.freedesktop.impl.portal.Settings"sv;
constexpr StringView account_interface = "org.freedesktop.impl.portal.Account"sv;
constexpr StringView lockdown_interface = "org.freedesktop.impl.portal.Lockdown"sv;
constexpr StringView background_interface = "org.freedesktop.impl.portal.Background"sv;
constexpr StringView appchooser_interface = "org.freedesktop.impl.portal.AppChooser"sv;
constexpr StringView email_interface = "org.freedesktop.impl.portal.Email"sv;
constexpr StringView location_interface = "org.freedesktop.impl.portal.Location"sv;
constexpr StringView proxyresolver_interface = "org.freedesktop.impl.portal.ProxyResolver"sv;
constexpr StringView openuri_interface = "org.freedesktop.impl.portal.OpenURI"sv;
constexpr StringView notification_interface = "org.freedesktop.impl.portal.Notification"sv;
constexpr StringView realtime_interface = "org.freedesktop.impl.portal.Realtime"sv;
constexpr StringView powerprofile_interface = "org.freedesktop.impl.portal.PowerProfileMonitor"sv;
constexpr StringView gamemode_interface = "org.freedesktop.impl.portal.GameMode"sv;
constexpr StringView memorymonitor_interface = "org.freedesktop.impl.portal.MemoryMonitor"sv;
constexpr StringView filechooser_interface = "org.freedesktop.impl.portal.FileChooser"sv;
constexpr StringView screenshot_interface = "org.freedesktop.impl.portal.Screenshot"sv;
constexpr StringView print_interface = "org.freedesktop.impl.portal.Print"sv;
constexpr StringView wallpaper_interface = "org.freedesktop.impl.portal.Wallpaper"sv;
constexpr StringView secret_interface = "org.freedesktop.impl.portal.Secret"sv;
constexpr StringView usb_interface = "org.freedesktop.impl.portal.USB"sv;
constexpr StringView dynamiclauncher_interface = "org.freedesktop.impl.portal.DynamicLauncher"sv;
constexpr StringView access_interface = "org.freedesktop.impl.portal.Access"sv;

constexpr u32 response_success = to_underlying(Portal::Response::Success);
constexpr u32 response_cancelled = to_underlying(Portal::Response::Cancelled);
constexpr u32 response_other_error = to_underlying(Portal::Response::OtherError);

// --- environment & state ----------------------------------------------------

// Registers a portal interface and, alongside it, its introspection XML so the
// service can answer org.freedesktop.DBus.Introspectable.
void register_portal_interface(Portal::Service& service, StringView name, Portal::Service::MethodHandler method,
    Portal::Service::PropertyHandler property = {}, Vector<ByteString> property_names = {})
{
    service.register_interface(name.to_byte_string(), move(method), move(property), move(property_names),
        Portal::interface_introspection_xml(name).value_or(StringView {}));
}

Optional<ByteString> environment(StringView name)
{
    if (auto value = Core::Environment::get(name); value.has_value())
        return value->to_byte_string();
    return {};
}

ByteString runtime_dir()
{
    if (auto value = environment("XDG_RUNTIME_DIR"sv); value.has_value() && !value->is_empty())
        return value.value();
    return ByteString { "/tmp" };
}

// The place the portal keeps its state (wallpaper, secrets, tokens): the
// session's SDE_STATE_DIR, else $XDG_CONFIG_HOME/sde, else $HOME/.config/sde.
ByteString portal_state_file(StringView name)
{
    ByteString base;
    if (auto state = environment("SDE_STATE_DIR"sv); state.has_value() && !state->is_empty())
        base = state.value();
    else if (auto config = environment("XDG_CONFIG_HOME"sv); config.has_value() && !config->is_empty())
        base = ByteString::formatted("{}/sde", config.value());
    else if (auto home = environment("HOME"sv); home.has_value() && !home->is_empty())
        base = ByteString::formatted("{}/.config/sde", home.value());
    else
        base = ByteString { "/tmp/sde" };

    (void)Core::System::mkdir(base, 0700);
    return ByteString::formatted("{}/{}", base, name);
}

// --- process helpers --------------------------------------------------------

ErrorOr<Optional<ByteString>> run_command(Vector<ByteString> const& arguments, bool capture_stdout)
{
    Core::ProcessSpawnOptions options {
        .name = arguments.first(),
        .executable = arguments.first(),
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };

    int read_fd = -1;
    int write_fd = -1;
    if (capture_stdout) {
        auto pipe = TRY(Core::System::pipe2(O_CLOEXEC));
        read_fd = pipe[0];
        write_fd = pipe[1];
        options.file_actions.append(Core::FileAction::DuplicateFile { write_fd, STDOUT_FILENO });
        options.file_actions.append(Core::FileAction::CloseFile { read_fd });
    }

    auto process = TRY(Core::Process::spawn(options));
    if (write_fd >= 0)
        ::close(write_fd);

    Optional<ByteString> output;
    if (read_fd >= 0) {
        auto file = TRY(Core::File::adopt_fd(read_fd, Core::File::OpenMode::Read));
        auto contents = TRY(file->read_until_eof());
        StringView text { reinterpret_cast<char const*>(contents.data()), contents.size() };
        output = text.trim_whitespace().to_byte_string();
    }

    (void)process.wait_for_termination();
    return output;
}

void spawn_async(Vector<ByteString> const& arguments)
{
    Core::ProcessSpawnOptions options {
        .name = arguments.first(),
        .executable = arguments.first(),
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };
    (void)Core::Process::spawn(options);
}

// Feed the contents of `fd` to the stdin of `/bin/sh -c command`. Returns false
// if the child could not be started.
bool feed_fd_to_shell(int fd, ByteString const& command)
{
    auto pipe = Core::System::pipe2(O_CLOEXEC);
    if (pipe.is_error())
        return false;
    int read_fd = pipe.value()[0];
    int write_fd = pipe.value()[1];

    Vector<ByteString> arguments { "/bin/sh", "-c", command };
    Core::ProcessSpawnOptions options {
        .name = "/bin/sh"sv,
        .executable = "/bin/sh"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };
    options.file_actions.append(Core::FileAction::DuplicateFile { read_fd, STDIN_FILENO });
    options.file_actions.append(Core::FileAction::CloseFile { write_fd });

    auto process = Core::Process::spawn(options);
    ::close(read_fd);
    if (process.is_error()) {
        ::close(write_fd);
        return false;
    }

    lseek(fd, 0, SEEK_SET);
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = ::write(write_fd, buffer + offset, static_cast<size_t>(n - offset));
            if (written <= 0)
                break;
            offset += written;
        }
    }
    ::close(write_fd);
    (void)process.value().wait_for_termination();
    return true;
}

// --- URI helpers ------------------------------------------------------------

ByteString filename_to_uri(ByteString const& path)
{
    StringBuilder builder;
    builder.append("file://"sv);
    for (unsigned char character : path.bytes()) {
        if (isalnum(character) || character == '/' || character == '-' || character == '_' || character == '.' || character == '~')
            builder.append(static_cast<char>(character));
        else
            builder.appendff("%{:02X}", character);
    }
    return builder.to_byte_string();
}

int hex_digit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

Optional<ByteString> filename_from_uri(StringView uri)
{
    if (!uri.starts_with("file://"sv))
        return {};
    auto encoded = uri.substring_view("file://"sv.length());
    ByteBuffer decoded;
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            int high = hex_digit(encoded[i + 1]);
            int low = hex_digit(encoded[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.append(static_cast<u8>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.append(static_cast<u8>(encoded[i]));
    }
    return ByteString { StringView { reinterpret_cast<char const*>(decoded.data()), decoded.size() } };
}

// Launch a URI. Prefer SDE_PORTAL_OPENER (the session launcher) so SDE handles
// it; otherwise fall back to xdg-open, the portable default-app launcher.
bool launch_uri(ByteString const& uri)
{
    if (auto opener = environment("SDE_PORTAL_OPENER"sv); opener.has_value() && !opener->is_empty()) {
        spawn_async({ opener.value(), uri });
        return true;
    }
    auto result = run_command({ "xdg-open", uri }, false);
    return !result.is_error();
}

// --- Settings ---------------------------------------------------------------

struct Appearance {
    u32 color_scheme { 2 };
    double accent_r { 0.29 };
    double accent_g { 0.43 };
    double accent_b { 0.67 };
};

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
    auto broker = ByteString::formatted("{}/sde-theme", runtime_dir());
    auto file = Core::File::open(broker, Core::File::OpenMode::Read);
    if (file.is_error())
        return appearance;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return appearance;
    auto bytes = contents.release_value();
    StringView text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    auto theme_path = text.trim_whitespace().to_byte_string();

    auto config = Core::ConfigFile::open(theme_path);
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

void handle_settings(Portal::MethodCall& call)
{
    auto appearance = read_appearance();
    if (call.member() == "Read"sv) {
        if (call.string_argument(0) != appearance_namespace) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "unknown namespace"sv);
            return;
        }
        if (auto value = appearance_value(call.string_argument(1), appearance); value.has_value()) {
            call.reply({ Portal::Value::make_variant(value.release_value()) });
            return;
        }
        call.reply_error(Portal::ErrorName::InvalidArgs, "unknown key"sv);
        return;
    }
    if (call.member() == "ReadAll"sv) {
        Portal::Dictionary results;
        if (call.string_argument(0) == appearance_namespace) {
            for (auto key : { "color-scheme"sv, "accent-color"sv, "contrast"sv }) {
                if (auto value = appearance_value(key, appearance); value.has_value())
                    results.append(Portal::Value::Entry { key, value.release_value() });
            }
        }
        call.reply({ Portal::Value::make_dict(move(results)) });
        return;
    }
    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

// --- Account ----------------------------------------------------------------

void handle_account(Portal::MethodCall& call)
{
    if (call.member() != "GetUserInformation"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    struct passwd* password = getpwuid(getuid());
    ByteString name = (password && password->pw_gecos && password->pw_gecos[0]) ? ByteString { password->pw_gecos } : ByteString { password ? password->pw_name : "User" };
    ByteString id = (password && password->pw_name) ? ByteString { password->pw_name } : ByteString { "user" };
    if (auto comma = name.find(','); comma.has_value())
        name = name.substring(0, comma.value());

    Portal::Dictionary results;
    results.append(Portal::Value::Entry { "id", Portal::Value::make_string(id) });
    results.append(Portal::Value::Entry { "name", Portal::Value::make_string(name) });
    results.append(Portal::Value::Entry { "image", Portal::Value::make_string("") });
    call.reply_response(response_success, results);
}

// --- Lockdown ---------------------------------------------------------------

Portal::Value bool_setting(StringView, StringView env)
{
    bool value = false;
    if (auto text = environment(env); text.has_value() && !text->is_empty())
        value = (*text == "1"sv || text->equals_ignoring_ascii_case("true"sv));
    return Portal::Value::make_bool(value);
}

// --- Background -------------------------------------------------------------

void write_autostart(bool enable)
{
    ByteString dir;
    if (auto config = environment("XDG_CONFIG_HOME"sv); config.has_value() && !config->is_empty())
        dir = ByteString::formatted("{}/autostart", config.value());
    else if (auto home = environment("HOME"sv); home.has_value() && !home->is_empty())
        dir = ByteString::formatted("{}/.config/autostart", home.value());
    else
        dir = ByteString { "/tmp/autostart" };
    (void)Core::System::mkdir(dir, 0755);

    // The frontend supplies the caller's app id under "xdp-request-id"; the
    // backend keeps a single generic entry since it is not told the app id here.
    auto path = ByteString::formatted("{}/xdg-desktop-portal-serenity-autostart.desktop", dir);
    if (enable) {
        auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
        if (file.is_error())
            return;
        (void)file.value()->write_until_depleted(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Autostart\n"
            "X-GNOME-Autostart-enabled=true\n"sv);
    } else {
        (void)Core::System::unlink(path);
    }
}

// --- AppChooser -------------------------------------------------------------

void handle_appchooser(Portal::MethodCall& call)
{
    if (call.member() != "ChooseApplication"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    auto const* choices_argument = call.argument(3);
    Portal::Value::Container choices = choices_argument ? choices_argument->container() : Portal::Value::Container {};

    Portal::Dictionary results;
    u32 response = response_cancelled;
    size_t count = choices.size();
    int chosen = -1;

    if (auto chooser = environment("SDE_PORTAL_APP_CHOOSER"sv); count > 0 && chooser.has_value() && !chooser->is_empty()) {
        StringBuilder joined;
        for (size_t i = 0; i < count; ++i) {
            if (i)
                joined.append('\n');
            joined.append(choices[i].as_string());
        }
        if (auto output = run_command({ chooser.value(), "-t", "Choose an Application", "-c", joined.to_byte_string() }, true); !output.is_error() && output.value().has_value())
            chosen = output.value()->to_number<int>().value_or(-1);
    }

    if (count > 0) {
        if (chosen < 0 || static_cast<size_t>(chosen) >= count)
            chosen = 0; // default to the first offered application
        results.append(Portal::Value::Entry { "choice", Portal::Value::make_string(choices[static_cast<size_t>(chosen)].as_string()) });
        response = response_success;
    }
    call.reply_response(response, results);
}

// --- Email ------------------------------------------------------------------

void handle_email(Portal::MethodCall& call)
{
    if (call.member() != "ComposeEmail"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    auto address = call.string_argument(3);
    auto uri = ByteString::formatted("mailto:{}", address);
    bool opened = false;
    if (auto opener = environment("SDE_PORTAL_OPENER"sv); opener.has_value() && !opener->is_empty()) {
        spawn_async({ opener.value(), uri });
        opened = true;
    } else {
        auto result = run_command({ "xdg-open", uri }, false);
        opened = !result.is_error();
    }
    call.reply_response(opened ? response_success : response_other_error, {});
}

// --- ProxyResolver ----------------------------------------------------------

void handle_proxyresolver(Portal::MethodCall& call)
{
    if (call.member() != "Lookup"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    // Empty list = "use the proxy autoconfiguration / direct connection".
    call.reply({ Portal::Value::make_array({}, "s"sv) });
}

// --- OpenURI ----------------------------------------------------------------

Optional<ByteString> materialize_fd(int fd)
{
    ByteString directory;
    if (auto tmp = environment("TMPDIR"sv); tmp.has_value() && !tmp->is_empty())
        directory = tmp.value();
    else
        directory = ByteString { "/tmp" };

    auto path = ByteString::formatted("{}/sde-openfile-XXXXXX", directory);
    Array<char, PATH_MAX> buffer;
    memcpy(buffer.data(), path.characters(), min(path.length() + 1, buffer.size()));
    int output = mkstemp(buffer.data());
    if (output < 0) {
        ::close(fd);
        return {};
    }
    lseek(fd, 0, SEEK_SET);
    char chunk[65536];
    ssize_t n;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = ::write(output, chunk + offset, static_cast<size_t>(n - offset));
            if (written <= 0)
                break;
            offset += written;
        }
    }
    ::close(output);
    ::close(fd);
    return ByteString { buffer.data() };
}

void handle_openuri(Portal::MethodCall& call)
{
    if (call.member() == "Activate"sv) {
        // Activate an installed application by its app id (desktop file id).
        auto app_id = call.string_argument(1);
        bool launched = false;
        if (!app_id.is_empty()) {
            if (auto opener = environment("SDE_PORTAL_OPENER"sv); opener.has_value() && !opener->is_empty()) {
                spawn_async({ opener.value(), app_id });
                launched = true;
            } else {
                auto result = run_command({ "gio", "launch", app_id }, false);
                launched = !result.is_error();
            }
        }
        call.reply_response(launched ? response_success : response_other_error, {});
        return;
    }

    ByteString uri;
    if (call.member() == "OpenURI"sv || call.member() == "OpenDirectory"sv) {
        uri = call.string_argument(2);
    } else if (call.member() == "OpenFile"sv) {
        // Materialise the fd and open the resulting file:// URI.
        auto const* fd_argument = call.argument(2);
        if (fd_argument && fd_argument->is(Portal::Value::Type::UnixFd)) {
            if (auto path = materialize_fd(fd_argument->as_unix_fd()); path.has_value())
                uri = filename_to_uri(path.value());
        }
    } else {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }

    bool opened = !uri.is_empty() && launch_uri(uri);
    call.reply_response(opened ? response_success : response_other_error, {});
}

// --- Notification -----------------------------------------------------------

void show_notification(ByteString const& id, ByteString const& title, ByteString const& body, ByteString const& icon)
{
    if (auto notifier = environment("SDE_PORTAL_NOTIFIER"sv); notifier.has_value() && !notifier->is_empty()) {
        spawn_async({ notifier.value(), id, title, body, icon });
        return;
    }
    spawn_async({ "notify-send", title, body });
}

void handle_notification(Portal::Service& service, Portal::MethodCall& call)
{
    if (call.member() == "AddNotification"sv) {
        auto id = call.string_argument(0);
        auto const* notification = call.argument(1);
        ByteString title;
        ByteString body;
        ByteString icon;
        if (notification) {
            title = notification->string_or("title"sv);
            body = notification->string_or("body"sv);
            // 'icon' is a serialized (sv): "themed" + a name, or "file-descriptor"
            // + an fd. Pass the themed name through; fd icons are skipped.
            if (auto const* icon_value = notification->find("icon"sv)) {
                auto const& structure = icon_value->unwrapped();
                if (structure.is(Portal::Value::Type::Struct) && structure.container().size() == 2) {
                    auto const& kind = structure.container()[0];
                    auto const& value = structure.container()[1].unwrapped();
                    if (kind.as_string() == "themed"sv && value.is(Portal::Value::Type::String))
                        icon = value.as_string();
                }
            }
        }
        show_notification(id, title, body, icon);
        call.reply();
        return;
    }
    if (call.member() == "RemoveNotification"sv) {
        auto id = call.string_argument(0);
        // Tell the client the notification is gone (reason 2 = dismissed).
        service.emit_signal(notification_interface, "NotificationClosed"sv,
            { Portal::Value::make_string(id), Portal::Value::make_uint32(2) });
        call.reply();
        return;
    }
    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

// --- Realtime / GameMode ----------------------------------------------------

void handle_realtime(Portal::MethodCall& call)
{
    // Accepted (the frontend/realtime policy decides; SDE grants it).
    call.reply_response(response_success, {});
}

void handle_gamemode(Portal::MethodCall& call)
{
    if (call.member() == "UnregisterGame"sv) {
        call.reply();
        return;
    }
    Portal::Dictionary results;
    if (call.member() == "QueryStatus"sv)
        results.append(Portal::Value::Entry { "active", Portal::Value::make_bool(false) });
    call.reply_response(response_success, results);
}

Optional<Portal::Value> generic_version_property(StringView property, u32 version)
{
    if (property == "version"sv)
        return Portal::Value::make_uint32(version);
    return {};
}

// --- FileChooser ------------------------------------------------------------

void handle_filechooser(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    if (call.member() != "OpenFile"sv && call.member() != "SaveFile"sv && call.member() != "SaveFiles"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }

    bool const is_save_files = call.member() == "SaveFiles"sv;
    auto const* options = call.argument(4);
    auto const* files = is_save_files ? call.argument(5) : nullptr;

    bool multiple = false;
    bool directory = false;
    ByteString current_name;
    ByteString current_folder;
    if (options) {
        multiple = options->bool_or("multiple"sv);
        directory = options->bool_or("directory"sv);
        current_name = options->string_or("current_name"sv);
        if (auto const* folder = options->find("current_folder"sv); folder && folder->unwrapped().is(Portal::Value::Type::String))
            current_folder = folder->unwrapped().as_string();
    }
    // SaveFiles names the files to save; use the first as the suggested name.
    if (is_save_files && files && !files->container().is_empty())
        current_name = files->container().first().as_string();

    auto chooser = environment("SDE_PORTAL_FILE_CHOOSER"sv);
    if (!chooser.has_value() || chooser->is_empty()) {
        call.reply_response(response_other_error, results);
        return;
    }

    auto mode = call.member() == "OpenFile"sv ? (directory ? "folder"sv : "open"sv) : "save"sv;
    ByteString folder_argument = !current_folder.is_empty() ? current_folder : (environment("HOME"sv).has_value() ? environment("HOME"sv).value() : ByteString { "/tmp" });
    ByteString name_argument = !current_name.is_empty() ? current_name : ByteString { "Untitled" };
    ByteString title_argument = call.string_argument(3);
    if (title_argument.is_empty())
        title_argument = ByteString { "Select File" };

    Vector<ByteString> arguments { chooser.value(), "-m", mode, "-t", title_argument, "-n", name_argument, "-f", folder_argument };
    if (multiple)
        arguments.append("--multiple"sv);

    auto command = run_command(arguments, true);
    u32 response = response_cancelled;
    if (!command.is_error() && command.value().has_value() && !command.value()->is_empty()) {
        auto path = command.value().value();
        Portal::Value::Container uris;
        uris.append(Portal::Value::make_string(filename_to_uri(path)));
        // SaveFiles: the remaining files are placed next to the chosen one.
        if (is_save_files && files) {
            auto separator = path.find_last('/');
            auto directory_path = separator.has_value() ? path.substring(0, separator.value()) : ByteString { "." };
            auto const& file_list = files->container();
            for (size_t i = 1; i < file_list.size(); ++i)
                uris.append(Portal::Value::make_string(filename_to_uri(ByteString::formatted("{}/{}", directory_path, file_list[i].as_string()))));
        }
        results.append(Portal::Value::Entry { "uris", Portal::Value::make_array(move(uris), "s"sv) });
        response = response_success;
    } else if (command.is_error()) {
        response = response_other_error;
    }
    call.reply_response(response, results);
}

// --- Screenshot -------------------------------------------------------------

// Encode a bitmap as a PNG at `path`.
bool save_png(Gfx::Bitmap const& bitmap, ByteString const& path)
{
    auto png = Gfx::PNGWriter::encode(bitmap);
    if (png.is_error())
        return false;
    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return false;
    return !file.value()->write_until_depleted(png.value()).is_error();
}

bool sample_png_color(ByteString const& path, int x, int y, double& out_r, double& out_g, double& out_b)
{
    auto decoded = Gfx::Bitmap::load_from_file(path);
    if (decoded.is_error())
        return false;
    auto const& bitmap = *decoded.value();
    int width = bitmap.width();
    int height = bitmap.height();
    if (x < 0 || x >= width)
        x = width / 2;
    if (y < 0 || y >= height)
        y = height / 2;

    auto color = bitmap.get_pixel(x, y);
    out_r = color.red() / 255.0;
    out_g = color.green() / 255.0;
    out_b = color.blue() / 255.0;
    return true;
}

bool crop_png(ByteString const& src, ByteString const& dst, int x, int y, int w, int h)
{
    auto decoded = Gfx::Bitmap::load_from_file(src);
    if (decoded.is_error())
        return false;
    auto const& full = *decoded.value();
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x + w > full.width())
        w = full.width() - x;
    if (y + h > full.height())
        h = full.height() - y;
    if (w <= 0 || h <= 0)
        return false;

    auto cropped = Gfx::Bitmap::create(full.format(), { w, h });
    if (cropped.is_error())
        return false;
    Gfx::Painter painter { *cropped.value() };
    painter.blit({ 0, 0 }, full, { x, y, w, h });
    return save_png(*cropped.value(), dst);
}

void handle_screenshot(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    bool const is_pick_color = call.member() == "PickColor"sv;
    if (!is_pick_color && call.member() != "Screenshot"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }

    bool interactive = false;
    if (!is_pick_color) {
        if (auto const* options = call.argument(3); options)
            interactive = options->bool_or("interactive"sv);
    }

    auto runtime = runtime_dir();
    auto path = ByteString::formatted("{}/sde-{}-{}.png", runtime, is_pick_color ? "pickcolor"sv : "screenshot"sv, getpid());
    if (Screencast::capture_png(path) != 0) {
        call.reply_response(response_other_error, results);
        return;
    }

    if (!is_pick_color) {
        if (interactive) {
            auto overlay = environment("SDE_PORTAL_SCREENSHOT_OVERLAY"sv);
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            bool selected = false;
            if (overlay.has_value() && !overlay->is_empty()) {
                if (auto output = run_command({ overlay.value(), "-i", path }, true); !output.is_error() && output.value().has_value()) {
                    auto parts = output.value().value().split(' ');
                    if (parts.size() == 4) {
                        x = parts[0].to_number<int>().value_or(0);
                        y = parts[1].to_number<int>().value_or(0);
                        w = parts[2].to_number<int>().value_or(0);
                        h = parts[3].to_number<int>().value_or(0);
                        selected = w > 0 && h > 0;
                    }
                }
            }
            if (selected) {
                auto cropped = ByteString::formatted("{}/sde-screenshot-area-{}.png", runtime, getpid());
                if (crop_png(path, cropped, x, y, w, h)) {
                    results.append(Portal::Value::Entry { "uri", Portal::Value::make_string(filename_to_uri(cropped)) });
                    call.reply_response(response_success, results);
                    return;
                }
            } else if (overlay.has_value() && !overlay->is_empty()) {
                // The overlay ran but the user cancelled: report cancellation.
                call.reply_response(response_cancelled, results);
                return;
            }
        }
        results.append(Portal::Value::Entry { "uri", Portal::Value::make_string(filename_to_uri(path)) });
        call.reply_response(response_success, results);
        return;
    }

    int px = -1;
    int py = -1;
    if (auto picker = environment("SDE_PORTAL_COLOR_PICKER"sv); picker.has_value() && !picker->is_empty()) {
        if (auto output = run_command({ picker.value(), "-i", path }, true); !output.is_error() && output.value().has_value()) {
            auto parts = output.value().value().split(' ');
            if (parts.size() == 2) {
                px = parts[0].to_number<int>().value_or(-1);
                py = parts[1].to_number<int>().value_or(-1);
            }
        }
    }

    double r = 0;
    double g = 0;
    double b = 0;
    if (sample_png_color(path, px, py, r, g, b)) {
        results.append(Portal::Value::Entry { "color", Portal::Value::make_struct({ Portal::Value::make_double(r), Portal::Value::make_double(g), Portal::Value::make_double(b) }) });
        call.reply_response(response_success, results);
        return;
    }
    call.reply_response(response_other_error, results);
}

// --- Print ------------------------------------------------------------------

ByteString collect_printers()
{
    StringBuilder list;
    if (auto output = run_command({ "/bin/sh", "-c", "lpstat -p 2>/dev/null" }, true); !output.is_error() && output.value().has_value()) {
        for (auto line : output.value().value().split('\n')) {
            auto space = line.find(' ');
            if (!space.has_value())
                continue;
            auto rest = line.substring_view(space.value() + 1);
            auto end = rest.find(' ').value_or(rest.length());
            auto name = rest.substring_view(0, end);
            if (!name.is_empty()) {
                if (list.length())
                    list.append(',');
                list.append(name);
            }
        }
    }
    if (list.is_empty())
        return ByteString { "Print to File" };
    return list.to_byte_string();
}

void handle_print(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    if (call.member() == "PreparePrint"sv) {
        auto title = call.string_argument(3);
        auto printers = collect_printers();
        ByteString chosen_printer;
        u32 copies = 1;
        u32 response = response_cancelled;

        if (auto dialog = environment("SDE_PORTAL_PRINT_DIALOG"sv); dialog.has_value() && !dialog->is_empty()) {
            auto output = run_command({ dialog.value(), "-t", title.is_empty() ? ByteString { "Print" } : title, "-p", printers }, true);
            if (!output.is_error() && output.value().has_value()) {
                auto tokens = output.value().value().split('\t');
                if (!tokens.is_empty() && !tokens[0].is_empty()) {
                    chosen_printer = tokens[0];
                    if (tokens.size() > 1)
                        copies = max(1u, tokens[1].to_number<u32>().value_or(1));
                    response = response_success;
                }
            }
        }

        Portal::Dictionary settings;
        settings.append(Portal::Value::Entry { "printer", Portal::Value::make_string(chosen_printer) });
        settings.append(Portal::Value::Entry { "copies", Portal::Value::make_uint32(copies) });
        results.append(Portal::Value::Entry { "settings", Portal::Value::make_dict(move(settings)) });
        results.append(Portal::Value::Entry { "page_setup", Portal::Value::make_dict({}) });
        call.reply_response(response, results);
        return;
    }

    if (call.member() == "Print"sv) {
        auto const* fd_argument = call.argument(4);
        u32 response = response_other_error;
        auto command = environment("SDE_PORTAL_PRINT_CMD"sv);
        if (fd_argument && fd_argument->is(Portal::Value::Type::UnixFd) && command.has_value() && !command->is_empty()) {
            if (feed_fd_to_shell(fd_argument->as_unix_fd(), command.value()))
                response = response_success;
        }
        if (fd_argument && fd_argument->is(Portal::Value::Type::UnixFd))
            ::close(fd_argument->as_unix_fd());
        call.reply_response(response, results);
        return;
    }

    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

// --- Wallpaper --------------------------------------------------------------

// Decode the image at `uri` and write the compositor's wallpaper state
// (wallpaper.raw: {magic, format=BGRA8888, width, height, pitch} + pixels).
bool write_wallpaper_raw(StringView uri)
{
    auto path = filename_from_uri(uri);
    if (!path.has_value())
        return false;
    auto decoded = Gfx::Bitmap::load_from_file(path.value());
    if (decoded.is_error())
        return false;

    // The wallpaper state is BGRA8888; convert whatever the decoder produced.
    auto converted = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, decoded.value()->size());
    if (converted.is_error())
        return false;
    {
        Gfx::Painter painter { *converted.value() };
        painter.blit({ 0, 0 }, *decoded.value(), { 0, 0, decoded.value()->width(), decoded.value()->height() });
    }
    auto const& bitmap = *converted.value();

    int width = bitmap.width();
    int height = bitmap.height();
    int pitch = width * 4;

    struct Header {
        u32 magic;
        u32 format;
        u32 width;
        u32 height;
        u32 pitch;
    };
    Header header = { 0x53445750u, 2u /* BGRA8888 */, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(pitch) };

    ByteBuffer raw;
    raw.resize(sizeof(header) + static_cast<size_t>(pitch) * static_cast<size_t>(height));
    memcpy(raw.data(), &header, sizeof(header));
    for (int y = 0; y < height; ++y)
        memcpy(raw.data() + sizeof(header) + static_cast<size_t>(y) * pitch, bitmap.scanline_u8(y), static_cast<size_t>(pitch));

    auto raw_path = portal_state_file("wallpaper.raw"sv);
    auto file = Core::File::open(raw_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error())
        return false;
    if (auto result = file.value()->write_until_depleted(raw); result.is_error())
        return false;

    auto mode_path = portal_state_file("wallpaper.mode"sv);
    if (auto mode_file = Core::File::open(mode_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !mode_file.is_error())
        (void)mode_file.value()->write_until_depleted("stretch"sv);
    return true;
}

void handle_wallpaper(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    if (call.member() != "SetWallpaper"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    call.reply_response(write_wallpaper_raw(call.string_argument(3)) ? response_success : response_other_error, results);
}

// --- Secret -----------------------------------------------------------------

ByteString sanitized_app_id(StringView app_id)
{
    StringBuilder result;
    for (auto character : app_id.is_empty() ? "default"sv : app_id) {
        if (character == '/' || character == '\\')
            result.append('_');
        else
            result.append(character);
    }
    return result.to_byte_string();
}

void handle_secret(Portal::MethodCall& call)
{
    if (call.member() != "RetrieveSecret"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    auto app_id = call.string_argument(1);
    auto name = sanitized_app_id(app_id);
    auto dir = portal_state_file("secrets"sv);
    (void)Core::System::mkdir(dir, 0700);
    auto path = ByteString::formatted("{}/{}", dir, name);

    Array<u8, 64> secret {};
    bool have_secret = false;
    if (auto file = Core::File::open(path, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error() && contents.value().size() == secret.size()) {
            memcpy(secret.data(), contents.value().data(), secret.size());
            have_secret = true;
        }
    }
    if (!have_secret) {
        // Read exactly 64 bytes from /dev/urandom (it never reaches EOF), with
        // AK's PRNG as the fallback.
        size_t filled = 0;
        if (auto random = Core::File::open("/dev/urandom"sv, Core::File::OpenMode::Read); !random.is_error()) {
            while (filled < secret.size()) {
                auto chunk = random.value()->read_some(secret.span().slice(filled));
                if (chunk.is_error() || chunk.value().is_empty())
                    break;
                filled += chunk.value().size();
            }
        }
        while (filled < secret.size())
            secret[filled++] = static_cast<u8>(get_random_uniform(256));
        if (auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !file.is_error())
            (void)file.value()->write_until_depleted(secret.span());
    }

    Portal::Value::Container bytes;
    for (auto byte : secret)
        bytes.append(Portal::Value::make_byte(byte));
    call.reply({ Portal::Value::make_uint32(response_success), Portal::Value::make_array(move(bytes), "y"sv) });
}

// --- USB / DynamicLauncher / Access -----------------------------------------

void handle_usb(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    if (call.member() == "ListDevices"sv) {
        // No USB device broker: report an empty list (standards-compliant).
        results.append(Portal::Value::Entry { "devices", Portal::Value::make_array({}, "(ua{sv})"sv) });
        call.reply_response(response_success, results);
        return;
    }
    if (call.member() == "AcquireDevices"sv || call.member() == "ReleaseDevices"sv) {
        call.reply_response(response_success, results);
        return;
    }
    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

void handle_dynamic_launcher(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    if (call.member() == "RequestInstallToken"sv) {
        StringBuilder token;
        for (int i = 0; i < 32; ++i)
            token.append("0123456789abcdef"[get_random_uniform(16)]);
        call.reply({ Portal::Value::make_string(token.to_byte_string()) });
        return;
    }
    if (call.member() == "PrepareInstall"sv) {
        auto name = call.string_argument(3);
        // Echo the requested name; no icon post-processing.
        results.append(Portal::Value::Entry { "name", Portal::Value::make_string(name.is_empty() ? ByteString { "Application" } : name) });
        call.reply_response(response_success, results);
        return;
    }
    if (call.member() == "Launch"sv) {
        auto desktop_file_id = call.string_argument(3);
        bool launched = false;
        if (!desktop_file_id.is_empty()) {
            auto result = run_command({ "gio", "launch", desktop_file_id }, false);
            launched = !result.is_error();
        }
        call.reply_response(launched ? response_success : response_other_error, results);
        return;
    }
    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
}

// A LibGUI permission dialog (AccessDialog) prints 0 (allow) or 1 (deny).
void handle_access(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    if (call.member() != "AccessDialog"sv) {
        call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        return;
    }
    auto title = call.string_argument(3);
    auto subtitle = call.string_argument(4);
    auto body = call.string_argument(5);

    u32 response = response_cancelled;
    if (auto dialog = environment("SDE_PORTAL_ACCESS_DIALOG"sv); dialog.has_value() && !dialog->is_empty()) {
        auto output = run_command({ dialog.value(), "-t", title.is_empty() ? ByteString { "Allow this request?" } : title, "-s", subtitle, "-b", body }, true);
        if (!output.is_error() && output.value().has_value())
            response = static_cast<u32>(max(0, output.value()->to_number<int>().value_or(0)));
    }
    call.reply_response(response, results);
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    Core::EventLoop event_loop;

    auto service = TRY(Portal::Service::create(bus_name.to_byte_string(), object_path.to_byte_string()));

    register_portal_interface(*service,
        settings_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_settings(call); },
        [](StringView property) { return generic_version_property(property, 2); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        account_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_account(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        lockdown_interface.to_byte_string(),
        {},
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "disable_application_handlers"sv)
                return bool_setting(property, "SDE_PORTAL_LOCKDOWN_APP_HANDLERS"sv);
            if (property == "disable_commandline"sv)
                return bool_setting(property, "SDE_PORTAL_LOCKDOWN_COMMANDLINE"sv);
            if (property == "disable_printing"sv)
                return bool_setting(property, "SDE_PORTAL_LOCKDOWN_PRINTING"sv);
            if (property == "disable_save_to_disk"sv)
                return bool_setting(property, "SDE_PORTAL_LOCKDOWN_SAVE_TO_DISK"sv);
            if (property == "disable_location"sv)
                return bool_setting(property, "SDE_PORTAL_LOCKDOWN_LOCATION"sv);
            if (property == "disable_user_switching"sv)
                return bool_setting(property, "SDE_PORTAL_LOCKDOWN_USER_SWITCHING"sv);
            return {};
        },
        Vector<ByteString> { "disable_application_handlers"sv, "disable_commandline"sv, "disable_printing"sv, "disable_save_to_disk"sv, "disable_location"sv, "disable_user_switching"sv });

    register_portal_interface(*service,
        background_interface.to_byte_string(),
        [](Portal::MethodCall& call) {
            if (call.member() == "SetStatus"sv) {
                call.reply();
                return;
            }
            if (call.member() == "SetAutostart"sv) {
                bool enable = call.argument(0) && call.argument(0)->as_bool();
                write_autostart(enable);
                call.reply();
                return;
            }
            if (call.member() == "SetWallpaperURI"sv) {
                // Same effect as Wallpaper.SetWallpaper: write the compositor's
                // wallpaper state from the given file:// URI.
                write_wallpaper_raw(call.string_argument(3));
                call.reply();
                return;
            }
            call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(1);
            if (property == "ApplicationName"sv)
                return Portal::Value::make_string(ByteString { "Serenity Desktop Environment" });
            if (property == "ApplicationID"sv)
                return Portal::Value::make_string(ByteString { "org.serenityos.SDE" });
            return {};
        },
        Vector<ByteString> { "version"sv, "ApplicationName"sv, "ApplicationID"sv });

    register_portal_interface(*service,
        appchooser_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_appchooser(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        email_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_email(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        location_interface.to_byte_string(),
        [](Portal::MethodCall& call) {
            // No location provider: report "not available" (response 2).
            call.reply_response(response_other_error, {});
        });

    register_portal_interface(*service,
        proxyresolver_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_proxyresolver(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        openuri_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_openuri(call); },
        [](StringView property) { return generic_version_property(property, 3); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        notification_interface.to_byte_string(),
        [service = service.ptr()](Portal::MethodCall& call) { handle_notification(*service, call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        realtime_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_realtime(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        powerprofile_interface.to_byte_string(),
        {},
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "power-saver-enabled"sv) {
                bool enabled = false;
                if (auto value = environment("SDE_POWER_SAVER"sv); value.has_value() && !value->is_empty())
                    enabled = value.value() != "0"sv;
                return Portal::Value::make_bool(enabled);
            }
            return generic_version_property(property, 1);
        },
        Vector<ByteString> { "version"sv, "power-saver-enabled"sv });

    register_portal_interface(*service,
        gamemode_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_gamemode(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        memorymonitor_interface.to_byte_string(),
        {},
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        filechooser_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_filechooser(call); },
        [](StringView property) { return generic_version_property(property, 3); }, // OpenFile, SaveFile, SaveFiles
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        screenshot_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_screenshot(call); },
        [](StringView property) { return generic_version_property(property, 2); }, // Screenshot, PickColor
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        print_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_print(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        wallpaper_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_wallpaper(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        secret_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_secret(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        usb_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_usb(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        dynamiclauncher_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_dynamic_launcher(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    register_portal_interface(*service,
        access_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_access(call); },
        [](StringView property) { return generic_version_property(property, 1); },
        Vector<ByteString> { "version"sv });

    Screencast::register_interface(*service);
    RemoteDesktop::register_interface(*service);
    InputCapture::register_interface(*service);
    GlobalShortcuts::register_interface(*service);

    dbgln("xdg-desktop-portal-serenity: serving {} on {}", object_path, bus_name);
    return event_loop.exec();
}
