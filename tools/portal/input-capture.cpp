/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 InputCapture backend for xdg-desktop-portal-serenity (docs/17-input-capture.md).
//
// org.freedesktop.impl.portal.InputCapture is a thin bridge: the compositor runs
// the EIS implementation (it has the input and the pointer barriers) and this
// backend relays session control and the EIS file descriptor to the app over
// D-Bus. Session control travels over the private serenity_input_capture
// protocol; the compositor's Activated/Deactivated/Disabled/ZonesChanged events
// are forwarded as D-Bus signals.
//
// This is the AK/LibCore port of tools/portal-input-capture.c: the `dbus-1`
// transport (Portal::Service / Portal::MethodCall / Portal::Value, see dbus.h),
// AK types and a Core::Notifier on the Wayland display fd. There is no
// GLib/GIO. The compositor only offers the protocol when SDE_INPUT_CAPTURE=1.
//
// The request/response methods (GetZones, SetPointerBarriers, ConnectToEIS) wait
// for the compositor's reply with `wl_display_roundtrip`; the private protocol
// answers each request synchronously (the compositor emits the zones /
// barriers_result / eis_fd event while handling the request), so the D-Bus reply
// is produced inside this handler. The unsolicited capture events still arrive
// asynchronously through the Core::Notifier.

#include "input-capture.h"
#include "introspection.h"
#include "screencast.h"

#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/Command.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>

#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "serenity-input-capture-client-protocol.h"

