/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// org.freedesktop.impl.portal.Clipboard over ext-data-control-v1.
//
// This is deliberately the same model as KDE's ClipboardPortal: the backend is
// a privileged data-control client, mirrors the current selection, and proxies
// the fd-based transfer protocol. Clipboard sessions are not created here; they
// belong to RemoteDesktop or InputCapture, which expose `clipboard_enabled` in
// their Start result after RequestClipboard has been accepted.

#include "clipboard.h"
#include "input-capture.h"
#include "introspection.h"
#include "remote-desktop.h"

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Notifier.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

namespace {

constexpr StringView clipboard_interface = "org.freedesktop.impl.portal.Clipboard"sv;
constexpr u32 clipboard_version = 1;

struct Offer {
    struct ext_data_control_offer_v1* wl { nullptr };
    Vector<ByteString> mime_types;
};

struct Source {
    struct ext_data_control_source_v1* wl { nullptr };
    ByteString session_handle;
};

struct PendingTransfer {
    u32 serial { 0 };
    int fd { -1 };
    ByteString session_handle;
};

struct wl_display* s_display { nullptr };
struct wl_registry* s_registry { nullptr };
struct wl_seat* s_seat { nullptr };
struct ext_data_control_manager_v1* s_manager { nullptr };
struct ext_data_control_device_v1* s_device { nullptr };
RefPtr<Core::Notifier> s_notifier;
Portal::Service* s_service { nullptr };
Offer* s_current_offer { nullptr };
Source* s_current_source { nullptr };
Vector<Offer*> s_offers;
Vector<ByteString> s_enabled_sessions;
Vector<PendingTransfer> s_pending_transfers;
u32 s_next_transfer_serial { 1 };

void emit_owner_changed(ByteString const& session_handle);
void destroy_source();

bool session_enabled(ByteString const& handle)
{
    return s_enabled_sessions.contains_slow(handle);
}

Vector<ByteString> mime_types_from_options(Portal::Value const* options)
{
    Vector<ByteString> result;
    if (!options || !options->is(Portal::Value::Type::Dict))
        return result;
    auto const* value = options->find("mime_types"sv);
    if (!value || !value->unwrapped().is(Portal::Value::Type::Array))
        return result;
    for (auto const& item : value->unwrapped().container()) {
        if (item.is(Portal::Value::Type::String))
            result.append(item.as_string());
    }
    return result;
}

void on_offer_offer(void* data, struct ext_data_control_offer_v1*, char const* mime_type)
{
    auto* offer = static_cast<Offer*>(data);
    if (mime_type && !offer->mime_types.contains_slow(mime_type))
        offer->mime_types.append(ByteString { mime_type });
}

const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = on_offer_offer,
};

Offer* find_offer(struct ext_data_control_offer_v1* wl)
{
    for (auto* offer : s_offers) {
        if (offer->wl == wl)
            return offer;
    }
    return nullptr;
}

void delete_offer(Offer* offer)
{
    if (!offer)
        return;
    s_offers.remove_first_matching([&](auto* candidate) { return candidate == offer; });
    if (s_current_offer == offer)
        s_current_offer = nullptr;
    if (offer->wl)
        ext_data_control_offer_v1_destroy(offer->wl);
    delete offer;
}

void on_device_data_offer(void*, struct ext_data_control_device_v1*, struct ext_data_control_offer_v1* id)
{
    if (!id)
        return;
    auto* offer = new Offer;
    offer->wl = id;
    ext_data_control_offer_v1_add_listener(id, &offer_listener, offer);
    s_offers.append(offer);
}

void on_device_selection(void*, struct ext_data_control_device_v1*, struct ext_data_control_offer_v1* id)
{
    if (s_current_offer)
        delete_offer(s_current_offer);
    if (id)
        s_current_offer = find_offer(id);
    for (auto const& session : s_enabled_sessions)
        emit_owner_changed(session);
}

void on_device_primary_selection(void*, struct ext_data_control_device_v1*, struct ext_data_control_offer_v1* id)
{
    // The Clipboard portal covers the regular selection. Primary selection is
    // exposed through the compositor's standard primary-selection protocol and
    // is not part of this portal interface.
    if (id)
        delete_offer(find_offer(id));
}

void on_device_finished(void*, struct ext_data_control_device_v1*)
{
    s_device = nullptr;
}

