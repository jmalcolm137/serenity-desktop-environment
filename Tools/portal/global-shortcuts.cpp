/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-global-shortcuts-portal — the SDE backend for the freedesktop
// GlobalShortcuts portal (R5). Shortcuts requested by apps are written to the
// SDE config system (global-shortcuts.ini in the state directory) where the
// compositor binds them; when one fires, the compositor reports it on the
// session FIFO ($XDG_RUNTIME_DIR/sde-global-shortcuts.fifo) and this backend
// emits the Activated signal. It is an ordinary Serenity-style service: AK
// types over the `dbus-1` transport (see dbus.h), a LibCore event loop, and no
// GLib/GIO.

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Environment.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "global-shortcuts.h"
#include "introspection.h"

namespace {

constexpr StringView interface_name = "org.freedesktop.impl.portal.GlobalShortcuts"sv;

// One bound shortcut, as it is remembered for ListShortcuts and written to the
// INI file.
struct Shortcut {
    ByteString session;
    ByteString id;
    ByteString trigger;
    Optional<ByteString> description;
};

ByteString user_runtime_dir()
{
    if (auto value = Core::Environment::get("XDG_RUNTIME_DIR"sv); value.has_value() && !value->is_empty())
        return value->to_byte_string();
    return "/tmp"sv;
}

// The user config dir: $XDG_CONFIG_HOME, else $HOME/.config, else /tmp.
ByteString user_config_dir()
{
    if (auto value = Core::Environment::get("XDG_CONFIG_HOME"sv); value.has_value() && !value->is_empty())
        return value->to_byte_string();
    if (auto home = Core::Environment::get("HOME"sv); home.has_value() && !home->is_empty())
        return ByteString::formatted("{}/.config", *home);
    return "/tmp"sv;
}

// Recursively create a directory, assembled from Core::System::mkdir. Failures
// (including EEXIST) are ignored, exactly like the original.
void create_directories(ByteString const& path, mode_t mode)
{
    for (size_t i = 1; i <= path.length(); ++i) {
        if (i != path.length() && path[i] != '/')
            continue;
        auto component = path.substring(0, i);
        if (component.is_empty())
            continue;
        (void)Core::System::mkdir(component, mode);
    }
}

ByteString shortcuts_ini_path()
{
    ByteString base;
    if (auto state = Core::Environment::get("SDE_STATE_DIR"sv); state.has_value() && !state->is_empty())
        base = state->to_byte_string();
    else
        base = ByteString::formatted("{}/sde", user_config_dir());
    create_directories(base, 0700);
    return ByteString::formatted("{}/global-shortcuts.ini", base);
}

// The trigger format is "CTRL+ALT+t"; normalise the modifier names to the ones
// the compositor's combo parser understands (Ctrl/Alt/Shift/Super).
ByteString normalize_trigger(StringView trigger)
{
    StringBuilder builder;
    size_t start = 0;
    bool first = true;
    for (;;) {
        auto separator = trigger.find('+', start);
        auto end = separator.value_or(trigger.length());
        auto part = trigger.substring_view(start, end - start);
        StringView mapped = part;
        if (part.equals_ignoring_ascii_case("CTRL"sv) || part.equals_ignoring_ascii_case("CONTROL"sv))
            mapped = "Ctrl"sv;
        else if (part.equals_ignoring_ascii_case("ALT"sv))
            mapped = "Alt"sv;
        else if (part.equals_ignoring_ascii_case("SHIFT"sv))
            mapped = "Shift"sv;
        else if (part.equals_ignoring_ascii_case("LOGO"sv) || part.equals_ignoring_ascii_case("SUPER"sv) || part.equals_ignoring_ascii_case("META"sv))
            mapped = "Super"sv;
        if (!first)
            builder.append('+');
        builder.append(mapped);
        first = false;
        if (!separator.has_value())
            break;
        start = *separator + 1;
    }
    return builder.to_byte_string();
}

class Backend {
public:
    void set_service(Portal::Service& service) { m_service = &service; }

    void handle(Portal::MethodCall& call);

private:
    void ensure_fifo();
    void on_fifo_readable();
    void process_fifo_lines();
    void dispatch_line(StringView line);

    void bind_shortcuts(Portal::MethodCall& call);
    void list_shortcuts(Portal::MethodCall& call);
    Portal::Value bound_shortcuts() const;
    void write_shortcuts_ini();