namespace {

constexpr StringView input_capture_interface = "org.freedesktop.impl.portal.InputCapture"sv;
constexpr StringView input_capture_manager_name = "serenity_input_capture_manager_v1"sv;
constexpr u32 capability_keyboard = 1u;
constexpr u32 capability_pointer = 2u;
constexpr u32 supported_capabilities = capability_keyboard | capability_pointer;
constexpr u32 input_capture_version = 2;

struct Session {
    ByteString handle;
    struct serenity_input_capture_session_v1* wl_session { nullptr };
    u32 capabilities { 0 };
    // The D-Bus call waiting for the compositor's reply; valid only while a
    // request/response method is blocked in wl_display_roundtrip.
    Portal::MethodCall* pending_eis { nullptr };
    Portal::MethodCall* pending_zones { nullptr };
    Portal::MethodCall* pending_barriers { nullptr };
};

struct wl_display* s_display { nullptr };
struct wl_registry* s_registry { nullptr };
struct serenity_input_capture_manager_v1* s_manager { nullptr };
RefPtr<Core::Notifier> s_wl_notifier;
Portal::Service* s_service { nullptr };
u32 s_activation_id { 0 };
Vector<NonnullOwnPtr<Session>> s_sessions;

// --- Wayland connection -----------------------------------------------------

void registry_global(void*, struct wl_registry* registry, u32 name, char const* interface, u32)
{
    if (interface && input_capture_manager_name == interface)
        s_manager = static_cast<struct serenity_input_capture_manager_v1*>(
            wl_registry_bind(registry, name, &serenity_input_capture_manager_v1_interface, 1));
}

void registry_global_remove(void*, struct wl_registry*, u32)
{
}

const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

void on_wayland_readable()
{
    if (!s_display)
        return;
    if (wl_display_dispatch(s_display) < 0) {
        s_wl_notifier = nullptr;
        return;
    }
    wl_display_flush(s_display);
}

bool ensure_wayland()
{
    if (s_display)
        return s_manager != nullptr;

    s_display = wl_display_connect(nullptr);
    if (!s_display) {
        dbgln("xdg-desktop-portal-serenity: input capture cannot connect to Wayland");
        return false;
    }
    s_registry = wl_display_get_registry(s_display);
    wl_registry_add_listener(s_registry, &registry_listener, nullptr);
    wl_display_roundtrip(s_display);
    wl_display_roundtrip(s_display);

    if (!s_manager) {
        dbgln("xdg-desktop-portal-serenity: compositor does not offer serenity_input_capture (SDE_INPUT_CAPTURE=1?)");
        return false;
    }

    if (s_service) {
        s_wl_notifier = Core::Notifier::construct(wl_display_get_fd(s_display), Core::Notifier::Type::Read, s_service);
        s_wl_notifier->on_activation = [] { on_wayland_readable(); };
    }
    dbgln("xdg-desktop-portal-serenity: input capture connected to the compositor EIS relay");
    return true;
}

// --- sessions ---------------------------------------------------------------

Session* session_find(ByteString const& handle)
{
    for (auto& session : s_sessions) {
        if (session->handle == handle)
            return session.ptr();
    }
    return nullptr;
}

void emit_signal(StringView signal, ByteString const& handle, Portal::Dictionary options)
{
    if (!s_service)
        return;
    s_service->emit_signal(input_capture_interface, signal, {
        Portal::Value::make_object_path(handle),
        Portal::Value::make_dict(move(options)),
    });
}

void on_eis_fd(void* data, struct serenity_input_capture_session_v1*, int32_t fd)
{
    auto* session = static_cast<Session*>(data);
    if (session->pending_eis) {
        session->pending_eis->reply({ Portal::Value::make_unix_fd(fd) });
        session->pending_eis = nullptr;
    }
    close(fd);
}

void on_zones(void* data, struct serenity_input_capture_session_v1*, struct wl_array* zones, uint32_t zone_set)
{
    auto* session = static_cast<Session*>(data);
    if (!session->pending_zones)
        return;

    Portal::Value::Container zone_values;
    size_t const count = zones->size / (4 * sizeof(int32_t));
    auto const* raw = static_cast<int32_t const*>(zones->data);
    for (size_t i = 0; i < count; ++i) {
        zone_values.append(Portal::Value::make_struct({
            Portal::Value::make_uint32(static_cast<u32>(raw[i * 4])),
            Portal::Value::make_uint32(static_cast<u32>(raw[i * 4 + 1])),
            Portal::Value::make_int32(raw[i * 4 + 2]),
            Portal::Value::make_int32(raw[i * 4 + 3]),
        }));
    }

    Portal::Dictionary results;
    results.append(Portal::Value::Entry { ByteString { "zones"sv }, Portal::Value::make_variant(Portal::Value::make_array(move(zone_values), "(uuii)"sv)) });
    results.append(Portal::Value::Entry { ByteString { "zone_set"sv }, Portal::Value::make_variant(Portal::Value::make_uint32(zone_set)) });
    session->pending_zones->reply_response(0, results);
    session->pending_zones = nullptr;
}

void on_barriers_result(void* data, struct serenity_input_capture_session_v1*, struct wl_array* failed)
{
    auto* session = static_cast<Session*>(data);
    if (!session->pending_barriers)
        return;

    Portal::Value::Container failed_values;
    size_t const count = failed->size / sizeof(uint32_t);
    auto const* raw = static_cast<uint32_t const*>(failed->data);
    for (size_t i = 0; i < count; ++i)
        failed_values.append(Portal::Value::make_uint32(raw[i]));

    Portal::Dictionary results;
    results.append(Portal::Value::Entry { ByteString { "failed_barriers"sv }, Portal::Value::make_variant(Portal::Value::make_array(move(failed_values), "u"sv)) });
    session->pending_barriers->reply_response(0, results);
    session->pending_barriers = nullptr;
}

void on_zones_changed(void* data, struct serenity_input_capture_session_v1*)
{
    auto* session = static_cast<Session*>(data);
    emit_signal("ZonesChanged"sv, session->handle, {});
}

void on_activated(void* data, struct serenity_input_capture_session_v1*, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y, uint32_t barrier_id)
{
    auto* session = static_cast<Session*>(data);
    s_activation_id = activation_id;

    Portal::Dictionary options;
    options.append(Portal::Value::Entry { ByteString { "activation_id"sv }, Portal::Value::make_variant(Portal::Value::make_uint32(activation_id)) });
    options.append(Portal::Value::Entry { ByteString { "cursor_position"sv }, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_double(wl_fixed_to_double(cursor_x)), Portal::Value::make_double(wl_fixed_to_double(cursor_y)) })) });
    options.append(Portal::Value::Entry { ByteString { "barrier_id"sv }, Portal::Value::make_variant(Portal::Value::make_uint32(barrier_id)) });
    emit_signal("Activated"sv, session->handle, move(options));
}

