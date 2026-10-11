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
#include <gio/gunixfdlist.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "portal-screencast.h"
#include "portal-remote-desktop.h"
#include "portal-global-shortcuts.h"

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

    // Prefer the LibGUI AppChooserDialog (SDE_PORTAL_APP_CHOOSER); otherwise fall
    // back to the first offered application. The frontend passes ordered choices.
    char const* handle = NULL;
    char const* app_id = NULL;
    char const* parent_window = NULL;
    GVariant* choices = NULL;
    GVariant* options = NULL;
    g_variant_get(parameters, "(o&s&s@as@a{sv})", &handle, &app_id, &parent_window, &choices, &options);

    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    guint32 response = RESPONSE_CANCELLED;

    gsize count = choices ? g_variant_n_children(choices) : 0;
    int chosen = -1;
    char const* chooser = getenv("SDE_PORTAL_APP_CHOOSER");
    if (count > 0 && chooser && *chooser) {
        GString* joined = g_string_new("");
        for (gsize i = 0; i < count; ++i) {
            GVariant* child = g_variant_get_child_value(choices, i);
            if (i)
                g_string_append_c(joined, '\n');
            g_string_append(joined, g_variant_get_string(child, NULL));
            g_variant_unref(child);
        }
        char const* argv[] = { chooser, "-t", "Choose an Application", "-c", joined->str, NULL };
        char* out = NULL;
        gint wait_status = 0;
        GError* error = NULL;
        if (g_spawn_sync(NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error)
            && out && g_spawn_check_wait_status(wait_status, NULL))
            chosen = atoi(g_strstrip(out));
        if (out)
            g_free(out);
        if (error)
            g_error_free(error);
        g_string_free(joined, TRUE);
    }

    if (count > 0) {
        if (chosen < 0 || (gsize)chosen >= count)
            chosen = 0; // default to the first offered application
        GVariant* child = g_variant_get_child_value(choices, (gsize)chosen);
        g_variant_builder_add(&results, "{sv}", "choice", g_variant_new_string(g_variant_get_string(child, NULL)));
        g_variant_unref(child);
        response = RESPONSE_SUCCESS;
    }
    if (options)
        g_variant_unref(options);
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
    "    <method name='Activate'>"
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

