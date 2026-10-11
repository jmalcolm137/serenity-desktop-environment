/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// sde-settings-portal — the SDE backend for the freedesktop Settings portal
// (R4.4). It implements org.freedesktop.impl.portal.Settings (the interface
// xdg-desktop-portal calls) and answers the appearance subset — color-scheme,
// accent-color, contrast — from the active SDE theme, so GTK/Qt/Flatpak apps
// follow the Serenity theme.
//
// It reads the theme broker file ($XDG_RUNTIME_DIR/sde-theme, written by the
// session) and re-reads it on change, emitting SettingChanged.
//
//   gdbus call --session --dest org.freedesktop.impl.portal.desktop.sde
//     --object-path /org/freedesktop/portal/desktop
//     --method org.freedesktop.impl.portal.Settings.Read
//     org.freedesktop.appearance color-scheme

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.sde"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"
#define APPEARANCE_NAMESPACE "org.freedesktop.appearance"

static const char introspection_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Settings'>"
    "    <method name='Read'>"
    "      <arg type='s' name='namespace' direction='in'/>"
    "      <arg type='s' name='key' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='ReadAll'>"
    "      <arg type='s' name='namespace' direction='in'/>"
    "      <arg type='a{sv}' name='value' direction='out'/>"
    "    </method>"
    "    <signal name='SettingChanged'>"
    "      <arg type='s' name='namespace'/>"
    "      <arg type='s' name='key'/>"
    "      <arg type='v' name='value'/>"
    "    </signal>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

typedef struct {
    GDBusConnection* connection;
    GFileMonitor* theme_monitor;
    guint registration_id;
} Portal;

// --- theme reading ----------------------------------------------------------

static char* broker_path(void)
{
    const char* runtime = g_get_user_runtime_dir();
    return g_build_filename(runtime ? runtime : "/tmp", "sde-theme", NULL);
}

static char* read_first_line(const char* path)
{
    FILE* file = fopen(path, "r");
    if (!file)
        return NULL;
    char buffer[4096];
    char* result = NULL;
    if (fgets(buffer, sizeof(buffer), file)) {
        buffer[strcspn(buffer, "\r\n")] = '\0';
        if (buffer[0] != '\0')
            result = g_strdup(buffer);
    }
    fclose(file);
    return result;
}

static gboolean parse_color(const char* value, double* r, double* g, double* b)
{
    if (!value || value[0] != '#')
        return FALSE;
    unsigned int packed = 0;
    if (strlen(value) >= 7 && sscanf(value + 1, "%6x", &packed) == 1) {
        *r = ((packed >> 16) & 0xff) / 255.0;
        *g = ((packed >> 8) & 0xff) / 255.0;
        *b = (packed & 0xff) / 255.0;
        return TRUE;
    }
    return FALSE;
}

// Defaults for when there is no readable theme: a light scheme, blue accent.
static void read_appearance(guint32* color_scheme, double* accent_r, double* accent_g, double* accent_b)
{
    *color_scheme = 2; // prefer-light (SDE's classic default)
    *accent_r = 0.29;
    *accent_g = 0.43;
    *accent_b = 0.67;

    char* broker = broker_path();
    char* theme_path = read_first_line(broker);
    g_free(broker);
    if (!theme_path)
        return;

    FILE* file = fopen(theme_path, "r");
    g_free(theme_path);
    if (!file)
        return;

    char window_color[64] = { 0 };
    char accent_color[64] = { 0 };
    char selection_color[64] = { 0 };
    gboolean in_colors = FALSE;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char* trimmed = g_strstrip(line);
        if (trimmed[0] == '[') {
            in_colors = g_strcmp0(trimmed, "[Colors]") == 0;
            continue;
        }
        if (!in_colors)
            continue;
        if (g_str_has_prefix(trimmed, "Window="))
            g_strlcpy(window_color, trimmed + strlen("Window="), sizeof(window_color));
        else if (g_str_has_prefix(trimmed, "Accent="))
            g_strlcpy(accent_color, trimmed + strlen("Accent="), sizeof(accent_color));
        else if (g_str_has_prefix(trimmed, "Selection="))
            g_strlcpy(selection_color, trimmed + strlen("Selection="), sizeof(selection_color));
    }
    fclose(file);

    double r = 0, g = 0, b = 0;
    if (parse_color(window_color, &r, &g, &b)) {
        double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        *color_scheme = luminance < 0.5 ? 1 : 2; // prefer-dark : prefer-light
    }
    if (!parse_color(accent_color, accent_r, accent_g, accent_b))
        (void)parse_color(selection_color, accent_r, accent_g, accent_b);
}

