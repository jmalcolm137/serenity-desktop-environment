/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "InputCapture.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "Config.h"
#include "Server.h"

extern "C" {
#include <wayland-server-core.h>
#include <libeis.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <xkbcommon/xkbcommon.h>
#include "serenity-input-capture-server-protocol.h"
}

// InputCapture capability bits (match the portal's SupportedCapabilities).
#define IC_CAP_KEYBOARD 1u
#define IC_CAP_POINTER 2u

namespace InputCapture {

// Wire structs: the protocol packs plain arrays (docs/17 §5).
struct WireZone {
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
};

struct WireBarrier {
    uint32_t id;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

struct Barrier {
    uint32_t id { 0 };
    int32_t x1 { 0 };
    int32_t y1 { 0 };
    int32_t x2 { 0 };
    int32_t y2 { 0 };
};

struct Session {
    Server* server { nullptr };
    wl_resource* resource { nullptr };
    ByteString app_id;
    Vector<Barrier> barriers;
    bool enabled { false };
    bool active { false };
    uint32_t capabilities { 0 };
    uint32_t zone_set_seen { 0 };
    uint32_t activation_id { 0 };
};

// The single EIS implementation for the compositor, created lazily on the first
// connect_to_eis; its fd is pumped from a Core::Notifier (the compositor's loop
// model, docs/08 §6). P2 assumes a single capture client (the portal backend).
static struct eis* s_eis { nullptr };
static RefPtr<Core::Notifier> s_eis_notifier;
static Vector<NonnullOwnPtr<Session>> s_sessions;
static struct eis_seat* s_eis_seat { nullptr };
static struct eis_device* s_eis_pointer { nullptr };
static struct eis_device* s_eis_keyboard { nullptr };
static Session* s_active_session { nullptr };
static uint32_t s_next_activation_id { 0 };
static uint32_t s_zone_set { 1 };
static double s_last_x { 0 };
static double s_last_y { 0 };
static bool s_have_last { false };

static uint64_t now_usec()
{
    timespec ts { };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

// --- zones ----------------------------------------------------------------

static void build_zones(Server& server, wl_array& zones)
{
    for (auto& output : server.outputs) {
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output->output, &box);
        WireZone zone { static_cast<uint32_t>(box.width), static_cast<uint32_t>(box.height), box.x, box.y };
        if (auto* slot = wl_array_add(&zones, sizeof(zone)))
            memcpy(slot, &zone, sizeof(zone));
    }
}

static void send_zones(Session& session)
{
    wl_array zones;
    wl_array_init(&zones);
    build_zones(*session.server, zones);
    session.zone_set_seen = s_zone_set;
    serenity_input_capture_session_v1_send_zones(session.resource, &zones, s_zone_set);
    wl_array_release(&zones);
}

// --- activation -----------------------------------------------------------

static void deactivate(Session& session, double cursor_x, double cursor_y)
{
    if (!session.active)
        return;
    if (s_eis_pointer)
        eis_device_stop_emulating(s_eis_pointer);
    if (s_eis_keyboard)
        eis_device_stop_emulating(s_eis_keyboard);
    session.active = false;
    if (s_active_session == &session)
        s_active_session = nullptr;
    serenity_input_capture_session_v1_send_deactivated(session.resource, session.activation_id,
        wl_fixed_from_double(cursor_x), wl_fixed_from_double(cursor_y));
    dbgln("WindowServer: InputCapture: deactivated (session '{}')", session.app_id);
}

static void activate(Session& session, uint32_t barrier_id, double cursor_x, double cursor_y)
{
    if (session.active)
        return;
    session.activation_id = ++s_next_activation_id;
    session.active = true;
    s_active_session = &session;
    if ((session.capabilities & IC_CAP_POINTER) && s_eis_pointer)
        eis_device_start_emulating(s_eis_pointer, session.activation_id);
    if ((session.capabilities & IC_CAP_KEYBOARD) && s_eis_keyboard)
        eis_device_start_emulating(s_eis_keyboard, session.activation_id);
    serenity_input_capture_session_v1_send_activated(session.resource, session.activation_id,
        wl_fixed_from_double(cursor_x), wl_fixed_from_double(cursor_y), barrier_id);
    dbgln("WindowServer: InputCapture: activated (session '{}', barrier {}, activation_id {}, caps {})",
        session.app_id, barrier_id, session.activation_id, session.capabilities);
}

// Does the segment (px,py)->(cx,cy) cross this axis-aligned barrier?
static bool crosses(Barrier const& b, double px, double py, double cx, double cy)
{
    if (b.x1 == b.x2) { // vertical
        double const x = b.x1;
        if (px == cx || (px - x) * (cx - x) > 0)
            return false;
        double const t = (x - px) / (cx - px);
        double const y = py + t * (cy - py);
        return y >= min(b.y1, b.y2) && y <= max(b.y1, b.y2);
    }
    if (b.y1 == b.y2) { // horizontal
        double const y = b.y1;
        if (py == cy || (py - y) * (cy - y) > 0)
            return false;
        double const t = (y - py) / (cy - py);
        double const x = px + t * (cx - px);
        return x >= min(b.x1, b.x2) && x <= max(b.x1, b.x2);
    }
    return false;
}

// --- EIS ------------------------------------------------------------------

static void ensure_pointer_device(struct eis_seat* seat)
{
    if (s_eis_pointer || !seat)
        return;
    auto* device = eis_seat_new_device(seat);
    if (!device) {
        warnln("WindowServer: InputCapture: could not create the EIS pointer device");
        return;
    }
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "SDE input capture pointer");
    eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
    eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
    eis_device_add(device);
    eis_device_resume(device);
    s_eis_pointer = device;
    dbgln("WindowServer: InputCapture: EIS pointer device ready");
}

// Build an XKB keymap (the configured layout) into a memfd for the EIS keyboard
// device. Returns -1 on failure.
static int make_keymap_fd(size_t* size_out)
{
    auto const& settings = Config::input().keyboard;
    auto model = settings.model;
    auto layout = settings.layouts;
    auto variant = settings.variant;
    auto options = settings.options;
    xkb_rule_names names {
        .rules = nullptr,
        .model = model.is_empty() ? nullptr : model.characters(),
        .layout = layout.is_empty() ? nullptr : layout.characters(),
        .variant = variant.is_empty() ? nullptr : variant.characters(),
        .options = options.is_empty() ? nullptr : options.characters(),
    };
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap)
        keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        xkb_context_unref(context);
        return -1;
    }
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    if (!text)
        return -1;

    size_t const length = strlen(text) + 1;
    int fd = memfd_create("sde-eis-keymap", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)length) < 0) {
        if (fd >= 0)
            close(fd);
        free(text);
        return -1;
    }
    void* mapping = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        free(text);
        return -1;
    }
    memcpy(mapping, text, length);
    munmap(mapping, length);
    free(text);
    *size_out = length;
    return fd;
}

