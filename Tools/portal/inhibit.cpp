/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The SDE backend for the freedesktop Inhibit portal (docs/15): an application
// asks the session not to end, suspend, idle or switch away. The impl method
// returns nothing, so the result travels through the Request object exported at
// the call's handle (org.freedesktop.impl.portal.Request), and the inhibition
// lasts until the caller closes it.
//
// Inhibiting is layered and best-effort:
//   * logind's block inhibitor (org.freedesktop.login1.Manager.Inhibit) on the
//     system bus, when logind is available -- the portable path; and
//   * SDE_PORTAL_INHIBIT_CMD, the session seam the backend runs with the OR of
//     the live flags (0 when nothing is inhibited any more), which the session
//     wires to its own idle/power path (scripts/sde-idle, sde-power).
// logind is optional here as everywhere in SDE: an absent logind just means the
// seam (and the session) does all the work.

#include "inhibit.h"

#include "host.h"
#include "introspection.h"

#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

#include <dbus/dbus.h>
#include <unistd.h>

namespace {

constexpr StringView inhibit_interface = "org.freedesktop.impl.portal.Inhibit"sv;
constexpr StringView request_interface = "org.freedesktop.impl.portal.Request"sv;

// org.freedesktop.portal.Inhibit flags.
constexpr u32 InhibitLogout = 1;
constexpr u32 InhibitSuspend = 4;
constexpr u32 InhibitIdle = 8;

Portal::Service* s_service;

// One live inhibition, exported as an org.freedesktop.impl.portal.Request at
// `handle` (the frontend closes it to release).
struct Inhibition {
    ByteString handle;
    u32 flags { 0 };
    Vector<int> logind_fds;
};

Vector<NonnullOwnPtr<Inhibition>>& inhibitions()
{
    static Vector<NonnullOwnPtr<Inhibition>> list;
    return list;
}

// The OR of every live inhibition's flags: the seam command sees the session's
// overall state rather than one request at a time.
u32 active_flags()
{
    u32 flags = 0;
    for (auto const& inhibition : inhibitions())
        flags |= inhibition->flags;
    return flags;
}

// Tell the session's seam command about the current set of inhibitors.
void run_seam()
{
    auto command = Portal::environment("SDE_PORTAL_INHIBIT_CMD"sv);
    if (!command.has_value() || command->is_empty())
        return;
    (void)Portal::spawn_async(Vector<ByteString> { command.value(), ByteString::formatted("{}", active_flags()) });
}

// logind's "what" for an inhibit flag, or an empty view when logind cannot
// express it (a user-switch has no logind inhibitor).
StringView logind_what(u32 flag)
{
    switch (flag) {
    case InhibitLogout:
        return "shutdown"sv;
    case InhibitSuspend:
        return "sleep"sv;
    case InhibitIdle:
        return "idle"sv;
    default:
        return {};
    }
}

// Ask logind for a block inhibitor: the returned fd holds the inhibition (close
// it to release). Returns -1 when logind is unavailable.
int take_logind_inhibitor(StringView what, StringView why)
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (!bus) {
        dbus_error_free(&error);
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);

    DBusMessage* message = dbus_message_new_method_call("org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Inhibit");
    if (!message) {
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        return -1;
    }
    ByteString what_string = what.to_byte_string();
    ByteString why_string = why.is_empty() ? ByteString { "application request" } : why.to_byte_string();
    char const* what_arg = what_string.characters();
    char const* who_arg = "xdg-desktop-portal-serenity";
    char const* why_arg = why_string.characters();
    char const* mode_arg = "block";
    dbus_message_append_args(message, DBUS_TYPE_STRING, &what_arg, DBUS_TYPE_STRING, &who_arg,
        DBUS_TYPE_STRING, &why_arg, DBUS_TYPE_STRING, &mode_arg, DBUS_TYPE_INVALID);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, message, 1000, &error);
    dbus_message_unref(message);

    int fd = -1;
    if (reply) {
        DBusMessageIter iter;
        if (dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
            int received = -1;
            dbus_message_iter_get_basic(&iter, &received);
            if (received >= 0)
                fd = dup(received); // the message owns its copy of the descriptor
        }
        dbus_message_unref(reply);
    }
    dbus_error_free(&error);
    dbus_connection_close(bus);
    dbus_connection_unref(bus);
    return fd;
}

void release_inhibition(Inhibition& inhibition)
{
    for (int fd : inhibition.logind_fds) {
        if (fd >= 0)
            ::close(fd);
    }
    inhibition.logind_fds.clear();
}

// org.freedesktop.impl.portal.Request.Close: the caller is done inhibiting.
void handle_close(Portal::MethodCall& call)
{
    // The request object lives at the call's own path.
    for (size_t i = 0; i < inhibitions().size(); ++i) {
        if (inhibitions()[i]->handle != call.path())
            continue;
        release_inhibition(*inhibitions()[i]);
        dbgln("inhibit: released '{}'", inhibitions()[i]->handle);
        if (s_service)
            s_service->remove_object_path(inhibitions()[i]->handle);
        inhibitions().remove(i);
        break;
    }
    run_seam();
    call.reply();
}

void handle_inhibit(Portal::MethodCall& call)
{
    auto handle = call.object_path_argument(0);
    auto const* flags_argument = call.argument(3);
    u32 flags = flags_argument && flags_argument->is(Portal::Value::Type::UInt32) ? flags_argument->as_uint32() : 0;
    auto const* options = call.argument(4);
    ByteString reason = options ? options->string_or("reason"sv) : ByteString {};

    if (handle.is_empty()) {
        call.reply_error(Portal::ErrorName::InvalidArgs, "the inhibit handle is required"sv);
        return;
    }

    auto inhibition = make<Inhibition>();
    inhibition->handle = handle;
    inhibition->flags = flags;
    for (u32 bit = 1; bit <= InhibitIdle; bit <<= 1) {
        auto what = logind_what(bit);
        if (what.is_empty() || !(flags & bit))
            continue;
        if (int fd = take_logind_inhibitor(what, reason); fd >= 0)
            inhibition->logind_fds.append(fd);
    }
    auto const logind_count = inhibition->logind_fds.size();
    if (s_service)
        s_service->add_object_path(handle);
    inhibitions().append(move(inhibition));
    run_seam();
    dbgln("inhibit: '{}' flags {} ({} logind inhibitor(s), {} live)", handle, flags, logind_count, inhibitions().size());

    // The impl method returns nothing: the caller learns the result from the
    // Request.Response signal on the handle.
    call.reply();
    Portal::Dictionary results;
    s_service->emit_signal_on(handle, request_interface, "Response"sv,
        Vector<Portal::Value> { Portal::Value::make_uint32(to_underlying(Portal::Response::Success)), Portal::Value::make_dict(move(results)) });
}

}

void Inhibit::register_interface(Portal::Service& service)
{
    s_service = &service;

    service.register_interface(
        inhibit_interface.to_byte_string(),
        [](Portal::MethodCall& call) {
            if (call.member() == "Inhibit"sv) {
                handle_inhibit(call);
                return;
            }
            call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(1);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(inhibit_interface).value_or(StringView {}));

    // The request objects the inhibitions are exported as. Dispatch is by
    // interface, so this serves a Close call on any request path.
    service.register_interface(
        request_interface.to_byte_string(),
        [](Portal::MethodCall& call) {
            if (call.member() == "Close"sv) {
                handle_close(call);
                return;
            }
            call.reply_error(Portal::ErrorName::UnknownMethod, "unknown method"sv);
        },
        {},
        {},
        Portal::interface_introspection_xml(request_interface).value_or(StringView {}));
}
