/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// R5 GlobalShortcuts backend for xdg-desktop-portal-serenity.
//
// Shortcuts requested by apps are written to the SDE config system
// (global-shortcuts.ini in the state directory) where the compositor binds them
// (see WindowServer/src/Config.cpp). When one fires, the compositor reports it on
// the session FIFO ($XDG_RUNTIME_DIR/sde-global-shortcuts.fifo) and this backend
// emits the Activated signal.

#define _GNU_SOURCE

#include "portal-global-shortcuts.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char portal_global_shortcuts_interface_name[] = "org.freedesktop.impl.portal.GlobalShortcuts";

struct gs_shortcut {
    char* session;
    char* id;
    char* trigger;
    char* description;
};

static GPtrArray* g_shortcuts = NULL; // struct gs_shortcut*
static GDBusConnection* g_connection = NULL;
static GIOChannel* g_fifo_channel = NULL;
static guint g_fifo_watch = 0;

const char portal_global_shortcuts_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.GlobalShortcuts'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='CreateSession'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='BindShortcuts'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a(sa{sv})' name='shortcuts' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='ListShortcuts'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <signal name='Activated'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='s' name='shortcut_id'/>"
    "      <arg type='t' name='timestamp'/>"
    "      <arg type='a{sv}' name='options'/>"
    "    </signal>"
    "    <signal name='Deactivated'>"
    "      <arg type='o' name='session_handle'/>"
    "      <arg type='s' name='shortcut_id'/>"
    "      <arg type='t' name='timestamp'/>"
    "      <arg type='a{sv}' name='options'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

// The trigger format is "CTRL+ALT+t"; normalise the modifier names to the ones
// the compositor's combo parser understands (Ctrl/Alt/Shift/Super).
static gchar* normalize_trigger(char const* trigger)
{
    gchar** parts = g_strsplit(trigger, "+", -1);
    GString* out = g_string_new(NULL);
    for (int i = 0; parts[i]; ++i) {
        char const* part = parts[i];
        char const* mapped = part;
        if (g_ascii_strcasecmp(part, "CTRL") == 0 || g_ascii_strcasecmp(part, "CONTROL") == 0)
            mapped = "Ctrl";
        else if (g_ascii_strcasecmp(part, "ALT") == 0)
            mapped = "Alt";
        else if (g_ascii_strcasecmp(part, "SHIFT") == 0)
            mapped = "Shift";
        else if (g_ascii_strcasecmp(part, "LOGO") == 0 || g_ascii_strcasecmp(part, "SUPER") == 0 || g_ascii_strcasecmp(part, "META") == 0)
            mapped = "Super";
        if (out->len)
            g_string_append_c(out, '+');
        g_string_append(out, mapped);
    }
    g_strfreev(parts);
    return g_string_free(out, FALSE);
}

static gchar* shortcuts_ini_path(void)
{
    char const* dir = g_get_user_config_dir();
    char const* state = g_getenv("SDE_STATE_DIR");
    gchar* base;
    if (state && *state)
        base = g_strdup(state);
    else
        base = g_build_filename(dir ? dir : "/tmp", "sde", NULL);
    g_mkdir_with_parents(base, 0700);
    gchar* path = g_build_filename(base, "global-shortcuts.ini", NULL);
    g_free(base);
    return path;
}

static void write_shortcuts_ini(void)
{
    GKeyFile* key_file = g_key_file_new();
    if (g_shortcuts) {
        for (guint i = 0; i < g_shortcuts->len; ++i) {
            struct gs_shortcut* shortcut = g_ptr_array_index(g_shortcuts, i);
            gchar* group = g_strdup_printf("Shortcut%u", i);
            g_key_file_set_string(key_file, group, "Session", shortcut->session);
            g_key_file_set_string(key_file, group, "Id", shortcut->id);
            g_key_file_set_string(key_file, group, "Trigger", shortcut->trigger);
            if (shortcut->description)
                g_key_file_set_string(key_file, group, "Description", shortcut->description);
            g_free(group);
        }
    }
    gsize length = 0;
    gchar* data = g_key_file_to_data(key_file, &length, NULL);
    gchar* path = shortcuts_ini_path();
    if (data)
        g_file_set_contents(path, data, (gssize)length, NULL);
    g_free(data);
    g_free(path);
    g_key_file_unref(key_file);
}

// --- FIFO: compositor -> Activated signal -----------------------------------

