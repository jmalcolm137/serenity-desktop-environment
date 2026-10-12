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
// The compositor only offers the protocol when SDE_INPUT_CAPTURE=1.

#define _GNU_SOURCE

#include "portal-input-capture.h"

#include <glib-unix.h>
#include <gio/gunixfdlist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "serenity-input-capture-client-protocol.h"

#define IC_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define IC_CAP_KEYBOARD 1u
#define IC_CAP_POINTER 2u

struct ic_session {
    char* handle;
    struct serenity_input_capture_session_v1* wl_session;
    guint32 capabilities;
    GDBusMethodInvocation* pending_eis;
    GDBusMethodInvocation* pending_zones;
    GDBusMethodInvocation* pending_barriers;
    struct ic_session* next;
};

static struct wl_display* g_ic_display;
static struct wl_registry* g_ic_registry;
static struct serenity_input_capture_manager_v1* g_ic_manager;
static guint g_ic_source_id;
static struct ic_session* g_ic_sessions;
static GDBusConnection* g_ic_connection;
static guint32 g_ic_activation_id;

// --- Wayland connection -----------------------------------------------------

static void ic_registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, serenity_input_capture_manager_v1_interface.name) == 0)
        g_ic_manager = wl_registry_bind(registry, name, &serenity_input_capture_manager_v1_interface, 1);
}

static void ic_registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener ic_registry_listener = { ic_registry_global, ic_registry_global_remove };

static gboolean ic_wl_dispatch(gint fd, GIOCondition condition, gpointer data)
{
    (void)fd;
    (void)condition;
    (void)data;
    if (wl_display_dispatch(g_ic_display) < 0)
        return G_SOURCE_REMOVE;
    wl_display_flush(g_ic_display);
    return G_SOURCE_CONTINUE;
}

static int ic_ensure(void)
{
    if (g_ic_display)
        return g_ic_manager != NULL;

    g_ic_display = wl_display_connect(NULL);
    if (!g_ic_display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: input capture cannot connect to Wayland\n");
        return 0;
    }
    g_ic_registry = wl_display_get_registry(g_ic_display);
    wl_registry_add_listener(g_ic_registry, &ic_registry_listener, NULL);
    wl_display_roundtrip(g_ic_display);
    wl_display_roundtrip(g_ic_display);

    if (!g_ic_manager) {
        fprintf(stderr, "xdg-desktop-portal-serenity: compositor does not offer serenity_input_capture (SDE_INPUT_CAPTURE=1?)\n");
        return 0;
    }
    g_ic_source_id = g_unix_fd_add(wl_display_get_fd(g_ic_display), G_IO_IN, ic_wl_dispatch, NULL);
    fprintf(stderr, "xdg-desktop-portal-serenity: input capture connected to the compositor EIS relay\n");
    return 1;
}

// --- sessions ---------------------------------------------------------------

static struct ic_session* ic_session_find(char const* handle)
{
    for (struct ic_session* s = g_ic_sessions; s; s = s->next) {
        if (g_strcmp0(s->handle, handle) == 0)
            return s;
    }
    return NULL;
}

static void ic_return_response(GDBusMethodInvocation* invocation, guint32 response, GVariantBuilder* results)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, results));
}

static void ic_emit_signal(char const* signal, char const* handle, GVariant* options)
{
    if (!g_ic_connection)
        return;
    g_dbus_connection_emit_signal(g_ic_connection, NULL, IC_OBJECT_PATH,
        "org.freedesktop.impl.portal.InputCapture", signal,
        g_variant_new("(o@a{sv})", handle, options), NULL);
}

static void ic_wl_eis_fd(void* data, struct serenity_input_capture_session_v1* session, int32_t fd)
{
    (void)session;
    struct ic_session* s = data;
    if (s->pending_eis) {
        GUnixFDList* list = g_unix_fd_list_new();
        gint index = g_unix_fd_list_append(list, fd, NULL);
        g_dbus_method_invocation_return_value_with_unix_fd_list(s->pending_eis, g_variant_new("(h)", index), list);
        g_object_unref(list);
        s->pending_eis = NULL;
    }
    close(fd);
}