    Portal::Service* m_service { nullptr };
    Vector<Shortcut> m_shortcuts;
    RefPtr<Core::Notifier> m_fifo_notifier;
    Vector<u8> m_fifo_buffer;
};

void Backend::handle(Portal::MethodCall& call)
{
    if (call.member() == "CreateSession"sv) {
        ensure_fifo();
        call.reply_response(0);
        return;
    }
    if (call.member() == "BindShortcuts"sv) {
        ensure_fifo();
        bind_shortcuts(call);
        return;
    }
    if (call.member() == "ListShortcuts"sv) {
        list_shortcuts(call);
        return;
    }
    auto message = ByteString::formatted("unknown method {}", call.member());
    call.reply_error(Portal::ErrorName::UnknownMethod, message.view());
}

// The bound shortcuts as the `a(sa{sv})` the portal returns under "shortcuts":
// each element is the shortcut id plus its trigger (and description if any).
Portal::Value Backend::bound_shortcuts() const
{
    Portal::Value::Container shortcuts;
    for (auto const& shortcut : m_shortcuts) {
        Portal::Dictionary props;
        props.append(Portal::Value::Entry { "trigger"sv, Portal::Value::make_variant(Portal::Value::make_string(shortcut.trigger)) });
        if (shortcut.description.has_value())
            props.append(Portal::Value::Entry { "description"sv, Portal::Value::make_variant(Portal::Value::make_string(shortcut.description.value())) });
        shortcuts.append(Portal::Value::make_struct({ Portal::Value::make_string(shortcut.id), Portal::Value::make_dict(move(props)) }));
    }
    return Portal::Value::make_array(move(shortcuts), "(sa{sv})"sv);
}

void Backend::bind_shortcuts(Portal::MethodCall& call)
{
    auto session_handle = call.object_path_argument(1);
    if (auto const* shortcuts = call.argument(2); shortcuts && shortcuts->is(Portal::Value::Type::Array)) {
        for (auto const& element : shortcuts->container()) {
            if (!element.is(Portal::Value::Type::Struct))
                continue;
            auto const& fields = element.container();
            if (fields.size() < 2)
                continue;
            auto const& id = fields[0];
            auto const& props = fields[1];
            if (!id.is(Portal::Value::Type::String) || !props.is(Portal::Value::Type::Dict))
                continue;
            auto const* trigger = props.find("trigger"sv);
            if (!trigger || !trigger->unwrapped().is(Portal::Value::Type::String))
                continue;

            Shortcut shortcut;
            shortcut.session = session_handle;
            shortcut.id = id.as_string();
            shortcut.trigger = normalize_trigger(trigger->unwrapped().as_string());
            if (auto const* description = props.find("description"sv); description && description->unwrapped().is(Portal::Value::Type::String))
                shortcut.description = description->unwrapped().as_string();
            m_shortcuts.append(move(shortcut));
        }
    }
    write_shortcuts_ini();

    Portal::Dictionary results;
    results.append(Portal::Value::Entry { "shortcuts"sv, Portal::Value::make_variant(bound_shortcuts()) });
    call.reply_response(0, results);
}

void Backend::list_shortcuts(Portal::MethodCall& call)
{
    Portal::Dictionary results;
    results.append(Portal::Value::Entry { "shortcuts"sv, Portal::Value::make_variant(bound_shortcuts()) });
    call.reply_response(0, results);
}

void Backend::write_shortcuts_ini()
{
    auto path = shortcuts_ini_path();
    auto config = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::Yes);
    if (config.is_error()) {
        dbgln("sde-global-shortcuts-portal: could not write {}: {}", path, config.error());
        return;
    }
    auto file = config.release_value();
    for (size_t i = 0; i < m_shortcuts.size(); ++i) {
        auto const& shortcut = m_shortcuts[i];
        auto group = ByteString::formatted("Shortcut{}", i);
        file->write_entry(group, "Session"sv, shortcut.session);
        file->write_entry(group, "Id"sv, shortcut.id);
        file->write_entry(group, "Trigger"sv, shortcut.trigger);
        if (shortcut.description.has_value())
            file->write_entry(group, "Description"sv, shortcut.description.value());
    }
    // `file` is flushed back to disk by ConfigFile::sync() when it goes away.
}

// --- FIFO: compositor -> Activated signal -----------------------------------

void Backend::ensure_fifo()
{
    if (m_fifo_notifier)
        return;
    auto path = ByteString::formatted("{}/sde-global-shortcuts.fifo", user_runtime_dir());
    (void)Core::System::mkfifo(path, 0600);
    auto fd = Core::System::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd.is_error()) {
        dbgln("sde-global-shortcuts-portal: could not open {}: {}", path, fd.error());
        return;
    }
    m_fifo_notifier = Core::Notifier::construct(fd.release_value(), Core::Notifier::Type::Read | Core::Notifier::Type::HangUp);
    m_fifo_notifier->on_activation = [this] { on_fifo_readable(); };
}

void Backend::on_fifo_readable()
{
    if (!m_fifo_notifier)
        return;
    for (;;) {
        u8 buffer[4096];
        auto result = Core::System::read(m_fifo_notifier->fd(), Bytes { buffer, sizeof(buffer) });
        if (result.is_error())
            break;
        auto count = result.release_value();
        if (count <= 0)
            break;
        m_fifo_buffer.append(buffer, static_cast<size_t>(count));
    }
    process_fifo_lines();
}

void Backend::process_fifo_lines()
{
    size_t start = 0;
    for (size_t i = 0; i < m_fifo_buffer.size(); ++i) {
        if (m_fifo_buffer[i] != '\n')
            continue;
        StringView line { reinterpret_cast<char const*>(m_fifo_buffer.data()) + start, i - start };
        dispatch_line(line);
        start = i + 1;
    }
    if (start > 0)
        m_fifo_buffer.remove(0, start);
}

// Each line is "<session_handle>\t<shortcut_id>"; turn it into Activated with a
// fresh monotonic timestamp and no options.
void Backend::dispatch_line(StringView line)
{
    line = line.trim_whitespace();
    auto separator = line.find('\t');
    if (!separator.has_value())
        return;
    if (!m_service)
        return;
    auto session_handle = line.substring_view(0, *separator);
    auto shortcut_id = line.substring_view(*separator + 1);
    auto timestamp = static_cast<u64>(AK::MonotonicTime::now().nanoseconds() / 1000);
    m_service->emit_signal(interface_name, "Activated"sv, {
        Portal::Value::make_object_path(session_handle.to_byte_string()),
        Portal::Value::make_string(shortcut_id.to_byte_string()),
        Portal::Value::make_uint64(timestamp),
        Portal::Value::make_dict({}),
    });
}

}

void GlobalShortcuts::register_interface(Portal::Service& service)
{
    static Backend backend;
    backend.set_service(service);
    auto* backend_ptr = &backend;
    service.register_interface(
        interface_name.to_byte_string(),
        [backend_ptr](Portal::MethodCall& call) { backend_ptr->handle(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(1);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(interface_name).value_or(StringView {}));
}