void on_deactivated(void* data, struct serenity_input_capture_session_v1*, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y)
{
    auto* session = static_cast<Session*>(data);

    Portal::Dictionary options;
    options.append(Portal::Value::Entry { ByteString { "activation_id"sv }, Portal::Value::make_variant(Portal::Value::make_uint32(activation_id)) });
    options.append(Portal::Value::Entry { ByteString { "cursor_position"sv }, Portal::Value::make_variant(Portal::Value::make_struct({ Portal::Value::make_double(wl_fixed_to_double(cursor_x)), Portal::Value::make_double(wl_fixed_to_double(cursor_y)) })) });
    emit_signal("Deactivated"sv, session->handle, move(options));
}

void on_disabled(void* data, struct serenity_input_capture_session_v1*)
{
    auto* session = static_cast<Session*>(data);
    emit_signal("Disabled"sv, session->handle, {});
}

const struct serenity_input_capture_session_v1_listener session_listener = {
    on_eis_fd,
    on_zones,
    on_zones_changed,
    on_barriers_result,
    on_activated,
    on_deactivated,
    on_disabled,
};

Session* session_create(ByteString const& handle)
{
    if (auto* existing = session_find(handle))
        return existing;

    auto session = make<Session>();
    session->handle = handle;
    session->wl_session = serenity_input_capture_manager_v1_create_session(s_manager, "xdg-desktop-portal-serenity");
    auto* raw = session.ptr();
    serenity_input_capture_session_v1_add_listener(session->wl_session, &session_listener, raw);
    s_sessions.append(move(session));
    wl_display_flush(s_display);
    return raw;
}

// --- the permission prompt --------------------------------------------------

// `atoi`-style leading-integer parse, with the original's MAX(0, ...) clamp: a
// non-numeric (or absent) response means "allow".
u32 parse_dialog_response(StringView text)
{
    text = text.trim_whitespace();
    size_t index = 0;
    bool negative = false;
    if (index < text.length() && (text[index] == '+' || text[index] == '-')) {
        negative = text[index] == '-';
        ++index;
    }
    u32 value = 0;
    while (index < text.length() && text[index] >= '0' && text[index] <= '9') {
        value = value * 10 + static_cast<u32>(text[index] - '0');
        ++index;
    }
    return negative ? 0 : value;
}

u32 run_access_prompt()
{
    auto dialog = Core::Environment::get("SDE_PORTAL_ACCESS_DIALOG"sv);
    // No prompt configured; the compositor's SDE_INPUT_CAPTURE gate is the boundary.
    if (!dialog.has_value() || dialog->is_empty())
        return 0;

    Vector<ByteString> arguments {
        "-t"sv,
        "Allow input capture?"sv,
        "-s"sv,
        ""sv,
        "-b"sv,
        "An application wants to capture your input."sv,
    };

    auto result = Core::command(dialog->to_byte_string(), arguments, {});
    if (result.is_error())
        return 1;

    auto const& command_result = result.value();
    if (command_result.exit_code != 0)
        return 1;

    StringView text { reinterpret_cast<char const*>(command_result.output.data()), command_result.output.size() };
    return parse_dialog_response(text);
}

// --- D-Bus interface --------------------------------------------------------

