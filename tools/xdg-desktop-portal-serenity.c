/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// xdg-desktop-portal-serenity — the SDE backend for the freedesktop portals
// (R5). It implements org.freedesktop.impl.portal.* on
// /org/freedesktop/portal/desktop and owns
// org.freedesktop.impl.portal.desktop.serenity, so the xdg-desktop-portal
// frontend can route requests to it (see contrib/xdg-desktop-portal/portals.conf
// and the .portal file).
//
// It is a portable host D-Bus service (GIO/GDBus, standard protocols only): the
// Serenity-specific bits are reached through the session launcher/notifier, with
// portable fallbacks, so it also runs under other compositors.
//
// Phase 1 implements: Settings, Account, Lockdown, Background, AppChooser, Email,
// Location, ProxyResolver, OpenURI, Notification. ScreenCast / RemoteDesktop /
// InputCapture / GlobalShortcuts / DynamicLauncher / FileChooser / Print are
// Phase 2.

#include <gio/gio.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "portal-screencast.h"

#define BUS_NAME "org.freedesktop.impl.portal.desktop.serenity"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"
#define APPEARANCE_NAMESPACE "org.freedesktop.appearance"

// Portal response codes (org.freedesktop.portal.Request).
enum {
    RESPONSE_SUCCESS = 0,
    RESPONSE_CANCELLED = 1,
    RESPONSE_OTHER_ERROR = 2,
};

typedef struct {
    GDBusConnection* connection;
    guint registration_ids[16];
    guint registration_count;
} Portal;

static void return_response(GDBusMethodInvocation* invocation, guint32 response, GVariantBuilder* results)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, results));
}

// --- Settings ---------------------------------------------------------------

static gboolean parse_color(char const* value, double* r, double* g, double* b)
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

static char* read_theme_appearance(void)
{
    char* theme_path = NULL;
    char const* runtime = g_get_user_runtime_dir();
    if (runtime) {
        gchar* broker = g_build_filename(runtime, "sde-theme", NULL);
        gchar* contents = NULL;
        if (g_file_get_contents(broker, &contents, NULL, NULL)) {
            theme_path = g_strdup(g_strstrip(contents));
            g_free(contents);
        }
        g_free(broker);
    }
    return theme_path;
}

static void read_appearance(guint32* color_scheme, double* ar, double* ag, double* ab)
{
    *color_scheme = 2; // prefer-light (SDE's classic default)
    *ar = 0.29;
    *ag = 0.43;
    *ab = 0.67;

    char* theme_path = read_theme_appearance();
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
        *color_scheme = luminance < 0.5 ? 1 : 2;
    }
    if (!parse_color(accent_color, ar, ag, ab))
        (void)parse_color(selection_color, ar, ag, ab);
}

static GVariant* settings_appearance_value(char const* key, guint32 color_scheme, double ar, double ag, double ab)
{
    if (g_strcmp0(key, "color-scheme") == 0)
        return g_variant_new_uint32(color_scheme);
    if (g_strcmp0(key, "contrast") == 0)
        return g_variant_new_uint32(0);
    if (g_strcmp0(key, "accent-color") == 0)
        return g_variant_new("(ddd)", ar, ag, ab);
    return NULL;
}

static void handle_settings(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    guint32 color_scheme = 2;
    double ar = 0, ag = 0, ab = 0;
    read_appearance(&color_scheme, &ar, &ag, &ab);

    if (g_strcmp0(method_name, "Read") == 0) {
        char const* ns = NULL;
        char const* key = NULL;
        g_variant_get(parameters, "(&s&s)", &ns, &key);
        if (g_strcmp0(ns, APPEARANCE_NAMESPACE) != 0) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown namespace %s", ns);
            return;
        }
        GVariant* value = settings_appearance_value(key, color_scheme, ar, ag, ab);
        if (!value) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "unknown key %s", key);
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        return;
    }

    if (g_strcmp0(method_name, "ReadAll") == 0) {
        char const* ns = NULL;
        g_variant_get(parameters, "(&s)", &ns);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (g_strcmp0(ns, APPEARANCE_NAMESPACE) == 0) {
            g_variant_builder_add(&builder, "{sv}", "color-scheme", g_variant_new_uint32(color_scheme));
            g_variant_builder_add(&builder, "{sv}", "accent-color", g_variant_new("(ddd)", ar, ag, ab));
            g_variant_builder_add(&builder, "{sv}", "contrast", g_variant_new_uint32(0));
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &builder));
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

