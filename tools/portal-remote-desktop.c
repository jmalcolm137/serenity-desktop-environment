/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 RemoteDesktop backend for xdg-desktop-portal-serenity.
//
// Input injection uses the compositor's standard virtual input: wlr-virtual-
// pointer/unstable-v1 and virtual-keyboard/unstable-v1. Video is the paired
// ScreenCast session (its stream is returned by Start). ConnectToEIS (libei) is
// not implemented; apps fall back to the Notify* methods.

#define _GNU_SOURCE

#include "portal-remote-desktop.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "portal-screencast.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

// Device bitmask (org.freedesktop.portal.RemoteDesktop.DeviceType).
enum {
    RD_DEVICE_KEYBOARD = 1,
    RD_DEVICE_POINTER = 2,
    RD_DEVICE_TOUCHSCREEN = 4,
};

struct rd_state {
    int connected;
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_seat* seat;
    struct zwlr_virtual_pointer_manager_v1* pointer_manager;
    struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
    struct zwlr_virtual_pointer_v1* pointer;
    struct zwp_virtual_keyboard_v1* keyboard;
    int width;
    int height;
};

static struct rd_state g_rd = { 0 };
static char* g_rd_restore_token = NULL;

static guint32 now_ms(void)
{
    return (guint32)(g_get_monotonic_time() / 1000);
}

static void rd_registry_global(void* data, struct wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(interface, wl_seat_interface.name) == 0)
        g_rd.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        g_rd.pointer_manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        g_rd.keyboard_manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void rd_registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener rd_registry_listener = {
    .global = rd_registry_global,
    .global_remove = rd_registry_global_remove,
};

// Send an xkb keymap to the virtual keyboard (us layout), as the protocol
// requires before key events.
static void rd_send_keymap(void)
{
    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (!keymap) {
        if (context)
            xkb_context_unref(context);
        return;
    }
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (text) {
        size_t size = strlen(text) + 1;
        int fd = memfd_create("sde-vkbd", MFD_CLOEXEC);
        if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
            (void)write(fd, text, size);
            zwp_virtual_keyboard_v1_keymap(g_rd.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
            wl_display_flush(g_rd.display);
        }
        if (fd >= 0)
            close(fd);
        free(text);
    }
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
}

static void rd_ensure(void)
{
    if (g_rd.connected)
        return;
    g_rd.connected = 1;
    g_rd.display = wl_display_connect(NULL);
    if (!g_rd.display) {
        fprintf(stderr, "xdg-desktop-portal-serenity: remote desktop cannot connect to Wayland\n");
        return;
    }
    g_rd.registry = wl_display_get_registry(g_rd.display);
    wl_registry_add_listener(g_rd.registry, &rd_registry_listener, NULL);
    wl_display_roundtrip(g_rd.display);
    wl_display_roundtrip(g_rd.display);

    if (g_rd.pointer_manager)
        g_rd.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_rd.pointer_manager, g_rd.seat);
    if (g_rd.keyboard_manager) {
        g_rd.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g_rd.keyboard_manager, g_rd.seat);
        if (g_rd.keyboard)
            rd_send_keymap();
    }
    wl_display_flush(g_rd.display);
}

// --- D-Bus interface --------------------------------------------------------

const char portal_remote_desktop_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.RemoteDesktop'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SelectDevices'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
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
    "    <method name='NotifyPointerMotion'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='d' name='dx' direction='in'/>"
    "      <arg type='d' name='dy' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerMotionAbsolute'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='stream' direction='in'/>"
    "      <arg type='d' name='x' direction='in'/>"
    "      <arg type='d' name='y' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerButton'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='i' name='button' direction='in'/>"
    "      <arg type='u' name='state' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerAxis'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='d' name='dx' direction='in'/>"
    "      <arg type='d' name='dy' direction='in'/>"
    "    </method>"
    "    <method name='NotifyPointerAxisDiscrete'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='axis' direction='in'/>"
    "      <arg type='i' name='steps' direction='in'/>"
    "    </method>"
    "    <method name='NotifyKeyboardKeycode'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='i' name='keycode' direction='in'/>"
    "      <arg type='u' name='state' direction='in'/>"
    "    </method>"
    "    <method name='NotifyKeyboardKeysym'>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='i' name='keysym' direction='in'/>"
    "      <arg type='u' name='state' direction='in'/>"
    "    </method>"
    "    <method name='ConnectToEIS'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='h' name='fd' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void return_response(GDBusMethodInvocation* invocation, guint32 response, GVariantBuilder* results)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, results));
}