void handle_call(Portal::MethodCall& call)
{
    if (!ensure_wayland()) {
        call.reply_error(Portal::ErrorName::NotSupported, "input capture is unavailable (compositor has no EIS relay; set SDE_INPUT_CAPTURE=1)"sv);
        return;
    }

    auto const& method = call.member();

    if (method == "CreateSession"sv || method == "CreateSession2"sv) {
        auto session_handle = call.object_path_argument(0);
        if (session_handle.is_empty()) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "missing session handle"sv);
            return;
        }
        session_create(session_handle);
        if (method == "CreateSession"sv)
            call.reply_response(0, {});
        else
            call.reply({ Portal::Value::make_dict({}) });
        return;
    }

    if (method == "Start"sv) {
        auto session_handle = call.object_path_argument(1);
        auto const* options = call.argument(4);
        auto* session = session_handle.is_empty() ? nullptr : session_find(session_handle);
        if (!session) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "unknown session"sv);
            return;
        }

        u32 requested = 0;
        u32 persist_mode = 0;
        bool restored = false;
        if (options) {
            requested = options->uint32_or("capabilities"sv, 0);
            persist_mode = Screencast::clamp_persist_mode(options->uint32_or("persist_mode"sv, 0));

            // The frontend hands back the restore_data an earlier Start returned
            // (it keeps it per app in the permission store): the user already
            // agreed, so skip the prompt.
            restored = Screencast::parse_restore_data(*options).has_value();
        }

        u32 const granted = requested & supported_capabilities;
        // A restored (persisted) session skips the prompt.
        u32 const response = restored ? 0 : run_access_prompt();
        if (response != 0) {
            call.reply_response(response, {});
            return;
        }
        session->capabilities = granted;

        Portal::Dictionary results;
        results.append(Portal::Value::Entry { ByteString { "capabilities"sv }, Portal::Value::make_variant(Portal::Value::make_uint32(granted)) });
        results.append(Portal::Value::Entry { ByteString { "clipboard_enabled"sv }, Portal::Value::make_variant(Portal::Value::make_bool(false)) });
        if (persist_mode != 0) {
            Screencast::RestoreData restore;
            restore.types = granted;
            results.append(Portal::Value::Entry { ByteString { "persist_mode"sv }, Portal::Value::make_variant(Portal::Value::make_uint32(persist_mode)) });
            results.append(Portal::Value::Entry { ByteString { "restore_data"sv }, Portal::Value::make_variant(Screencast::make_restore_data(restore)) });
        }
        dbgln("xdg-desktop-portal-serenity: input capture session started (caps {}, restored {})", granted, restored);
        call.reply_response(0, results);
        return;
    }

    // Remaining methods operate on an existing session.
    ByteString session_handle;
    Portal::Value const* options = nullptr;
    Portal::Value const* barriers = nullptr;
    u32 zone_set = 0;
    if (method == "GetZones"sv) {
        session_handle = call.object_path_argument(1);
        options = call.argument(3);
    } else if (method == "SetPointerBarriers"sv) {
        session_handle = call.object_path_argument(1);
        options = call.argument(3);
        barriers = call.argument(4);
        if (auto const* zone_set_value = call.argument(5); zone_set_value && zone_set_value->is(Portal::Value::Type::UInt32))
            zone_set = zone_set_value->as_uint32();
    } else {
        session_handle = call.object_path_argument(0);
        options = call.argument(2);
    }
    (void)options;

    auto* session = session_handle.is_empty() ? nullptr : session_find(session_handle);
    if (!session) {
        call.reply_error(Portal::ErrorName::InvalidArgs, "unknown session"sv);
        return;
    }

    if (method == "GetZones"sv) {
        session->pending_zones = &call;
        serenity_input_capture_session_v1_get_zones(session->wl_session);
        wl_display_roundtrip(s_display);
        if (session->pending_zones) {
            session->pending_zones = nullptr;
            call.reply_error(Portal::ErrorName::Failed, "compositor did not answer GetZones"sv);
        }
        return;
    }

    if (method == "SetPointerBarriers"sv) {
        struct wl_array packed;
        wl_array_init(&packed);
        if (barriers) {
            for (auto const& barrier : barriers->container()) {
                u32 id = barrier.uint32_or("barrier_id"sv, 0);
                i32 x1 = 0;
                i32 y1 = 0;
                i32 x2 = 0;
                i32 y2 = 0;
                if (auto const* position = barrier.find("position"sv)) {
                    auto const& rectangle = position->unwrapped();
                    if (rectangle.is(Portal::Value::Type::Struct) && rectangle.container().size() >= 4) {
                        x1 = rectangle.container()[0].as_int32();
                        y1 = rectangle.container()[1].as_int32();
                        x2 = rectangle.container()[2].as_int32();
                        y2 = rectangle.container()[3].as_int32();
                    }
                }
                struct PackedBarrier {
                    u32 id;
                    i32 x1;
                    i32 y1;
                    i32 x2;
                    i32 y2;
                } packed_barrier { id, x1, y1, x2, y2 };
                memcpy(wl_array_add(&packed, sizeof(packed_barrier)), &packed_barrier, sizeof(packed_barrier));
            }
        }

        session->pending_barriers = &call;
        serenity_input_capture_session_v1_set_pointer_barriers(session->wl_session, &packed, zone_set);
        wl_array_release(&packed);
        wl_display_roundtrip(s_display);
        if (session->pending_barriers) {
            session->pending_barriers = nullptr;
            call.reply_error(Portal::ErrorName::Failed, "compositor did not answer SetPointerBarriers"sv);
        }
        return;
    }

    if (method == "Enable"sv) {
        serenity_input_capture_session_v1_enable(session->wl_session, session->capabilities);
        wl_display_flush(s_display);
        call.reply_response(0, {});
        return;
    }

    if (method == "Disable"sv) {
        serenity_input_capture_session_v1_disable(session->wl_session);
        wl_display_flush(s_display);
        call.reply_response(0, {});
        return;
    }

    if (method == "Release"sv) {
        u32 activation_id = s_activation_id;
        double cursor_x = 0;
        double cursor_y = 0;
        if (options) {
            activation_id = options->uint32_or("activation_id"sv, activation_id);
            if (auto const* position = options->find("cursor_position"sv)) {
                auto const& point = position->unwrapped();
                if (point.is(Portal::Value::Type::Struct) && point.container().size() >= 2) {
                    cursor_x = point.container()[0].as_double();
                    cursor_y = point.container()[1].as_double();
                }
            }
        }
        serenity_input_capture_session_v1_release(session->wl_session, activation_id, wl_fixed_from_double(cursor_x), wl_fixed_from_double(cursor_y));
        wl_display_flush(s_display);
        call.reply_response(0, {});
        return;
    }

    if (method == "ConnectToEIS"sv) {
        if (session->pending_eis) {
            call.reply_error(Portal::ErrorName::Failed, "ConnectToEIS already pending"sv);
            return;
        }
        session->pending_eis = &call;
        serenity_input_capture_session_v1_connect_to_eis(session->wl_session);
        wl_display_roundtrip(s_display);
        if (session->pending_eis) {
            session->pending_eis = nullptr;
            call.reply_error(Portal::ErrorName::Failed, "compositor did not provide an EIS fd"sv);
        }
        return;
    }

    call.reply_error(Portal::ErrorName::UnknownMethod, ByteString::formatted("unknown method {}", method));
}

}

void InputCapture::register_interface(Portal::Service& service)
{
    s_service = &service;

    service.register_interface(
        input_capture_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_call(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(input_capture_version);
            if (property == "SupportedCapabilities"sv)
                return Portal::Value::make_uint32(supported_capabilities);
            return {};
        },
        Vector<ByteString> { "version"sv, "SupportedCapabilities"sv },
        Portal::interface_introspection_xml(input_capture_interface).value_or(StringView {}));
}