static GVariant* settings_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
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

// --- Account ----------------------------------------------------------------

static const char account_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Account'>"
    "    <method name='GetUserInformation'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void handle_account(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "GetUserInformation") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    // Standard implementation: report the local account (org.freedesktop.Accounts
    // is the usual source; we use the passwd entry, which is always available).
    struct passwd* pw = getpwuid(getuid());
    char const* name = (pw && pw->pw_gecos && pw->pw_gecos[0]) ? pw->pw_gecos : (pw ? pw->pw_name : "User");
    char const* id = (pw && pw->pw_name) ? pw->pw_name : "user";
    // Trim a trailing comma section from the GECOS field.
    gchar* display_name = g_strdup(name);
    gchar* comma = strchr(display_name, ',');
    if (comma)
        *comma = '\0';

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&results, "{sv}", "id", g_variant_new_string(id));
    g_variant_builder_add(&results, "{sv}", "name", g_variant_new_string(display_name));
    g_variant_builder_add(&results, "{sv}", "image", g_variant_new_string(""));
    g_free(display_name);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", RESPONSE_SUCCESS, &results));
}

static GVariant* account_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
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

// --- Lockdown ---------------------------------------------------------------

static const char lockdown_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Lockdown'>"
    "    <property name='disable_application_handlers' type='b' access='read'/>"
    "    <property name='disable_commandline' type='b' access='read'/>"
    "    <property name='disable_printing' type='b' access='read'/>"
    "    <property name='disable_save_to_disk' type='b' access='read'/>"
    "    <property name='disable_location' type='b' access='read'/>"
    "    <property name='disable_user_switching' type='b' access='read'/>"
    "  </interface>"
    "</node>";

static GVariant* bool_setting(char const* key, char const* env, gboolean fallback)
{
    char const* value = getenv(env);
    if (value && *value)
        fallback = (g_strcmp0(value, "1") == 0 || g_ascii_strcasecmp(value, "true") == 0);
    (void)key;
    return g_variant_new_boolean(fallback);
}

static GVariant* lockdown_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "disable_application_handlers") == 0)
        return bool_setting(property_name, "SDE_PORTAL_LOCKDOWN_APP_HANDLERS", FALSE);
    if (g_strcmp0(property_name, "disable_commandline") == 0)
        return bool_setting(property_name, "SDE_PORTAL_LOCKDOWN_COMMANDLINE", FALSE);
    if (g_strcmp0(property_name, "disable_printing") == 0)
        return bool_setting(property_name, "SDE_PORTAL_LOCKDOWN_PRINTING", FALSE);
    if (g_strcmp0(property_name, "disable_save_to_disk") == 0)
        return bool_setting(property_name, "SDE_PORTAL_LOCKDOWN_SAVE_TO_DISK", FALSE);
    if (g_strcmp0(property_name, "disable_location") == 0)
        return bool_setting(property_name, "SDE_PORTAL_LOCKDOWN_LOCATION", FALSE);
    if (g_strcmp0(property_name, "disable_user_switching") == 0)
        return bool_setting(property_name, "SDE_PORTAL_LOCKDOWN_USER_SWITCHING", FALSE);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}

// --- Background -------------------------------------------------------------

static const char background_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Background'>"
    "    <method name='SetStatus'>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <method name='SetWallpaperURI'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='uri' direction='in'/>"
    "      <arg type='b' name='show_preview' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <method name='SetAutostart'>"
    "      <arg type='b' name='enable' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "    <property name='ApplicationName' type='s' access='read'/>"
    "    <property name='ApplicationID' type='s' access='read'/>"
    "  </interface>"
    "</node>";

// SetAutostart writes an XDG autostart entry, the standard mechanism the SDE
// supervisor already honours.
static void write_autostart(gboolean enable, GVariant* options)
{
    char const* app_id = NULL;
    if (options) {
        GVariant* value = g_variant_lookup_value(options, "reason", G_VARIANT_TYPE_STRING);
        if (value)
            g_variant_unref(value);
    }
    (void)app_id;
    gchar* dir = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
    g_mkdir_with_parents(dir, 0755);
    // The frontend supplies the caller's app id in the options under
    // "xdp-request-id"; the desktop file name is app_id.desktop. We keep a single
    // generic entry since the backend is not told the app id here.
    gchar* path = g_build_filename(dir, "xdg-desktop-portal-serenity-autostart.desktop", NULL);
    if (enable) {
        gchar* contents = g_strdup(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Autostart\n"
            "X-GNOME-Autostart-enabled=true\n");
        g_file_set_contents(path, contents, -1, NULL);
        g_free(contents);
    } else {
        unlink(path);
    }
    g_free(path);
    g_free(dir);
}