static void ic_wl_zones(void* data, struct serenity_input_capture_session_v1* session, struct wl_array* zones, uint32_t zone_set)
{
    (void)session;
    struct ic_session* s = data;
    if (!s->pending_zones)
        return;

    GVariantBuilder zb;
    g_variant_builder_init(&zb, G_VARIANT_TYPE("a(uuii)"));
    size_t const count = zones->size / (4 * sizeof(int32_t));
    int32_t const* raw = zones->data;
    for (size_t i = 0; i < count; ++i)
        g_variant_builder_add(&zb, "(uuii)", (guint32)raw[i * 4], (guint32)raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3]);

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&results, "{sv}", "zones", g_variant_builder_end(&zb));
    g_variant_builder_add(&results, "{sv}", "zone_set", g_variant_new_uint32(zone_set));
    ic_return_response(s->pending_zones, 0, &results);
    s->pending_zones = NULL;
}

static void ic_wl_barriers_result(void* data, struct serenity_input_capture_session_v1* session, struct wl_array* failed)
{
    (void)session;
    struct ic_session* s = data;
    if (!s->pending_barriers)
        return;

    GVariantBuilder fb;
    g_variant_builder_init(&fb, G_VARIANT_TYPE("au"));
    size_t const count = failed->size / sizeof(uint32_t);
    uint32_t const* raw = failed->data;
    for (size_t i = 0; i < count; ++i)
        g_variant_builder_add(&fb, "u", raw[i]);

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&results, "{sv}", "failed_barriers", g_variant_builder_end(&fb));
    ic_return_response(s->pending_barriers, 0, &results);
    s->pending_barriers = NULL;
}

static void ic_wl_zones_changed(void* data, struct serenity_input_capture_session_v1* session)
{
    (void)session;
    struct ic_session* s = data;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    ic_emit_signal("ZonesChanged", s->handle, g_variant_builder_end(&options));
}

static void ic_wl_activated(void* data, struct serenity_input_capture_session_v1* session, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y, uint32_t barrier_id)
{
    (void)session;
    struct ic_session* s = data;
    g_ic_activation_id = activation_id;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "activation_id", g_variant_new_uint32(activation_id));
    g_variant_builder_add(&options, "{sv}", "cursor_position", g_variant_new("(dd)", wl_fixed_to_double(cursor_x), wl_fixed_to_double(cursor_y)));
    g_variant_builder_add(&options, "{sv}", "barrier_id", g_variant_new_uint32(barrier_id));
    ic_emit_signal("Activated", s->handle, g_variant_builder_end(&options));
}

static void ic_wl_deactivated(void* data, struct serenity_input_capture_session_v1* session, uint32_t activation_id, wl_fixed_t cursor_x, wl_fixed_t cursor_y)
{
    (void)session;
    struct ic_session* s = data;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "activation_id", g_variant_new_uint32(activation_id));
    g_variant_builder_add(&options, "{sv}", "cursor_position", g_variant_new("(dd)", wl_fixed_to_double(cursor_x), wl_fixed_to_double(cursor_y)));
    ic_emit_signal("Deactivated", s->handle, g_variant_builder_end(&options));
}

static void ic_wl_disabled(void* data, struct serenity_input_capture_session_v1* session)
{
    (void)session;
    struct ic_session* s = data;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    ic_emit_signal("Disabled", s->handle, g_variant_builder_end(&options));
}

static const struct serenity_input_capture_session_v1_listener ic_wl_session_listener = {
    .eis_fd = ic_wl_eis_fd,
    .zones = ic_wl_zones,
    .zones_changed = ic_wl_zones_changed,
    .pointer_barriers_result = ic_wl_barriers_result,
    .activated = ic_wl_activated,
    .deactivated = ic_wl_deactivated,
    .disabled = ic_wl_disabled,
};

static struct ic_session* ic_session_create(char const* handle)
{
    struct ic_session* existing = ic_session_find(handle);
    if (existing)
        return existing;