const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = on_device_data_offer,
    .selection = on_device_selection,
    .finished = on_device_finished,
    .primary_selection = on_device_primary_selection,
};

void on_source_send(void* data, struct ext_data_control_source_v1*, char const* mime_type, int32_t fd)
{
    auto* source = static_cast<Source*>(data);
    if (!source || !mime_type || fd < 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    u32 serial = s_next_transfer_serial++;
    s_pending_transfers.append(PendingTransfer { serial, fd, source->session_handle });
    if (s_service) {
        s_service->emit_signal(clipboard_interface, "SelectionTransfer"sv, {
            Portal::Value::make_object_path(source->session_handle),
            Portal::Value::make_string(ByteString { mime_type }),
            Portal::Value::make_uint32(serial),
        });
    }
}

void on_source_cancelled(void* data, struct ext_data_control_source_v1*)
{
    auto* source = static_cast<Source*>(data);
    if (s_current_source == source)
        s_current_source = nullptr;
    if (source && source->wl) {
        ext_data_control_source_v1_destroy(source->wl);
        source->wl = nullptr;
    }
}

const struct ext_data_control_source_v1_listener source_listener = {
    .send = on_source_send,
    .cancelled = on_source_cancelled,
};

void on_registry_global(void*, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, wl_seat_interface.name) == 0 && !s_seat)
        s_seat = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, min(version, 1u)));
    else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 && !s_manager)
        s_manager = static_cast<struct ext_data_control_manager_v1*>(wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, min(version, 1u)));
}

void on_registry_global_remove(void*, struct wl_registry*, uint32_t) { }

const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

void on_wayland_readable()
{
    if (!s_display)
        return;
    if (wl_display_dispatch(s_display) < 0) {
        s_notifier = nullptr;
        return;
    }
    wl_display_flush(s_display);
}

bool ensure_wayland()
{
    if (s_device)
        return true;
    if (s_display)
        return false;

    s_display = wl_display_connect(nullptr);
    if (!s_display) {
        dbgln("xdg-desktop-portal-serenity: clipboard cannot connect to Wayland");
        return false;
    }
    s_registry = wl_display_get_registry(s_display);
    wl_registry_add_listener(s_registry, &registry_listener, nullptr);
    wl_display_roundtrip(s_display);
    wl_display_roundtrip(s_display);
    if (!s_manager || !s_seat) {
        dbgln("xdg-desktop-portal-serenity: clipboard requires ext_data_control_manager_v1 and a seat");
        return false;
    }
    s_device = ext_data_control_manager_v1_get_data_device(s_manager, s_seat);
    ext_data_control_device_v1_add_listener(s_device, &device_listener, nullptr);
    wl_display_flush(s_display);
    if (s_service) {
        s_notifier = Core::Notifier::construct(wl_display_get_fd(s_display), Core::Notifier::Type::Read, s_service);
        s_notifier->on_activation = [] { on_wayland_readable(); };
    }
    dbgln("xdg-desktop-portal-serenity: clipboard connected to ext-data-control");
    return true;
}

void destroy_source()
{
    if (!s_current_source)
        return;
    if (s_current_source->wl)
        ext_data_control_source_v1_destroy(s_current_source->wl);
    delete s_current_source;
    s_current_source = nullptr;
}

void emit_owner_changed(ByteString const& session_handle)
{
    if (!s_service)
        return;
    Portal::Value::Container mime_types;
    if (s_current_offer) {
        for (auto const& mime : s_current_offer->mime_types)
            mime_types.append(Portal::Value::make_string(mime));
    }
    Portal::Dictionary options;
    options.append(Portal::Value::Entry { "mime_types"sv, Portal::Value::make_variant(Portal::Value::make_array(move(mime_types), "s"sv)) });
    options.append(Portal::Value::Entry { "session_is_owner"sv, Portal::Value::make_variant(Portal::Value::make_bool(s_current_source && s_current_source->session_handle == session_handle)) });
    s_service->emit_signal(clipboard_interface, "SelectionOwnerChanged"sv, {
        Portal::Value::make_object_path(session_handle),
        Portal::Value::make_dict(move(options)),
    });
}