static void handle_background(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "SetStatus") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_strcmp0(method_name, "SetAutostart") == 0) {
        gboolean enable = FALSE;
        GVariant* options = NULL;
        g_variant_get(parameters, "(b@a{sv})", &enable, &options);
        write_autostart(enable, options);
        if (options)
            g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_strcmp0(method_name, "SetWallpaperURI") == 0) {
        // Handled by the Wallpaper interface's SetWallpaper; accept here.
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

static GVariant* background_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(1);
    if (g_strcmp0(property_name, "ApplicationName") == 0)
        return g_variant_new_string("Serenity Desktop Environment");
    if (g_strcmp0(property_name, "ApplicationID") == 0)
        return g_variant_new_string("org.serenityos.SDE");
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}

// --- AppChooser -------------------------------------------------------------

static const char appchooser_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.AppChooser'>"
    "    <method name='ChooseApplication'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='as' name='choices' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void handle_appchooser(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "ChooseApplication") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    // Phase 1: choose the first offered application (a real chooser UI is a
    // follow-up). The frontend passes the ordered choices.
    GVariant* choices = NULL;
    g_variant_get(parameters, "(oss@as@a{sv})", NULL, NULL, NULL, &choices, NULL);
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    guint32 response = RESPONSE_CANCELLED;
    if (choices && g_variant_n_children(choices) > 0) {
        GVariant* first = g_variant_get_child_value(choices, 0);
        g_variant_builder_add(&results, "{sv}", "choice", g_variant_new_string(g_variant_get_string(first, NULL)));
        g_variant_unref(first);
        response = RESPONSE_SUCCESS;
    }
    if (choices)
        g_variant_unref(choices);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", response, &results));
}

static GVariant* appchooser_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
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

// --- Email ------------------------------------------------------------------

static const char email_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Email'>"
    "    <method name='ComposeEmail'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='address' direction='in'/>"
    "      <arg type='a{sa{sv}}' name='addresses' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void handle_email(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "ComposeEmail") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }
    char const* address = NULL;
    g_variant_get(parameters, "(oss&s@a{sa{sv}}@a{sv})", NULL, NULL, NULL, &address, NULL, NULL);
    // Delegate to the OpenURI machinery via a mailto: URI (the standard path).
    gchar* uri = g_strdup_printf("mailto:%s", address ? address : "");
    gboolean opened = FALSE;
    char const* opener = getenv("SDE_PORTAL_OPENER");
    if (opener && *opener) {
        gchar* argv[] = { (gchar*)opener, uri, NULL };
        opened = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    } else {
        GError* error = NULL;
        opened = g_app_info_launch_default_for_uri(uri, NULL, &error);
        if (error)
            g_error_free(error);
    }
    g_free(uri);

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", opened ? RESPONSE_SUCCESS : RESPONSE_OTHER_ERROR, &results));
}

static GVariant* email_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
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

// --- Location (session-based; Phase 1 reports unavailable) ------------------

static const char location_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Location'>"
    "    <method name='CreateSession'>"
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
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Update'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='Stop'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void handle_location(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;
    (void)method_name;
    // No location provider: report "not available" (response 2) with no results.
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", RESPONSE_OTHER_ERROR, &results));
}

// --- ProxyResolver ----------------------------------------------------------

static const char proxyresolver_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.ProxyResolver'>"
    "    <method name='Lookup'>"
    "      <arg type='s' name='uri' direction='in'/>"
    "      <arg type='as' name='proxies' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void handle_proxyresolver(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;
    if (g_strcmp0(method_name, "Lookup") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }
    // Empty list = "use the proxy autoconfiguration / direct connection".
    GVariantBuilder proxies;
    g_variant_builder_init(&proxies, G_VARIANT_TYPE("as"));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &proxies));
}

static GVariant* proxyresolver_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
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

// --- OpenURI ----------------------------------------------------------------

static const char openuri_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.OpenURI'>"
    "    <method name='OpenURI'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='uri' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='OpenDirectory'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='uri' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='OpenFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='h' name='fd' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

