/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "InputCapture.h"

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>

#ifdef SDE_HAVE_LIBEIS

#include <AK/StdLibExtras.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Model.h"

extern "C" {
#include <wayland-server-core.h>
#include <libeis.h>
#include "serenity-input-capture-server-protocol.h"
}

namespace InputCapture {

struct Session {
    Server* server { nullptr };
    wl_resource* resource { nullptr };
    ByteString app_id;
};

// The single EIS implementation for the compositor. Created lazily on the first
// connect_to_eis; its fd is registered with the Core::EventLoop and pumped from
// a Core::Notifier (the compositor's loop model: Wayland events are pumped the
// same way, see docs/08 §6).
static struct eis* s_eis { nullptr };
static RefPtr<Core::Notifier> s_eis_notifier;
static Vector<NonnullOwnPtr<Session>> s_sessions;

static void pump_eis()
{
    if (!s_eis)
        return;

    eis_dispatch(s_eis);

    struct eis_event* event = nullptr;
    while ((event = eis_get_event(s_eis))) {
        auto type = eis_event_get_type(event);
        switch (type) {
        case EIS_EVENT_CLIENT_CONNECT: {
            auto* client = eis_event_get_client(event);
            eis_client_connect(client);
            dbgln("WindowServer: InputCapture: EIS client connected");
            break;
        }
        case EIS_EVENT_CLIENT_DISCONNECT:
            dbgln("WindowServer: InputCapture: EIS client disconnected");
            break;
        default:
            break;
        }
        eis_event_unref(event);
    }
}

// Create the EIS implementation and wire its fd into the compositor event loop.
// Idempotent; returns false if libeis could not be set up.
static bool ensure_eis()
{
    if (s_eis)
        return true;

    s_eis = eis_new(nullptr);
    if (!s_eis) {
        warnln("WindowServer: InputCapture: could not create the EIS implementation");
        return false;
    }

    if (eis_setup_backend_fd(s_eis) < 0) {
        warnln("WindowServer: InputCapture: eis_setup_backend_fd failed: {}", strerror(errno));
        eis_unref(s_eis);
        s_eis = nullptr;
        return false;
    }

    int fd = eis_get_fd(s_eis);
    s_eis_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    s_eis_notifier->on_activation = [] {
        pump_eis();
    };

    dbgln("WindowServer: InputCapture: EIS implementation ready (fd {})", fd);
    return true;
}

static void session_resource_destroy(wl_resource* resource)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;
    s_sessions.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == session; });
}

static void session_connect_to_eis(wl_client*, wl_resource* resource)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;

    if (!ensure_eis()) {
        wl_resource_post_error(resource, 0, "input capture is unavailable (EIS setup failed)");
        return;
    }

    int fd = eis_backend_fd_add_client(s_eis);
    if (fd < 0) {
        wl_resource_post_error(resource, 0, "could not create an EIS client connection");
        return;
    }

    serenity_input_capture_session_v1_send_eis_fd(resource, fd);
    // send_eis_fd() queued the fd (duped) into the message; drop our copy.
    close(fd);
    dbgln("WindowServer: InputCapture: handed an EIS fd to app '{}'", session->app_id);
}

static void session_destroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct serenity_input_capture_session_v1_interface s_session_impl {
    .connect_to_eis = session_connect_to_eis,
    .destroy = session_destroy,
};

static void manager_create_session(wl_client* client, wl_resource* resource, char const* app_id, uint32_t id)
{
    auto* server = static_cast<Server*>(wl_resource_get_user_data(resource));

    auto* session_resource = wl_resource_create(client, &serenity_input_capture_session_v1_interface, 1, id);
    if (!session_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    auto session = make<Session>();
    session->server = server;
    session->resource = session_resource;
    session->app_id = app_id ? ByteString { app_id } : ByteString {};
    wl_resource_set_implementation(session_resource, &s_session_impl, session.ptr(), session_resource_destroy);
    s_sessions.append(move(session));
}

static const struct serenity_input_capture_manager_v1_interface s_manager_impl {
    .create_session = manager_create_session,
};

static void manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto* resource = wl_resource_create(client, &serenity_input_capture_manager_v1_interface, version < 1 ? version : 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &s_manager_impl, data, nullptr);
}

void initialize(Server& server)
{
    // Privileged and off by default: the session enables it only when the SDE
    // portal backend is the trusted client (docs/17 §6).
    auto const* enabled = getenv("SDE_INPUT_CAPTURE");
    if (!enabled || (*enabled == '\0') || ByteString { enabled } == "0"sv)
        return;

    if (!wl_global_create(server.display, &serenity_input_capture_manager_v1_interface, 1, &server, manager_bind)) {
        warnln("WindowServer: InputCapture: could not create the manager global");
        return;
    }
    dbgln("WindowServer: InputCapture: enabled (serenity_input_capture global offered)");
}

}

#else // !SDE_HAVE_LIBEIS

namespace InputCapture {

void initialize(Server&)
{
}

}

#endif // SDE_HAVE_LIBEIS