// --- D-Bus ------------------------------------------------------------------

static GVariant* appearance_value(const char* key, guint32 color_scheme, double ar, double ag, double ab)
{
    if (g_strcmp0(key, "color-scheme") == 0)
        return g_variant_new_uint32(color_scheme);
    if (g_strcmp0(key, "contrast") == 0)
        return g_variant_new_uint32(0);
    if (g_strcmp0(key, "accent-color") == 0)
        return g_variant_new("(ddd)", ar, ag, ab);
    return NULL;
}

static void handle_method_call(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters, GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;

    guint32 color_scheme = 2;
    double ar = 0, ag = 0, ab = 0;
    read_appearance(&color_scheme, &ar, &ag, &ab);

    if (g_strcmp0(method_name, "Read") == 0) {
        const char* namespace_name = NULL;
        const char* key = NULL;
        g_variant_get(parameters, "(&s&s)", &namespace_name, &key);
        if (g_strcmp0(namespace_name, APPEARANCE_NAMESPACE) != 0) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown namespace %s", namespace_name);
            return;
        }
        GVariant* value = appearance_value(key, color_scheme, ar, ag, ab);
        if (!value) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown key %s", key);
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        return;
    }

    if (g_strcmp0(method_name, "ReadAll") == 0) {
        const char* namespace_name = NULL;
        g_variant_get(parameters, "(&s)", &namespace_name);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (g_strcmp0(namespace_name, APPEARANCE_NAMESPACE) == 0) {
            g_variant_builder_add(&builder, "{sv}", "color-scheme", g_variant_new_uint32(color_scheme));
            g_variant_builder_add(&builder, "{sv}", "accent-color", g_variant_new("(ddd)", ar, ag, ab));
            g_variant_builder_add(&builder, "{sv}", "contrast", g_variant_new_uint32(0));
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &builder));
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

static GVariant* handle_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
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

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    handle_get_property,
    NULL, // set_property
    { 0 },
};

// Emit SettingChanged for the appearance keys on theme change.
static void emit_setting_changed(Portal* portal)
{
    guint32 color_scheme = 2;
    double ar = 0, ag = 0, ab = 0;
    read_appearance(&color_scheme, &ar, &ag, &ab);
    const char* names[] = { "color-scheme", "accent-color", "contrast" };
    GVariant* values[] = {
        g_variant_new_uint32(color_scheme),
        g_variant_new("(ddd)", ar, ag, ab),
        g_variant_new_uint32(0),
    };
    for (int i = 0; i < 3; ++i) {
        GError* error = NULL;
        g_dbus_connection_emit_signal(portal->connection, NULL, OBJECT_PATH,
            "org.freedesktop.impl.portal.Settings", "SettingChanged",
            g_variant_new("(ssv)", APPEARANCE_NAMESPACE, names[i], values[i]), &error);
        if (error) {
            g_warning("sde-settings-portal: emit failed: %s", error->message);
            g_error_free(error);
        }
    }
}

static void on_theme_changed(GFileMonitor* monitor, GFile* file, GFile* other_file, GFileMonitorEvent event_type, gpointer user_data)
{
    (void)monitor;
    (void)file;
    (void)other_file;
    (void)event_type;
    emit_setting_changed((Portal*)user_data);
}

static void on_bus_acquired(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    (void)name;
    Portal* portal = (Portal*)user_data;
    portal->connection = connection;

    GError* error = NULL;
    GDBusNodeInfo* node_info = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!node_info) {
        g_printerr("sde-settings-portal: bad introspection xml: %s\n", error->message);
        exit(1);
    }
    portal->registration_id = g_dbus_connection_register_object(connection, OBJECT_PATH,
        node_info->interfaces[0], &interface_vtable, portal, NULL, &error);
    g_dbus_node_info_unref(node_info);
    if (!portal->registration_id) {
        g_printerr("sde-settings-portal: could not register object: %s\n", error->message);
        exit(1);
    }

    char* broker = broker_path();
    GFile* file = g_file_new_for_path(broker);
    portal->theme_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
    if (portal->theme_monitor)
        g_signal_connect(portal->theme_monitor, "changed", G_CALLBACK(on_theme_changed), portal);
    else if (error)
        g_warning("sde-settings-portal: theme watch failed: %s", error->message);
    g_object_unref(file);
    g_free(broker);

    g_print("sde-settings-portal: serving %s on %s\n", OBJECT_PATH, BUS_NAME);
}

int main(void)
{
    Portal portal = { 0 };
    g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired, NULL, NULL, &portal, NULL);

    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