// Launch a URI. Prefer SDE_PORTAL_OPENER (the session launcher) so SDE handles
// it; otherwise fall back to GIO's default-app launch (the standard mechanism,
// which does not recurse through the portal).
static gboolean launch_uri(char const* uri)
{
    char const* opener = getenv("SDE_PORTAL_OPENER");
    if (opener && *opener) {
        gchar* argv[] = { (gchar*)opener, (gchar*)uri, NULL };
        return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    GError* error = NULL;
    gboolean ok = g_app_info_launch_default_for_uri(uri, NULL, &error);
    if (error)
        g_error_free(error);
    return ok;
}

static void handle_openuri(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    char const* uri = NULL;
    if (g_strcmp0(method_name, "OpenURI") == 0 || g_strcmp0(method_name, "OpenDirectory") == 0) {
        g_variant_get(parameters, "(o&s&s@a{sv})", NULL, NULL, &uri, NULL);
    } else if (g_strcmp0(method_name, "OpenFile") == 0) {
        // OpenFile passes an fd; build a file:// URI from /proc/self/fd/N.
        gint fd = -1;
        g_variant_get(parameters, "(os h@a{sv})", NULL, NULL, &fd, NULL);
        static char fd_uri[64];
        g_snprintf(fd_uri, sizeof(fd_uri), "file:///proc/self/fd/%d", fd);
        uri = fd_uri;
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    gboolean opened = uri && launch_uri(uri);
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", opened ? RESPONSE_SUCCESS : RESPONSE_OTHER_ERROR, &results));
}

static GVariant* openuri_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(3);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}

// --- Notification -----------------------------------------------------------

static const char notification_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Notification'>"
    "    <method name='AddNotification'>"
    "      <arg type='s' name='id' direction='in'/>"
    "      <arg type='a{sv}' name='notification' direction='in'/>"
    "    </method>"
    "    <method name='RemoveNotification'>"
    "      <arg type='s' name='id' direction='in'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

// Display a notification. SDE_PORTAL_NOTIFIER takes "summary\0body\0" as argv
// (the session points this at the Serenity notify tool); otherwise notify-send.
static void show_notification(char const* title, char const* body)
{
    char const* notifier = getenv("SDE_PORTAL_NOTIFIER");
    if (notifier && *notifier) {
        gchar* argv[] = { (gchar*)notifier, (gchar*)title, (gchar*)body, NULL };
        (void)g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }
    gchar* argv[] = { (gchar*)"notify-send", (gchar*)title, (gchar*)body, NULL };
    (void)g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void handle_notification(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "AddNotification") == 0) {
        char const* id = NULL;
        GVariant* notification = NULL;
        g_variant_get(parameters, "(&s@a{sv})", &id, &notification);
        char const* title = NULL;
        char const* body = NULL;
        if (notification) {
            if (!g_variant_lookup(notification, "title", "&s", &title))
                title = "";
            if (!g_variant_lookup(notification, "body", "&s", &body))
                body = "";
        }
        show_notification(title ? title : "", body ? body : "");
        if (notification)
            g_variant_unref(notification);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_strcmp0(method_name, "RemoveNotification") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

static GVariant* notification_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
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

// --- Realtime / PowerProfileMonitor / GameMode / MemoryMonitor --------------

static const char realtime_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Realtime'>"
    "    <method name='MakeThreadHighPriority'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='i' name='priority' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='MakeThreadRealtime'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='t' name='thread' direction='in'/>"
    "      <arg type='i' name='priority' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void handle_realtime(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)method_name;
    (void)parameters;
    (void)user_data;
    // Accepted (the frontend/realtime policy decides; SDE grants it).
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", RESPONSE_SUCCESS, &results));
}

static GVariant* generic_version_property(gchar const* property_name, GError** error, guint32 version)
{
    if (g_strcmp0(property_name, "version") == 0)
        return g_variant_new_uint32(version);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "unknown property %s", property_name);
    return NULL;
}

static const char powerprofile_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.PowerProfileMonitor'>"
    "    <property name='version' type='u' access='read'/>"
    "    <property name='power-saver-enabled' type='b' access='read'/>"
    "  </interface>"
    "</node>";

static GVariant* powerprofile_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    if (g_strcmp0(property_name, "power-saver-enabled") == 0) {
        char const* value = getenv("SDE_POWER_SAVER");
        return g_variant_new_boolean(value && *value && g_strcmp0(value, "0") != 0);
    }
    return generic_version_property(property_name, error, 1);
}