// Materialise the file passed by fd (OpenFile) into a temporary readable path so
// the opener can be handed a stable file:// URI. Returns a newly allocated path,
// or NULL.
static gchar* open_file_fd_to_temp(int index, GDBusMethodInvocation* invocation)
{
    GDBusMessage* message = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList* fd_list = message ? g_dbus_message_get_unix_fd_list(message) : NULL;
    if (!fd_list)
        return NULL;

    GError* error = NULL;
    gint fd = g_unix_fd_list_get(fd_list, index, &error);
    if (fd < 0) {
        if (error)
            g_error_free(error);
        return NULL;
    }

    gchar* path = g_build_filename(g_get_tmp_dir(), "sde-openfile-XXXXXX", NULL);
    int out = g_mkstemp(path);
    if (out < 0) {
        close(fd);
        g_free(path);
        return NULL;
    }
    lseek(fd, 0, SEEK_SET);
    char buffer[65536];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t written = write(out, buffer + off, (size_t)(n - off));
            if (written <= 0)
                break;
            off += written;
        }
    }
    close(out);
    close(fd);
    return path;
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
    gchar* temp_path = NULL;
    gchar* temp_uri = NULL;

    if (g_strcmp0(method_name, "OpenURI") == 0 || g_strcmp0(method_name, "OpenDirectory") == 0) {
        g_variant_get(parameters, "(o&s&s@a{sv})", NULL, NULL, &uri, NULL);
    } else if (g_strcmp0(method_name, "OpenFile") == 0) {
        // Resolve the handle to a real fd via the message's fd list, materialise
        // it, and open the resulting file:// URI.
        gint32 index = -1;
        g_variant_get(parameters, "(o&s&sh@a{sv})", NULL, NULL, &index, NULL);
        temp_path = open_file_fd_to_temp(index, invocation);
        if (temp_path) {
            temp_uri = g_filename_to_uri(temp_path, NULL, NULL);
            uri = temp_uri;
        }
    } else if (g_strcmp0(method_name, "Activate") == 0) {
        // Activate an installed application by its app id (desktop file id).
        char const* app_id = NULL;
        g_variant_get(parameters, "(o&s&s@a{sv})", NULL, &app_id, NULL, NULL);
        gboolean launched = FALSE;
        if (app_id && *app_id) {
            char const* opener = getenv("SDE_PORTAL_OPENER");
            if (opener && *opener) {
                gchar* argv[] = { (gchar*)opener, (gchar*)app_id, NULL };
                launched = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            } else {
                gchar* argv[] = { (gchar*)"gio", (gchar*)"launch", (gchar*)app_id, NULL };
                launched = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            }
        }
        g_free(temp_path);
        g_free(temp_uri);
        GVariantBuilder results;
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        return_response(invocation, launched ? RESPONSE_SUCCESS : RESPONSE_OTHER_ERROR, &results);
        return;
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    gboolean opened = uri && launch_uri(uri);
    g_free(temp_path);
    g_free(temp_uri);
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    return_response(invocation, opened ? RESPONSE_SUCCESS : RESPONSE_OTHER_ERROR, &results);
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
    "    <signal name='ActionInvoked'>"
    "      <arg type='s' name='id'/>"
    "      <arg type='s' name='action'/>"
    "      <arg type='s' name='parameter'/>"
    "    </signal>"
    "    <signal name='NotificationClosed'>"
    "      <arg type='s' name='id'/>"
    "      <arg type='u' name='reason'/>"
    "    </signal>"
    "    <property name='version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

// Display a notification. SDE_PORTAL_NOTIFIER takes "<id> <title> <body> <icon>"
// as argv (the session points this at the Serenity notify tool); otherwise
// notify-send is used (id and icon are then unused, matching SDE's contract).
static void show_notification(char const* id, char const* title, char const* body, char const* icon)
{
    char const* notifier = getenv("SDE_PORTAL_NOTIFIER");
    if (notifier && *notifier) {
        gchar* argv[] = { (gchar*)notifier, (gchar*)id, (gchar*)title, (gchar*)body, (gchar*)(icon ? icon : ""), NULL };
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
    (void)sender;
    (void)user_data;

    if (g_strcmp0(method_name, "AddNotification") == 0) {
        char const* id = NULL;
        GVariant* notification = NULL;
        g_variant_get(parameters, "(&s@a{sv})", &id, &notification);
        char const* title = NULL;
        char const* body = NULL;
        gchar* icon_name = NULL;
        if (notification) {
            if (!g_variant_lookup(notification, "title", "&s", &title))
                title = "";
            if (!g_variant_lookup(notification, "body", "&s", &body))
                body = "";
            // 'icon' is a serialized (sv): "themed" + a name, or "file-descriptor"
            // + an fd. Pass the themed name through; fd icons are skipped.
            GVariant* icon_variant = g_variant_lookup_value(notification, "icon", NULL);
            if (icon_variant) {
                GVariant* icon_value = NULL;
                if (g_variant_is_of_type(icon_variant, G_VARIANT_TYPE_VARIANT)) {
                    icon_value = g_variant_get_variant(icon_variant);
                    g_variant_unref(icon_variant);
                } else {
                    icon_value = icon_variant;
                }
                if (g_variant_is_of_type(icon_value, G_VARIANT_TYPE("(sv)"))) {
                    char const* kind = NULL;
                    GVariant* value = NULL;
                    g_variant_get(icon_value, "(&s@v)", &kind, &value);
                    if (value) {
                        // The (sv)'s variant carries the themed name (s) or the
                        // fd handle (h); only themed names are passed through.
                        GVariant* inner = g_variant_get_variant(value);
                        if (kind && strcmp(kind, "themed") == 0 && inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING))
                            icon_name = g_strdup(g_variant_get_string(inner, NULL));
                        if (inner)
                            g_variant_unref(inner);
                        g_variant_unref(value);
                    }
                }
                g_variant_unref(icon_value);
            }
        }
        show_notification(id ? id : "", title ? title : "", body ? body : "", icon_name);
        g_free(icon_name);
        if (notification)
            g_variant_unref(notification);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_strcmp0(method_name, "RemoveNotification") == 0) {
        char const* id = NULL;
        g_variant_get(parameters, "(&s)", &id);
        // Tell the client the notification is gone (reason 2 = dismissed).
        g_dbus_connection_emit_signal(connection, NULL, object_path,
            "org.freedesktop.impl.portal.Notification", "NotificationClosed",
            g_variant_new("(su)", id ? id : "", 2), NULL);
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
    "    <method name='SaveFiles'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='as' name='files' direction='in'/>"
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

    if (g_strcmp0(method_name, "OpenFile") != 0 && g_strcmp0(method_name, "SaveFile") != 0 && g_strcmp0(method_name, "SaveFiles") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    char const* handle = NULL;
    char const* app_id = NULL;
    char const* parent_window = NULL;
    char const* title = NULL;
    GVariant* options = NULL;
    GVariant* files = NULL;
    gboolean const is_save_files = g_strcmp0(method_name, "SaveFiles") == 0;
    if (is_save_files)
        g_variant_get(parameters, "(o&s&s&s@a{sv}@as)", &handle, &app_id, &parent_window, &title, &options, &files);
    else
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
    // SaveFiles names the files to save; use the first as the suggested name.
    if (is_save_files && files && g_variant_n_children(files) > 0)
        g_variant_get_child(files, 0, "&s", &current_name);

    char const* chooser = getenv("SDE_PORTAL_FILE_CHOOSER");
    if (!chooser || !*chooser) {
        if (current_folder)
            g_variant_unref(current_folder);
        if (files)
            g_variant_unref(files);
        if (options)
            g_variant_unref(options);
        return_response(invocation, RESPONSE_OTHER_ERROR, &results);
        return;
    }

    char const* mode = (g_strcmp0(method_name, "OpenFile") == 0) ? (directory ? "folder" : "open") : "save";
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
            GVariantBuilder uris;
            g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
            if (gchar* uri = g_filename_to_uri(path, NULL, NULL)) {
                g_variant_builder_add(&uris, "s", uri);
                g_free(uri);
            }
            // SaveFiles: the remaining files are placed next to the chosen one.
            if (is_save_files && files) {
                gchar* dir = g_path_get_dirname(path);
                gsize count = g_variant_n_children(files);
                for (gsize i = 1; i < count; ++i) {
                    char const* name = NULL;
                    g_variant_get_child(files, i, "&s", &name);
                    gchar* joined = g_build_filename(dir, name, NULL);
                    if (gchar* uri = g_filename_to_uri(joined, NULL, NULL)) {
                        g_variant_builder_add(&uris, "s", uri);
                        g_free(uri);
                    }
                    g_free(joined);
                }
                g_free(dir);
            }
            g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
            response = RESPONSE_SUCCESS;
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
    if (files)
        g_variant_unref(files);
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
    "    <method name='PickColor'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// Sample the pixel at (x, y) of the PNG at `path`, returning RGB in 0..1.
// Negative coordinates fall back to the centre of the frame.
static gboolean sample_png_color(char const* path, int x, int y, double* out_r, double* out_g, double* out_b)
{
    GError* error = NULL;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (!pixbuf) {
        if (error)
            g_error_free(error);
        return FALSE;
    }
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);

    if (x < 0 || x >= width)
        x = width / 2;
    if (y < 0 || y >= height)
        y = height / 2;

    guchar const* pixel = pixels + (gsize)y * rowstride + (gsize)x * channels;
    *out_r = pixel[0] / 255.0;
    *out_g = pixel[1] / 255.0;
    *out_b = pixel[2] / 255.0;
    g_object_unref(pixbuf);
    return TRUE;
}

// Capture the screen (reusing the ScreenCast capture) into a PNG and return its
// file:// URI. For PickColor, launch the LibGUI ColorPicker on the frozen frame
// (SDE_PORTAL_COLOR_PICKER), sample the clicked pixel and return it as (ddd).
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

    gboolean const is_pick_color = g_strcmp0(method_name, "PickColor") == 0;
    if (!is_pick_color && g_strcmp0(method_name, "Screenshot") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    char const* runtime = g_get_user_runtime_dir();
    gchar* path = g_strdup_printf("%s/sde-%s-%d.png", runtime ? runtime : "/tmp",
        is_pick_color ? "pickcolor" : "screenshot", (int)getpid());
    int captured = portal_screencast_capture_png(path);
    if (captured != 0) {
        g_free(path);
        return_response(invocation, RESPONSE_OTHER_ERROR, &results);
        return;
    }

    if (!is_pick_color) {
        if (gchar* uri = g_filename_to_uri(path, NULL, NULL)) {
            g_variant_builder_add(&results, "{sv}", "uri", g_variant_new_string(uri));
            g_free(uri);
            g_free(path);
            return_response(invocation, RESPONSE_SUCCESS, &results);
            return;
        }
        g_free(path);
        return_response(invocation, RESPONSE_OTHER_ERROR, &results);
        return;
    }

    int px = -1;
    int py = -1;
    char const* picker = getenv("SDE_PORTAL_COLOR_PICKER");
    if (picker && *picker) {
        char const* argv[] = { picker, "-i", path, NULL };
        char* out = NULL;
        gint wait_status = 0;
        GError* error = NULL;
        if (g_spawn_sync(NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error)
            && out && g_spawn_check_wait_status(wait_status, NULL))
            sscanf(g_strstrip(out), "%d %d", &px, &py);
        if (out)
            g_free(out);
        if (error)
            g_error_free(error);
    }

    double r = 0;
    double g = 0;
    double b = 0;
    gboolean ok = sample_png_color(path, px, py, &r, &g, &b);
    g_free(path);
    if (ok) {
        g_variant_builder_add(&results, "{sv}", "color", g_variant_new("(ddd)", r, g, b));
        return_response(invocation, RESPONSE_SUCCESS, &results);
        return;
    }
    return_response(invocation, RESPONSE_OTHER_ERROR, &results);
}

// --- Print ------------------------------------------------------------------

static const char print_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Print'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='Print'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='h' name='fd' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='PreparePrint'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='a{sv}' name='settings' direction='in'/>"
    "      <arg type='a{sv}' name='page_setup' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// Collect printer names from CUPS (lpstat), else a single "Print to File".
static gchar* collect_printers(void)
{
    gchar* printers = NULL;
    FILE* pipe = popen("lpstat -p 2>/dev/null", "r");
    if (pipe) {
        char line[512];
        GString* list = g_string_new(NULL);
        while (fgets(line, sizeof(line), pipe)) {
            char* space = strchr(line, ' ');
            if (!space)
                continue;
            char* name = space + 1;
            char* end = name;
            while (*end && *end != ' ' && *end != '\n')
                ++end;
            *end = '\0';
            if (*name) {
                if (list->len)
                    g_string_append_c(list, ',');
                g_string_append(list, name);
            }
        }
        pclose(pipe);
        if (list->len)
            printers = g_string_free(list, FALSE);
        else
            g_string_free(list, TRUE);
    }
    if (!printers)
        printers = g_strdup("Print to File");
    return printers;
}

static void handle_print(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
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

    if (g_strcmp0(method_name, "PreparePrint") == 0) {
        char const* handle = NULL;
        char const* app_id = NULL;
        char const* parent_window = NULL;
        char const* title = NULL;
        GVariant* settings = NULL;
        GVariant* page_setup = NULL;
        GVariant* options = NULL;
        g_variant_get(parameters, "(o&s&s&s@a{sv}@a{sv}@a{sv})", &handle, &app_id, &parent_window, &title, &settings, &page_setup, &options);

        char const* dialog = getenv("SDE_PORTAL_PRINT_DIALOG");
        gchar* printers = collect_printers();
        gchar* chosen_printer = g_strdup("");
        guint32 copies = 1;
        guint32 response = RESPONSE_CANCELLED;

        if (dialog && *dialog) {
            char const* argv[8];
            int argc = 0;
            argv[argc++] = dialog;
            argv[argc++] = "-t";
            argv[argc++] = title && *title ? title : "Print";
            argv[argc++] = "-p";
            argv[argc++] = printers;
            argv[argc] = NULL;
            char* out = NULL;
            gint wait_status = 0;
            GError* error = NULL;
            if (g_spawn_sync(NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error)
                && out && g_spawn_check_wait_status(wait_status, NULL)) {
                gchar** tokens = g_strsplit(g_strstrip(out), "\t", 2);
                if (tokens[0] && *tokens[0]) {
                    g_free(chosen_printer);
                    chosen_printer = g_strdup(tokens[0]);
                    if (tokens[1])
                        copies = (guint32)MAX(1, atoi(tokens[1]));
                    response = RESPONSE_SUCCESS;
                }
                g_strfreev(tokens);
            }
            if (out)
                g_free(out);
            if (error)
                g_error_free(error);
        }
        g_free(printers);

        GVariantBuilder out_settings;
        g_variant_builder_init(&out_settings, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&out_settings, "{sv}", "printer", g_variant_new_string(chosen_printer));
        g_variant_builder_add(&out_settings, "{sv}", "copies", g_variant_new_uint32(copies));
        g_variant_builder_add(&results, "{sv}", "settings", g_variant_builder_end(&out_settings));
        GVariantBuilder out_page_setup;
        g_variant_builder_init(&out_page_setup, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&results, "{sv}", "page_setup", g_variant_builder_end(&out_page_setup));
        g_free(chosen_printer);
        if (settings)
            g_variant_unref(settings);
        if (page_setup)
            g_variant_unref(page_setup);
        if (options)
            g_variant_unref(options);
        return_response(invocation, response, &results);
        return;
    }

    if (g_strcmp0(method_name, "Print") == 0) {
        char const* handle = NULL;
        char const* app_id = NULL;
        char const* parent_window = NULL;
        char const* title = NULL;
        gint32 fd_index = 0;
        GVariant* options = NULL;
        g_variant_get(parameters, "(o&s&s&sh@a{sv})", &handle, &app_id, &parent_window, &title, &fd_index, &options);

        int fd = -1;
        GDBusMessage* message = g_dbus_method_invocation_get_message(invocation);
        GUnixFDList* fd_list = message ? g_dbus_message_get_unix_fd_list(message) : NULL;
        if (fd_list)
            fd = g_unix_fd_list_get(fd_list, fd_index, NULL);

        guint32 response = RESPONSE_OTHER_ERROR;
        char const* command = getenv("SDE_PORTAL_PRINT_CMD");
        if (fd >= 0 && command && *command) {
            // Feed the document to the configured print command (e.g. "lp -d x").
            GError* error = NULL;
            gchar* argv[] = { (gchar*)"/bin/sh", (gchar*)"-c", (gchar*)command, NULL };
            gint child_stdin = -1;
            gboolean spawned = g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &child_stdin, NULL, NULL, &error);
            if (spawned && child_stdin >= 0) {
                char buffer[4096];
                ssize_t n;
                while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
                    ssize_t written = 0;
                    while (written < n) {
                        ssize_t w = write(child_stdin, buffer + written, (size_t)(n - written));
                        if (w <= 0)
                            break;
                        written += w;
                    }
                }
                close(child_stdin);
                response = RESPONSE_SUCCESS;
            }
            if (error)
                g_error_free(error);
        }
        if (fd >= 0)
            close(fd);
        if (options)
            g_variant_unref(options);
        return_response(invocation, response, &results);
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

// --- Wallpaper --------------------------------------------------------------

static gchar* portal_state_file(char const* name)
{
    char const* state = getenv("SDE_STATE_DIR");
    gchar* base;
    if (state && *state)
        base = g_strdup(state);
    else {
        char const* config = getenv("XDG_CONFIG_HOME");
        if (config && *config)
            base = g_build_filename(config, "sde", NULL);
        else {
            char const* home = getenv("HOME");
            base = g_build_filename(home ? home : "/tmp", ".config", "sde", NULL);
        }
    }
    g_mkdir_with_parents(base, 0700);
    gchar* path = g_build_filename(base, name, NULL);
    g_free(base);
    return path;
}

// Decode the image at `uri` and write the compositor's wallpaper state
// (wallpaper.raw: {magic, format=BGRA8888, width, height, pitch} + pixels).
static gboolean write_wallpaper_raw(char const* uri)
{
    if (!uri || !g_str_has_prefix(uri, "file://"))
        return FALSE;
    GError* error = NULL;
    gchar* path = g_filename_from_uri(uri, NULL, &error);
    if (error)
        g_error_free(error);
    if (!path)
        return FALSE;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    g_free(path);
    if (!pixbuf)
        return FALSE;

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    int pitch = width * 4;

    struct {
        guint32 magic;
        guint32 format;
        guint32 width;
        guint32 height;
        guint32 pitch;
    } header = { 0x53445750u, 2u /* BGRA8888 */, (guint32)width, (guint32)height, (guint32)pitch };

    gsize total = sizeof(header) + (gsize)pitch * (gsize)height;
    guchar* out = g_malloc(total);
    memcpy(out, &header, sizeof(header));
    for (int y = 0; y < height; ++y) {
        guchar const* src = pixels + (gsize)y * rowstride;
        guchar* dst = out + sizeof(header) + (gsize)y * pitch;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * channels + 2]; // B
            dst[x * 4 + 1] = src[x * channels + 1]; // G
            dst[x * 4 + 2] = src[x * channels + 0]; // R
            dst[x * 4 + 3] = channels == 4 ? src[x * channels + 3] : 0xff;
        }
    }
    gchar* raw_path = portal_state_file("wallpaper.raw");
    gboolean ok = g_file_set_contents(raw_path, (char const*)out, (gssize)total, NULL);
    g_free(raw_path);
    g_free(out);
    g_object_unref(pixbuf);
    if (ok) {
        gchar* mode_path = portal_state_file("wallpaper.mode");
        g_file_set_contents(mode_path, "stretch", -1, NULL);
        g_free(mode_path);
    }
    return ok;
}

static const char wallpaper_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Wallpaper'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='SetWallpaper'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='uri' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void handle_wallpaper(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
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

    if (g_strcmp0(method_name, "SetWallpaper") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    char const* handle = NULL;
    char const* app_id = NULL;
    char const* parent_window = NULL;
    char const* uri = NULL;
    GVariant* options = NULL;
    g_variant_get(parameters, "(o&s&s&s@a{sv})", &handle, &app_id, &parent_window, &uri, &options);
    gboolean ok = write_wallpaper_raw(uri);
    if (options)
        g_variant_unref(options);
    return_response(invocation, ok ? RESPONSE_SUCCESS : RESPONSE_OTHER_ERROR, &results);
}

// --- Secret / USB / DynamicLauncher -----------------------------------------

static gchar* sanitized_app_id(char const* app_id)
{
    gchar* result = g_strdup(app_id && *app_id ? app_id : "default");
    for (gchar* p = result; *p; ++p)
        if (*p == '/' || *p == '\\')
            *p = '_';
    return result;
}

static const char secret_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Secret'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='RetrieveSecret'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='ay' name='secret' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// A stable per-app secret, generated once and kept in the state directory.
static void handle_secret(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "RetrieveSecret") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }
    char const* handle = NULL;
    char const* app_id = NULL;
    GVariant* options = NULL;
    g_variant_get(parameters, "(o&s@a{sv})", &handle, &app_id, &options);

    gchar* name = sanitized_app_id(app_id);
    gchar* dir = portal_state_file("secrets");
    g_mkdir_with_parents(dir, 0700);
    gchar* path = g_build_filename(dir, name, NULL);

    guchar secret[64];
    gsize length = 0;
    gchar* contents = NULL;
    if (g_file_get_contents(path, &contents, &length, NULL) && length == sizeof(secret)) {
        memcpy(secret, contents, sizeof(secret));
        g_free(contents);
    } else {
        if (contents)
            g_free(contents);
        FILE* random = fopen("/dev/urandom", "rb");
        if (!random || fread(secret, 1, sizeof(secret), random) != sizeof(secret)) {
            for (gsize i = 0; i < sizeof(secret); ++i)
                secret[i] = (guchar)g_random_int();
        }
        if (random)
            fclose(random);
        g_file_set_contents(path, (char const*)secret, sizeof(secret), NULL);
    }
    g_free(path);
    g_free(dir);
    g_free(name);
    if (options)
        g_variant_unref(options);

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(u@ay)", RESPONSE_SUCCESS, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, secret, sizeof(secret), 1)));
}