void portal_remote_desktop_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    GVariantBuilder results;

    if (g_strcmp0(method_name, "CreateSession") == 0) {
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "SelectDevices") == 0) {
        char const* session_handle = NULL;
        GVariant* options = NULL;
        g_variant_get(parameters, "(o&o&s@a{sv})", NULL, &session_handle, NULL, &options);
        if (options) {
            // Restore-token round trip (same store as ScreenCast).
            char const* restore = NULL;
            if (g_variant_lookup(options, "restore_token", "&s", &restore) && restore && *restore) {
                guint32 types = 0;
                guint32 cursor_mode = 0;
                char* source_id = NULL;
                if (portal_restore_token_load(restore, &types, &cursor_mode, &source_id)) {
                    free(g_rd_restore_token);
                    g_rd_restore_token = strdup(restore);
                }
                free(source_id);
            }
            guint32 persist_mode = 0;
            if (g_variant_lookup(options, "persist_mode", "u", &persist_mode) && persist_mode != 0 && !g_rd_restore_token) {
                char token[33];
                for (int i = 0; i < 32; ++i)
                    token[i] = "0123456789abcdef"[g_random_int_range(0, 16)];
                token[32] = '\0';
                portal_restore_token_store(token, RD_DEVICE_POINTER | RD_DEVICE_KEYBOARD, 0, NULL);
                g_rd_restore_token = strdup(token);
            }
        }
        if (options)
            g_variant_unref(options);
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "Start") == 0) {
        rd_ensure();
        char const* session_handle = NULL;
        GVariant* options = NULL;
        g_variant_get(parameters, "(o&o&ss@a{sv})", NULL, &session_handle, NULL, NULL, &options);

        guint32 devices = RD_DEVICE_POINTER | RD_DEVICE_KEYBOARD;

        // Return the paired ScreenCast stream if the app provided its session.
        guint32 node = 0;
        int width = 0;
        int height = 0;
        guint32 source_type = 1;
        gboolean have_stream = FALSE;
        if (options) {
            char const* screencast_session = NULL;
            if (g_variant_lookup(options, "session_handle", "&o", &screencast_session) && screencast_session
                && portal_screencast_start_for_session(screencast_session, &node, &width, &height, &source_type) == 0)
                have_stream = TRUE;
        }
        g_rd.width = width;
        g_rd.height = height;

        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&results, "{sv}", "devices", g_variant_new_uint32(devices));
        if (have_stream) {
            GVariantBuilder streams;
            g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "position", g_variant_new("(ii)", 0, 0));
            g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", width, height));
            g_variant_builder_add(&props, "{sv}", "source_type", g_variant_new_uint32(source_type));
            g_variant_builder_add(&streams, "(ua{sv})", node, &props);
            g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
        }
        if (g_rd_restore_token)
            g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(g_rd_restore_token));
        if (options)
            g_variant_unref(options);
        fprintf(stderr, "xdg-desktop-portal-serenity: remote desktop started (devices %u, stream %u)\n", devices, have_stream ? node : 0);
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "NotifyPointerMotion") == 0) {
        double dx = 0, dy = 0;
        g_variant_get(parameters, "(o@a{sv}dd)", NULL, NULL, &dx, &dy);
        if (g_rd.pointer) {
            zwlr_virtual_pointer_v1_motion(g_rd.pointer, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "NotifyPointerMotionAbsolute") == 0) {
        guint32 stream = 0;
        double x = 0, y = 0;
        g_variant_get(parameters, "(o@a{sv}udd)", NULL, NULL, &stream, &x, &y);
        if (g_rd.pointer) {
            guint32 extent_x = g_rd.width > 0 ? (guint32)g_rd.width : 1;
            guint32 extent_y = g_rd.height > 0 ? (guint32)g_rd.height : 1;
            zwlr_virtual_pointer_v1_motion_absolute(g_rd.pointer, now_ms(), (guint32)x, (guint32)y, extent_x, extent_y);
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "NotifyPointerButton") == 0) {
        gint32 button = 0;
        guint32 state = 0;
        g_variant_get(parameters, "(o@a{sv}iu)", NULL, NULL, &button, &state);
        if (g_rd.pointer) {
            zwlr_virtual_pointer_v1_button(g_rd.pointer, now_ms(), (guint32)button, state);
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "NotifyPointerAxis") == 0) {
        double dx = 0, dy = 0;
        g_variant_get(parameters, "(o@a{sv}dd)", NULL, NULL, &dx, &dy);
        if (g_rd.pointer) {
            if (dy != 0.0)
                zwlr_virtual_pointer_v1_axis(g_rd.pointer, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
            if (dx != 0.0)
                zwlr_virtual_pointer_v1_axis(g_rd.pointer, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "NotifyPointerAxisDiscrete") == 0) {
        guint32 axis = 0;
        gint32 steps = 0;
        g_variant_get(parameters, "(o@a{sv}ui)", NULL, NULL, &axis, &steps);
        if (g_rd.pointer) {
            uint32_t wl_axis = axis == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            zwlr_virtual_pointer_v1_axis_discrete(g_rd.pointer, now_ms(), wl_axis, wl_fixed_from_double((double)steps), steps);
            zwlr_virtual_pointer_v1_frame(g_rd.pointer);
            wl_display_flush(g_rd.display);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "NotifyKeyboardKeycode") == 0) {
        gint32 keycode = 0;
        guint32 state = 0;
        g_variant_get(parameters, "(o@a{sv}iu)", NULL, NULL, &keycode, &state);
        if (g_rd.keyboard) {
            zwp_virtual_keyboard_v1_key(g_rd.keyboard, now_ms(), (guint32)keycode, state);
            wl_display_flush(g_rd.display);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "NotifyKeyboardKeysym") == 0) {
        // The virtual keyboard takes evdev keycodes; keysym injection would need
        // an xkb keymap lookup. Accept and ignore for now.
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "ConnectToEIS") == 0) {
        // libei is not used; apps should fall back to the Notify* methods.
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "ConnectToEIS is not supported");
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

GVariant* portal_remote_desktop_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(2);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}