static void ensure_keyboard_device(struct eis_seat* seat)
{
    if (s_eis_keyboard || !seat)
        return;
    auto* device = eis_seat_new_device(seat);
    if (!device) {
        warnln("WindowServer: InputCapture: could not create the EIS keyboard device");
        return;
    }
    eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
    eis_device_configure_name(device, "SDE input capture keyboard");
    eis_device_configure_capability(device, EIS_DEVICE_CAP_KEYBOARD);

    size_t keymap_size = 0;
    int fd = make_keymap_fd(&keymap_size);
    if (fd >= 0) {
        if (auto* keymap = eis_device_new_keymap(device, EIS_KEYMAP_TYPE_XKB, fd, keymap_size)) {
            eis_keymap_add(keymap);
            eis_keymap_unref(keymap);
        } else {
            warnln("WindowServer: InputCapture: could not attach the EIS keyboard keymap");
        }
        close(fd);
    } else {
        warnln("WindowServer: InputCapture: no keymap for the EIS keyboard (client must guess)");
    }

    eis_device_add(device);
    eis_device_resume(device);
    s_eis_keyboard = device;
    dbgln("WindowServer: InputCapture: EIS keyboard device ready");
}

static void pump_eis()
{
    if (!s_eis)
        return;

    eis_dispatch(s_eis);

    struct eis_event* event = nullptr;
    while ((event = eis_get_event(s_eis))) {
        switch (eis_event_get_type(event)) {
        case EIS_EVENT_CLIENT_CONNECT: {
            auto* client = eis_event_get_client(event);
            eis_client_connect(client);
            auto* seat = eis_client_new_seat(client, "SDE input capture");
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
            eis_seat_add(seat);
            s_eis_seat = seat;
            dbgln("WindowServer: InputCapture: EIS client connected");
            break;
        }
        case EIS_EVENT_SEAT_BIND:
        case EIS_EVENT_SEAT_DEVICE_REQUESTED: {
            auto* seat = eis_event_get_seat(event);
            if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER))
                ensure_pointer_device(seat);
            if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD))
                ensure_keyboard_device(seat);
            break;
        }
        case EIS_EVENT_CLIENT_DISCONNECT: {
            if (s_active_session)
                deactivate(*s_active_session, s_last_x, s_last_y);
            if (s_eis_pointer) {
                eis_device_unref(s_eis_pointer);
                s_eis_pointer = nullptr;
            }
            if (s_eis_keyboard) {
                eis_device_unref(s_eis_keyboard);
                s_eis_keyboard = nullptr;
            }
            if (s_eis_seat) {
                eis_seat_unref(s_eis_seat);
                s_eis_seat = nullptr;
            }
            dbgln("WindowServer: InputCapture: EIS client disconnected");
            break;
        }
        default:
            break;
        }
        eis_event_unref(event);
    }
}

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