static const char usb_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.USB'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='ListDevices'>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='AcquireDevices'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='a(ua{sv})' name='devices' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='ReleaseDevices'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='o' name='session_handle' direction='in'/>"
    "      <arg type='a(ua{sv})' name='devices' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void handle_usb(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
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
    if (g_strcmp0(method_name, "ListDevices") == 0) {
        // No USB device broker: report an empty list (standards-compliant).
        GVariantBuilder devices;
        g_variant_builder_init(&devices, G_VARIANT_TYPE("a(ua{sv})"));
        g_variant_builder_add(&results, "{sv}", "devices", g_variant_builder_end(&devices));
        return_response(invocation, RESPONSE_SUCCESS, &results);
        return;
    }
    if (g_strcmp0(method_name, "AcquireDevices") == 0 || g_strcmp0(method_name, "ReleaseDevices") == 0) {
        return_response(invocation, RESPONSE_SUCCESS, &results);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

static const char dynamiclauncher_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.DynamicLauncher'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='PrepareInstall'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='name' direction='in'/>"
    "      <arg type='v' name='icon_v' direction='in'/>"
    "      <arg type='h' name='icon_fd' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "    <method name='RequestInstallToken'>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='s' name='token' direction='out'/>"
    "    </method>"
    "    <method name='Launch'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='desktop_file_id' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void handle_dynamic_launcher(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
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

    if (g_strcmp0(method_name, "RequestInstallToken") == 0) {
        gchar token[33];
        for (int i = 0; i < 32; ++i)
            token[i] = "0123456789abcdef"[g_random_int_range(0, 16)];
        token[32] = '\0';
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", token));
        return;
    }
    if (g_strcmp0(method_name, "PrepareInstall") == 0) {
        char const* name = NULL;
        g_variant_get(parameters, "(o&s&s&svh@a{sv})", NULL, NULL, NULL, &name, NULL, NULL, NULL);
        // Echo the requested name; no icon post-processing.
        g_variant_builder_add(&results, "{sv}", "name", g_variant_new_string(name ? name : "Application"));
        return_response(invocation, RESPONSE_SUCCESS, &results);
        return;
    }
    if (g_strcmp0(method_name, "Launch") == 0) {
        char const* desktop_file_id = NULL;
        g_variant_get(parameters, "(o&s&s&s@a{sv})", NULL, NULL, NULL, &desktop_file_id, NULL);
        gboolean launched = FALSE;
        if (desktop_file_id) {
            gchar* argv[] = { (gchar*)"gio", (gchar*)"launch", (gchar*)desktop_file_id, NULL };
            launched = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        }
        return_response(invocation, launched ? RESPONSE_SUCCESS : RESPONSE_OTHER_ERROR, &results);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
}

// --- Access -----------------------------------------------------------------

static const char access_introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.impl.portal.Access'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='AccessDialog'>"
    "      <arg type='o' name='handle' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='s' name='parent_window' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='s' name='subtitle' direction='in'/>"
    "      <arg type='s' name='body' direction='in'/>"
    "      <arg type='a{sv}' name='options' direction='in'/>"
    "      <arg type='u' name='response' direction='out'/>"
    "      <arg type='a{sv}' name='results' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

// A LibGUI permission dialog (AccessDialog) prints 0 (allow) or 1 (deny).
static void handle_access(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
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

    if (g_strcmp0(method_name, "AccessDialog") != 0) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method %s", method_name);
        return;
    }

    char const* handle = NULL;
    char const* app_id = NULL;
    char const* parent_window = NULL;
    char const* title = NULL;
    char const* subtitle = NULL;
    char const* body = NULL;
    GVariant* options = NULL;
    g_variant_get(parameters, "(o&s&s&s&s&s@a{sv})", &handle, &app_id, &parent_window, &title, &subtitle, &body, &options);

    char const* dialog = getenv("SDE_PORTAL_ACCESS_DIALOG");
    guint32 response = RESPONSE_CANCELLED;
    if (dialog && *dialog) {
        char const* argv[10];
        int argc = 0;
        argv[argc++] = dialog;
        argv[argc++] = "-t";
        argv[argc++] = title ? title : "Allow this request?";
        argv[argc++] = "-s";
        argv[argc++] = subtitle ? subtitle : "";
        argv[argc++] = "-b";
        argv[argc++] = body ? body : "";
        argv[argc] = NULL;
        char* out = NULL;
        gint wait_status = 0;
        GError* error = NULL;
        if (g_spawn_sync(NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &wait_status, &error)
            && out && g_spawn_check_wait_status(wait_status, NULL))
            response = (guint32)MAX(0, atoi(g_strstrip(out)));
        if (out)
            g_free(out);
        if (error)
            g_error_free(error);
    }
    if (options)
        g_variant_unref(options);
    return_response(invocation, response, &results);
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
    return generic_version_property(property_name, error, 3); // OpenFile, SaveFile, SaveFiles
}