    struct ic_session* s = calloc(1, sizeof(*s));
    s->handle = strdup(handle);
    s->wl_session = serenity_input_capture_manager_v1_create_session(g_ic_manager, "xdg-desktop-portal-serenity");
    serenity_input_capture_session_v1_add_listener(s->wl_session, &ic_wl_session_listener, s);
    s->next = g_ic_sessions;
    g_ic_sessions = s;
    wl_display_flush(g_ic_display);
    return s;
}

// --- the permission prompt --------------------------------------------------

static guint32 ic_prompt(void)
{
    char const* dialog = getenv("SDE_PORTAL_ACCESS_DIALOG");
    if (!dialog || !*dialog)
        return 0; // no prompt configured; the compositor's SDE_INPUT_CAPTURE gate is the boundary

    char const* argv[] = { dialog, "-t", "Allow input capture?", "-s", "", "-b", "An application wants to capture your input.", NULL };
    char* out = NULL;
    gint wait_status = 0;
    GError* error = NULL;
    guint32 response = 1;
    if (g_spawn_sync(NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error)
        && out && g_spawn_check_wait_status(wait_status, NULL))
        response = (guint32)MAX(0, atoi(g_strstrip(out)));
    if (out)
        g_free(out);
    if (error)
        g_error_free(error);
    return response;
}

// --- D-Bus interface --------------------------------------------------------

const char portal_input_capture_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.InputCapture'>"
    "    <property name='version' type='u' access='read'/>"
    "    <property name='SupportedCapabilities' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='CreateSession2'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Start'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='GetZones'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SetPointerBarriers'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='aa{sv}' name='barriers' direction='in'/>"
    "      <arg type='u' name='zone_set' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Enable'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Disable'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Release'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='ConnectToEIS'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "    <signal name='Disabled'>"
    "      <arg type='o' name='session_handle' direction='out'/>"
    "      <arg type='a{sv}' name='options' direction='out'/>"
    "    </signal>"
    "    <signal name='Activated'>"
    "      <arg type='o' name='session_handle' direction='out'/>"
    "      <arg type='a{sv}' name='options' direction='out'/>"
    "    </signal>"
    "    <signal name='Deactivated'>"
    "      <arg type='o' name='session_handle' direction='out'/>"
    "      <arg type='a{sv}' name='options' direction='out'/>"
    "    </signal>"
    "    <signal name='ZonesChanged'>"
    "      <arg type='o' name='session_handle' direction='out'/>"
    "      <arg type='a{sv}' name='options' direction='out'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