// --- pointer hooks (called from the compositor's pointer path) ------------

static Session* enabled_session()
{
    for (auto& session : s_sessions) {
        if (session->enabled)
            return session.ptr();
    }
    return nullptr;
}

bool handle_pointer_motion(Server& server, uint32_t)
{
    double const x = server.cursor->x;
    double const y = server.cursor->y;

    if (s_active_session) {
        bool const captured = (s_active_session->capabilities & IC_CAP_POINTER) != 0;
        if (s_eis_pointer && captured && s_have_last) {
            eis_device_pointer_motion(s_eis_pointer, x - s_last_x, y - s_last_y);
            eis_device_frame(s_eis_pointer, now_usec());
        }
        s_last_x = x;
        s_last_y = y;
        s_have_last = true;
        return captured;
    }

    if (auto* session = enabled_session(); session && s_have_last) {
        for (auto const& barrier : session->barriers) {
            if (!crosses(barrier, s_last_x, s_last_y, x, y))
                continue;
            activate(*session, barrier.id, x, y);
            bool const captured = (session->capabilities & IC_CAP_POINTER) != 0;
            if (s_eis_pointer && captured) {
                eis_device_pointer_motion(s_eis_pointer, x - s_last_x, y - s_last_y);
                eis_device_frame(s_eis_pointer, now_usec());
            }
            s_last_x = x;
            s_last_y = y;
            return captured;
        }
    }

    s_last_x = x;
    s_last_y = y;
    s_have_last = true;
    return false;
}

bool handle_pointer_button(Server& server, uint32_t button, uint32_t state, uint32_t)
{
    (void)server;
    if (!s_active_session || !(s_active_session->capabilities & IC_CAP_POINTER) || !s_eis_pointer)
        return false;
    eis_device_button_button(s_eis_pointer, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
    eis_device_frame(s_eis_pointer, now_usec());
    return true;
}

bool handle_keyboard_key(Server& server, uint32_t keycode, bool is_press,
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)server;
    if (!s_active_session || !(s_active_session->capabilities & IC_CAP_KEYBOARD) || !s_eis_keyboard)
        return false;
    eis_device_keyboard_key(s_eis_keyboard, keycode, is_press);
    eis_device_frame(s_eis_keyboard, now_usec());
    // Per libeis, send the modifier state right after the frame for a receiver.
    eis_device_keyboard_send_xkb_modifiers(s_eis_keyboard, depressed, latched, locked, group);
    return true;
}