static GVariant* screenshot_get_property(GDBusConnection* connection, const gchar* sender, const gchar* object_path,
    const gchar* interface_name, const gchar* property_name, GError** error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;
    return generic_version_property(property_name, error, 2); // Screenshot, PickColor
}

static const GDBusInterfaceVTable filechooser_vtable = { handle_filechooser, filechooser_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable screenshot_vtable = { handle_screenshot, screenshot_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable print_vtable = { handle_print, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable wallpaper_vtable = { handle_wallpaper, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable secret_vtable = { handle_secret, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable usb_vtable = { handle_usb, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable dynamic_launcher_vtable = { handle_dynamic_launcher, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable access_vtable = { handle_access, simple_version_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable remote_desktop_vtable = { portal_remote_desktop_method_call, portal_remote_desktop_get_property, NULL, { 0 } };
static const GDBusInterfaceVTable global_shortcuts_vtable = { portal_global_shortcuts_method_call, portal_global_shortcuts_get_property, NULL, { 0 } };

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
    { print_introspection, &print_vtable },
    { wallpaper_introspection, &wallpaper_vtable },
    { secret_introspection, &secret_vtable },
    { usb_introspection, &usb_vtable },
    { dynamiclauncher_introspection, &dynamic_launcher_vtable },
    { access_introspection, &access_vtable },
    { portal_remote_desktop_introspection, &remote_desktop_vtable },
    { portal_global_shortcuts_introspection, &global_shortcuts_vtable },
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