void handle_clipboard(Portal::MethodCall& call)
{
    if (call.member() == "RequestClipboard"sv) {
        auto session = call.object_path_argument(0);
        if (!RemoteDesktop::has_session(session) && !InputCapture::has_session(session)) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "not a RemoteDesktop or InputCapture session"sv);
            return;
        }
        if (!ensure_wayland()) {
            call.reply_error(Portal::ErrorName::NotSupported, "clipboard data-control is unavailable"sv);
            return;
        }
        if (RemoteDesktop::has_session(session))
            RemoteDesktop::set_clipboard_enabled(session, true);
        else
            InputCapture::set_clipboard_enabled(session, true);
        if (!s_enabled_sessions.contains_slow(session))
            s_enabled_sessions.append(session);
        emit_owner_changed(session);
        call.reply();
        return;
    }

    auto session = call.object_path_argument(0);
    if (!session_enabled(session)) {
        call.reply_error(Portal::ErrorName::InvalidArgs, "clipboard was not enabled for this session"sv);
        return;
    }

    if (call.member() == "SetSelection"sv) {
        auto mime_types = mime_types_from_options(call.argument(1));
        destroy_source();
        if (mime_types.is_empty()) {
            if (s_device) {
                ext_data_control_device_v1_set_selection(s_device, nullptr);
                wl_display_flush(s_display);
            }
            call.reply();
            return;
        }
        if (!ensure_wayland()) {
            call.reply_error(Portal::ErrorName::NotSupported, "clipboard data-control is unavailable"sv);
            return;
        }
        auto* source = new Source;
        source->session_handle = session;
        source->wl = ext_data_control_manager_v1_create_data_source(s_manager);
        ext_data_control_source_v1_add_listener(source->wl, &source_listener, source);
        for (auto const& mime : mime_types)
            ext_data_control_source_v1_offer(source->wl, mime.characters());
        ext_data_control_device_v1_set_selection(s_device, source->wl);
        s_current_source = source;
        wl_display_flush(s_display);
        call.reply();
        return;
    }

    if (call.member() == "SelectionRead"sv) {
        auto mime_type = call.string_argument(1);
        if (!s_current_offer || !s_current_offer->mime_types.contains_slow(mime_type)) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "requested MIME type is not offered"sv);
            return;
        }
        int pipe_fds[2] { };
        if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
            call.reply_error(Portal::ErrorName::Failed, "could not create clipboard pipe"sv);
            return;
        }
        ext_data_control_offer_v1_receive(s_current_offer->wl, mime_type.characters(), pipe_fds[1]);
        wl_display_flush(s_display);
        call.reply({ Portal::Value::make_unix_fd(pipe_fds[0]) });
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return;
    }

    if (call.member() == "SelectionWrite"sv) {
        auto serial = call.argument(1) && call.argument(1)->is(Portal::Value::Type::UInt32) ? call.argument(1)->as_uint32() : 0u;
        for (auto const& transfer : s_pending_transfers) {
            if (transfer.serial == serial) {
                call.reply({ Portal::Value::make_unix_fd(transfer.fd) });
                return;
            }
        }
        call.reply_error(Portal::ErrorName::InvalidArgs, "no pending clipboard transfer for this serial"sv);
        return;
    }

    if (call.member() == "SelectionWriteDone"sv) {
        auto serial = call.argument(1) && call.argument(1)->is(Portal::Value::Type::UInt32) ? call.argument(1)->as_uint32() : 0u;
        auto found = s_pending_transfers.find_if([&](auto const& transfer) { return transfer.serial == serial; });
        if (found == s_pending_transfers.end()) {
            call.reply_error(Portal::ErrorName::InvalidArgs, "no pending clipboard transfer for this serial"sv);
            return;
        }
        if (found->fd >= 0)
            close(found->fd);
        s_pending_transfers.remove(found.index());
        call.reply();
        return;
    }

    call.reply_error(Portal::ErrorName::UnknownMethod, "unknown Clipboard method"sv);
}

}

namespace Clipboard {

bool is_enabled(ByteString const& session_handle)
{
    return session_enabled(session_handle);
}

void register_interface(Portal::Service& service)
{
    s_service = &service;
    service.register_interface(
        clipboard_interface.to_byte_string(),
        [](Portal::MethodCall& call) { handle_clipboard(call); },
        [](StringView property) -> Optional<Portal::Value> {
            if (property == "version"sv)
                return Portal::Value::make_uint32(clipboard_version);
            return {};
        },
        Vector<ByteString> { "version"sv },
        Portal::interface_introspection_xml(clipboard_interface).value_or(StringView {}));
}

}