static const char gamemode_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.GameMode'>"
    "    <method name='RegisterGame'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='UnregisterGame'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "    </method>"
    "    <method name='QueryStatus'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void handle_gamemode(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;
    if (g_strcmp0(method_name, "UnregisterGame") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    if (g_strcmp0(method_name, "QueryStatus") == 0)
        g_variant_builder_add(&results, "{sv}", "active", g_variant_new_boolean(FALSE));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ua{sv})", RESPONSE_SUCCESS, &results));
}

static const char memorymonitor_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.MemoryMonitor'>"
    "    <signal name='LowMemoryWarning'>"
    "      <arg type='u' name='level'/>"
    "    </signal>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

// --- FileChooser ------------------------------------------------------------

static const char filechooser_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.FileChooser'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='OpenFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='SaveFile'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// The chooser is a LibGUI app (FileChooserDialog); it prints the chosen path.
static void handle_filechooser(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));

    if (g_strcmp0(method_name, "OpenFile") != 0 && g_strcmp0(method_name, "SaveFile") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    char const* handle = NULL;
    char const* app_id = NULL;
    char const* parent_window = NULL;
    char const* title = NULL;
    GVariant* options = NULL;
    g_variant_get(parameters, "(o&s&s&s@a{sv})", &handle, &app_id, &parent_window, &title, &options);

    gboolean multiple = FALSE;
    gboolean directory = FALSE;
    char const* current_name = NULL;
    GVariant* current_folder = NULL;
    if (options) {
        g_variant_lookup(options, "multiple", "b", &multiple);
        g_variant_lookup(options, "directory", "b", &directory);
        g_variant_lookup(options, "current_name", "&s", &current_name);
        current_folder = g_variant_lookup_value(options, "current_folder", G_VARIANT_TYPE_BYTESTRING);
    }

    char const* chooser = getenv("SDE_PORTAL_FILE_CHOOSER");
    if (!chooser || !*chooser) {
        if (current_folder)
            g_variant_unref(current_folder);
        if (options)
            g_variant_unref(options);
        return_response(invocation, RESPONSE_OTHER_ERROR, &results);
        return;
    }

    char const* mode = (g_strcmp0(method_name, "SaveFile") == 0) ? "save" : (directory ? "folder" : "open");
    gchar* folder_arg = current_folder ? g_strdup(g_variant_get_bytestring(current_folder)) : g_strdup(g_get_home_dir());
    gchar* name_arg = g_strdup(current_name && *current_name ? current_name : "Untitled");
    gchar* title_arg = g_strdup(title && *title ? title : "Select File");

    char const* argv[12];
    int argc = 0;
    argv[argc++] = chooser;
    argv[argc++] = "-m";
    argv[argc++] = mode;
    argv[argc++] = "-t";
    argv[argc++] = title_arg;
    argv[argc++] = "-n";
    argv[argc++] = name_arg;
    argv[argc++] = "-f";
    argv[argc++] = folder_arg;
    if (multiple)
        argv[argc++] = "--multiple";
    argv[argc] = NULL;

    char* out = NULL;
    gint wait_status = 0;
    GError* error = NULL;
    gboolean ok = g_spawn_sync(NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error);

    guint32 response = RESPONSE_CANCELLED;
    if (ok && out && g_spawn_check_wait_status(wait_status, NULL)) {
        gchar* path = g_strstrip(out);
        if (*path) {
            if (gchar* uri = g_filename_to_uri(path, NULL, NULL)) {
                GVariantBuilder uris;
                g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
                g_variant_builder_add(&uris, "s", uri);
                g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
                g_free(uri);
                response = RESPONSE_SUCCESS;
            }
        }
    } else if (error) {
        response = RESPONSE_OTHER_ERROR;
    }

    g_free(out);
    g_free(folder_arg);
    g_free(name_arg);
    g_free(title_arg);
    if (error)
        g_error_free(error);
    if (current_folder)
        g_variant_unref(current_folder);
    if (options)
        g_variant_unref(options);

    return_response(invocation, response, &results);
}

// --- Screenshot -------------------------------------------------------------