void on_outputs_changed(Server&)
{
    // Zones changed: an active capture's zones/barriers are stale, so stop and
    // disarm every session; the app re-requests zones and re-arms (docs/17 P5).
    if (s_active_session)
        deactivate(*s_active_session, s_last_x, s_last_y);
    for (auto& session : s_sessions)
        session->enabled = false;
    s_zone_set++;
    for (auto& session : s_sessions)
        serenity_input_capture_session_v1_send_zones_changed(session->resource);
}

// --- protocol -------------------------------------------------------------

static void session_resource_destroy(wl_resource* resource)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;
    if (s_active_session == session)
        deactivate(*session, s_last_x, s_last_y);
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
    close(fd);
    dbgln("WindowServer: InputCapture: handed an EIS fd to app '{}'", session->app_id);
}

static void session_get_zones(wl_client*, wl_resource* resource)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (session)
        send_zones(*session);
}

static void session_set_pointer_barriers(wl_client*, wl_resource* resource, struct wl_array* barriers, uint32_t zone_set)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;

    session->barriers.clear();
    wl_array failed;
    wl_array_init(&failed);

    size_t const count = barriers->size / sizeof(WireBarrier);
    auto const* wire = static_cast<WireBarrier const*>(barriers->data);
    for (size_t i = 0; i < count; ++i) {
        auto const& b = wire[i];
        // A barrier is a non-empty axis-aligned line segment: exactly one of the
        // axes is degenerate (vertical or horizontal); diagonal is rejected.
        bool const vertical = b.x1 == b.x2 && b.y1 != b.y2;
        bool const horizontal = b.y1 == b.y2 && b.x1 != b.x2;
        if (b.id != 0 && (vertical || horizontal)) {
            session->barriers.append({ b.id, b.x1, b.y1, b.x2, b.y2 });
        } else {
            if (auto* slot = wl_array_add(&failed, sizeof(uint32_t)))
                *static_cast<uint32_t*>(slot) = b.id;
        }
    }

    // Setting barriers suspends any active capture until enable() again.
    if (session->active)
        deactivate(*session, s_last_x, s_last_y);
    session->enabled = false;
    session->zone_set_seen = zone_set;

    serenity_input_capture_session_v1_send_pointer_barriers_result(resource, &failed);
    wl_array_release(&failed);
    dbgln("WindowServer: InputCapture: session '{}' has {} barrier(s), {} rejected",
        session->app_id, session->barriers.size(), count - session->barriers.size());
}

static void session_enable(wl_client*, wl_resource* resource, uint32_t capabilities)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;
    session->capabilities = capabilities & (IC_CAP_KEYBOARD | IC_CAP_POINTER);
    session->enabled = true;
    s_have_last = false; // seed from the next motion to avoid a spurious crossing
    dbgln("WindowServer: InputCapture: session '{}' armed (caps {})", session->app_id, session->capabilities);
}

static void session_disable(wl_client*, wl_resource* resource)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session)
        return;
    if (session->active)
        deactivate(*session, s_last_x, s_last_y);
    session->enabled = false;
    serenity_input_capture_session_v1_send_disabled(resource);
    dbgln("WindowServer: InputCapture: session '{}' disabled", session->app_id);
}

static void session_release(wl_client*, wl_resource* resource, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y)
{
    auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
    if (!session || !session->active || session->activation_id != activation_id)
        return;
    deactivate(*session, wl_fixed_to_double(cursor_x), wl_fixed_to_double(cursor_y));
}

static void session_destroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct serenity_input_capture_session_v1_interface s_session_impl {
    .connect_to_eis = session_connect_to_eis,
    .get_zones = session_get_zones,
    .set_pointer_barriers = session_set_pointer_barriers,
    .enable = session_enable,
    .disable = session_disable,
    .release = session_release,
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

bool handle_pointer_motion(Server&, uint32_t)
{
    return false;
}

bool handle_pointer_button(Server&, uint32_t, uint32_t, uint32_t)
{
    return false;
}

bool handle_keyboard_key(Server&, uint32_t, bool, uint32_t, uint32_t, uint32_t, uint32_t)
{
    return false;
}

void on_outputs_changed(Server&)
{
}

}

#endif // SDE_HAVE_LIBEIS