void portal_input_capture_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    g_ic_connection = connection;

    if (!ic_ensure()) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
            "input capture is unavailable (compositor has no EIS relay; set SDE_INPUT_CAPTURE=1)");
        return;
    }

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));

    if (g_strcmp0(method_name, "CreateSession") == 0 || g_strcmp0(method_name, "CreateSession2") == 0) {
        char const* session_handle = NULL;
        if (g_strcmp0(method_name, "CreateSession") == 0)
            g_variant_get(parameters, "(o&s&s@a{sv})", &session_handle, NULL, NULL, NULL);
        else
            g_variant_get(parameters, "(o&s@a{sv})", &session_handle, NULL, NULL);
        if (!session_handle) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "missing session handle");
            return;
        }
        ic_session_create(session_handle);
        if (g_strcmp0(method_name, "CreateSession") == 0)
            ic_return_response(invocation, 0, &results);
        else
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &results));
        return;
    }

    if (g_strcmp0(method_name, "Start") == 0) {
        char const* session_handle = NULL;
        GVariant* options = NULL;
        g_variant_get(parameters, "(o&o&ss@a{sv})", NULL, &session_handle, NULL, NULL, &options);
        struct ic_session* s = session_handle ? ic_session_find(session_handle) : NULL;
        if (!s) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown session");
            return;
        }
        guint32 requested = 0;
        if (options)
            g_variant_lookup(options, "capabilities", "u", &requested);
        guint32 granted = requested & (IC_CAP_KEYBOARD | IC_CAP_POINTER);
        guint32 const response = ic_prompt();
        if (response != 0) {
            if (options)
                g_variant_unref(options);
            ic_return_response(invocation, response, &results);
            return;
        }
        s->capabilities = granted;
        g_variant_builder_add(&results, "{sv}", "capabilities", g_variant_new_uint32(granted));
        if (options)
            g_variant_unref(options);
        fprintf(stderr, "xdg-desktop-portal-serenity: input capture session started (caps %u)\n", granted);
        ic_return_response(invocation, 0, &results);
        return;
    }

    // Remaining methods operate on an existing session.
    char const* session_handle = NULL;
    GVariant* options = NULL;
    GVariant* barriers = NULL;
    guint32 zone_set = 0;
    if (g_strcmp0(method_name, "GetZones") == 0)
        g_variant_get(parameters, "(o&o&s@a{sv})", NULL, &session_handle, NULL, &options);
    else if (g_strcmp0(method_name, "SetPointerBarriers") == 0)
        g_variant_get(parameters, "(o&o&s@a{sv}@aa{sv}u)", NULL, &session_handle, NULL, &options, &barriers, &zone_set);
    else
        g_variant_get(parameters, "(o&s@a{sv})", &session_handle, NULL, &options);

    struct ic_session* s = session_handle ? ic_session_find(session_handle) : NULL;
    if (!s) {
        if (options)
            g_variant_unref(options);
        if (barriers)
            g_variant_unref(barriers);
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown session");
        return;
    }

    if (g_strcmp0(method_name, "GetZones") == 0) {
        if (options)
            g_variant_unref(options);
        s->pending_zones = invocation;
        serenity_input_capture_session_v1_get_zones(s->wl_session);
        wl_display_flush(g_ic_display);
        return;
    }

    if (g_strcmp0(method_name, "SetPointerBarriers") == 0) {
        struct wl_array packed;
        wl_array_init(&packed);
        GVariantIter iter;
        GVariant* entry = NULL;
        g_variant_iter_init(&iter, barriers);
        while ((entry = g_variant_iter_next_value(&iter))) {
            guint32 id = 0;
            gint x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            g_variant_lookup(entry, "barrier_id", "u", &id);
            g_variant_lookup(entry, "position", "(iiii)", &x1, &y1, &x2, &y2);
            g_variant_unref(entry);
            struct { guint32 id; gint32 x1, y1, x2, y2; } b = { id, x1, y1, x2, y2 };
            memcpy(wl_array_add(&packed, sizeof(b)), &b, sizeof(b));
        }
        if (options)
            g_variant_unref(options);
        g_variant_unref(barriers);

        s->pending_barriers = invocation;
        serenity_input_capture_session_v1_set_pointer_barriers(s->wl_session, &packed, zone_set);
        wl_array_release(&packed);
        wl_display_flush(g_ic_display);
        return;
    }

    if (g_strcmp0(method_name, "Enable") == 0) {
        if (options)
            g_variant_unref(options);
        serenity_input_capture_session_v1_enable(s->wl_session, s->capabilities);
        wl_display_flush(g_ic_display);
        ic_return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "Disable") == 0) {
        if (options)
            g_variant_unref(options);
        serenity_input_capture_session_v1_disable(s->wl_session);
        wl_display_flush(g_ic_display);
        ic_return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "Release") == 0) {
        guint32 activation_id = g_ic_activation_id;
        double cx = 0, cy = 0;
        if (options) {
            g_variant_lookup(options, "activation_id", "u", &activation_id);
            g_variant_lookup(options, "cursor_position", "(dd)", &cx, &cy);
            g_variant_unref(options);
        }
        serenity_input_capture_session_v1_release(s->wl_session, activation_id, wl_fixed_from_double(cx), wl_fixed_from_double(cy));
        wl_display_flush(g_ic_display);
        ic_return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "ConnectToEIS") == 0) {
        if (options)
            g_variant_unref(options);
        if (s->pending_eis) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "ConnectToEIS already pending");
            return;
        }
        s->pending_eis = invocation;
        serenity_input_capture_session_v1_connect_to_eis(s->wl_session);
        wl_display_flush(g_ic_display);
        return;
    }

    if (options)
        g_variant_unref(options);
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

GVariant* portal_input_capture_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(2);
    if (g_strcmp0(property_name, "SupportedCapabilities") == 0)
        return g_variant_new_uint32(IC_CAP_KEYBOARD | IC_CAP_POINTER);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}