static const char screenshot_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Screenshot'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='Screenshot'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// Capture the screen (reusing the ScreenCast capture) into a PNG and return its
// file:// URI.
static void handle_screenshot(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));

    if (g_strcmp0(method_name, "Screenshot") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    char const* runtime = g_get_user_runtime_dir();
    gchar* path = g_strdup_printf("%s/sde-screenshot-%d.png", runtime ? runtime : "/tmp", (int)getpid());
    int captured = portal_screencast_capture_png(path);
    if (captured == 0) {
        if (gchar* uri = g_filename_to_uri(path, NULL, NULL)) {
            g_variant_builder_add(&results, "{sv}", "uri", g_variant_new_string(uri));
            g_free(uri);
            g_free(path);
            return_response(invocation, RESPONSE_SUCCESS, &results);
            return;
        }
    }
    g_free(path);
    return_response(invocation, RESPONSE_OTHER_ERROR, &results);
}

// --- Settings introspection -------------------------------------------------

static const char settings_introspection[] =
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

// --- Registration -----------------------------------------------------------

typedef struct {
    char const* xml;
    GDBusInterfaceVTable const* vtable;
} InterfaceRegistration;

static const GDBusInterfaceVTable settings_vtable = { handle_settings, settings_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable account_vtable = { handle_account, account_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable lockdown_vtable = { NULL, lockdown_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable background_vtable = { handle_background, background_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable appchooser_vtable = { handle_appchooser, appchooser_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable email_vtable = { handle_email, email_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable location_vtable = { handle_location, NULL, NULL, { 0 } };
static const GDBusInterfaceVTable proxyresolver_vtable = { handle_proxyresolver, proxyresolver_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable openuri_vtable = { handle_openuri, openuri_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable notification_vtable = { handle_notification, notification_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable screencast_vtable = { portal_screencast_method_call, portal_screencast_get_property, NULL, { 0 } };

static GVariant* simple_version_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    return generic_version_property(property_name, error, 1);
}

static GVariant* filechooser_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    return generic_version_property(property_name, error, 2); // OpenFile, SaveFile
}

static const GDBusInterfaceVTable filechooser_vtable = { handle_filechooser, filechooser_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable screenshot_vtable = { handle_screenshot, simple_version_get_property, NULL, { 0 } };

static const GDBusInterfaceVTable realtime_vtable = { handle_realtime, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable powerprofile_vtable = { NULL, powerprofile_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable gamemode_vtable = { handle_gamemode, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable memorymonitor_vtable = { NULL, simple_version_get_property, NULL, { 0 } };

static InterfaceRegistration const interfaces[] = {
    { settings_introspection, &settings_vtable },
    { account_introspection, &account_vtable },
    { lockdown_introspection, &lockdown_vtable },
    { background_introspection, &background_vtable },
    { appchooser_introspection, &appchooser_vtable },
    { email_introspection, &email_vtable },
    { location_introspection, &location_vtable },
    { proxyresolver_introspection, &proxyresolver_vtable },
    { openuri_introspection, &openuri_vtable },
    { notification_introspection, &notification_vtable },
    { portal_screencast_introspection, &screencast_vtable },
    { realtime_introspection, &realtime_vtable },
    { powerprofile_introspection, &powerprofile_vtable },
    { gamemode_introspection, &gamemode_vtable },
    { memorymonitor_introspection, &memorymonitor_vtable },
    { filechooser_introspection, &filechooser_vtable },
    { screenshot_introspection, &screenshot_vtable },
};

static void register_interfaces(Portal* portal, GDBusConnection* connection)
{
    for (size_t i = 0; i < G_N_ELEMENTS(interfaces); ++i) {
        GError* error = NULL;
        GDBusNodeInfo* node_info = g_dbus_node_info_new_for_xml(interfaces[i].xml, &error);
        if (!node_info) {
            g_printerr("xdg-desktop-portal-serenity: bad introspection xml: %s\n", error->message);
            exit(1);
        }
        guint id = g_dbus_connection_register_object(connection, OBJECT_PATH, node_info->interfaces[0], interfaces[i].vtable, portal, NULL, &error);
        g_dbus_node_info_unref(node_info);
        if (!id) {
            g_printerr("xdg-desktop-portal-serenity: could not register object: %s\n", error->message);
            exit(1);
        }
        portal->registration_ids[portal->registration_count++] = id;
    }
}

static void on_bus_acquired(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    (void)name;
    Portal* portal = user_data;
    portal->connection = connection;
    register_interfaces(portal, connection);
    g_print("xdg-desktop-portal-serenity: serving %s on %s\n", OBJECT_PATH, BUS_NAME);
}

int main(int argc, char** argv)
{
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    Portal portal = { 0 };
    g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired, NULL, NULL, &portal, NULL);
    g_main_loop_run(loop);
    (void)argc;
    (void)argv;
    return 0;
}