static gboolean fifo_callback(GIOChannel* channel, GIOCondition condition, gpointer user_data)
{
    (void)user_data;
    if (condition & G_IO_IN) {
        for (;;) {
            gsize length = 0;
            gchar* line = NULL;
            GError* error = NULL;
            GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, &error);
            if (status != G_IO_STATUS_NORMAL) {
                if (error)
                    g_error_free(error);
                break;
            }
            gchar* trimmed = g_strstrip(line);
            gchar** tokens = g_strsplit(trimmed, "\t", 2);
            if (tokens[0] && tokens[1] && g_connection) {
                GError* emit_error = NULL;
                GVariantBuilder options;
                g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
                g_dbus_connection_emit_signal(g_connection, NULL, "/org/freedesktop/portal/desktop",
                    portal_global_shortcuts_interface_name, "Activated",
                    g_variant_new("(ost@a{sv})", tokens[0], tokens[1], (guint64)g_get_monotonic_time(), g_variant_builder_end(&options)),
                    &emit_error);
                if (emit_error)
                    g_error_free(emit_error);
            }
            g_strfreev(tokens);
            g_free(line);
        }
    }
    return TRUE;
}

static void ensure_fifo(GDBusConnection* connection)
{
    g_connection = connection;
    if (g_fifo_channel)
        return;
    char const* runtime = g_get_user_runtime_dir();
    gchar* path = g_strdup_printf("%s/sde-global-shortcuts.fifo", runtime ? runtime : "/tmp");
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) {
        g_free(path);
        return;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    g_free(path);
    if (fd < 0)
        return;
    g_fifo_channel = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(g_fifo_channel, NULL, NULL);
    g_fifo_watch = g_io_add_watch(g_fifo_channel, G_IO_IN | G_IO_HUP, fifo_callback, NULL);
}

// --- D-Bus ------------------------------------------------------------------

static void return_response(GDBusMethodInvocation* invocation, guint32 response, GVariantBuilder* results)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, results));
}

void portal_global_shortcuts_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (!g_shortcuts)
        g_shortcuts = g_ptr_array_new_with_free_func(NULL);

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));

    if (g_strcmp0(method_name, "CreateSession") == 0) {
        ensure_fifo(connection);
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "BindShortcuts") == 0) {
        ensure_fifo(connection);
        char const* session_handle = NULL;
        GVariant* shortcuts = NULL;
        g_variant_get(parameters, "(o&o@a(sa{sv})&s@a{sv})", NULL, &session_handle, &shortcuts, NULL, NULL);
        ensure_fifo(connection);

        if (shortcuts) {
            GVariantIter iter;
            g_variant_iter_init(&iter, shortcuts);
            char const* id = NULL;
            GVariant* props = NULL;
            while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &props)) {
                char const* trigger = NULL;
                char const* description = NULL;
                if (props) {
                    g_variant_lookup(props, "trigger", "&s", &trigger);
                    g_variant_lookup(props, "description", "&s", &description);
                }
                if (id && trigger) {
                    struct gs_shortcut* shortcut = g_new0(struct gs_shortcut, 1);
                    shortcut->session = g_strdup(session_handle);
                    shortcut->id = g_strdup(id);
                    shortcut->trigger = normalize_trigger(trigger);
                    shortcut->description = description ? g_strdup(description) : NULL;
                    g_ptr_array_add(g_shortcuts, shortcut);
                }
                if (props)
                    g_variant_unref(props);
            }
            g_variant_unref(shortcuts);
        }
        write_shortcuts_ini();

        // Return the bound shortcuts.
        GVariantBuilder bound;
        g_variant_builder_init(&bound, G_VARIANT_TYPE("a(sa{sv})"));
        for (guint i = 0; i < g_shortcuts->len; ++i) {
            struct gs_shortcut* shortcut = g_ptr_array_index(g_shortcuts, i);
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "trigger", g_variant_new_string(shortcut->trigger));
            if (shortcut->description)
                g_variant_builder_add(&props, "{sv}", "description", g_variant_new_string(shortcut->description));
            g_variant_builder_add(&bound, "(sa{sv})", shortcut->id, &props);
        }
        g_variant_builder_add(&results, "{sv}", "shortcuts", g_variant_builder_end(&bound));
        return_response(invocation, 0, &results);
        return;
    }

    if (g_strcmp0(method_name, "ListShortcuts") == 0) {
        GVariantBuilder bound;
        g_variant_builder_init(&bound, G_VARIANT_TYPE("a(sa{sv})"));
        for (guint i = 0; g_shortcuts && i < g_shortcuts->len; ++i) {
            struct gs_shortcut* shortcut = g_ptr_array_index(g_shortcuts, i);
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "trigger", g_variant_new_string(shortcut->trigger));
            if (shortcut->description)
                g_variant_builder_add(&props, "{sv}", "description", g_variant_new_string(shortcut->description));
            g_variant_builder_add(&bound, "(sa{sv})", shortcut->id, &props);
        }
        g_variant_builder_add(&results, "{sv}", "shortcuts", g_variant_builder_end(&bound));
        return_response(invocation, 0, &results);
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

GVariant* portal_global_shortcuts_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(1);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}
