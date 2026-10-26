/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Test-only D-Bus reader for the Clipboard portal. It creates a RemoteDesktop
// session, opts into clipboard access, then reads text/plain through
// Clipboard.SelectionRead and verifies the bytes supplied by
// clipboard-owner-test.c.
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DBusConnection* bus;

static DBusMessage* call(char const* interface, char const* method, DBusMessage** reply, char const* session)
{
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* message = dbus_message_new_method_call(
        "org.freedesktop.impl.portal.desktop.serenity",
        "/org/freedesktop/portal/desktop", interface, method);
    if (!message)
        return NULL;
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    char const* app = "org.sde.ClipboardTest";
    if (strcmp(interface, "org.freedesktop.impl.portal.RemoteDesktop") == 0) {
        char const* request = "/org/freedesktop/portal/desktop/request/1/clip_read";
        char const* parent = "";
        if (strcmp(method, "CreateSession") == 0) {
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &request);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app);
        } else if (strcmp(method, "Start") == 0) {
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &request);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent);
        } else {
            dbus_message_unref(message);
            return NULL;
        }
        DBusMessageIter dict;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_close_container(&iter, &dict);
    } else {
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        if (strcmp(method, "SelectionRead") == 0) {
            char const* mime = "text/plain";
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &mime);
        } else {
            DBusMessageIter dict;
            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
            dbus_message_iter_close_container(&iter, &dict);
        }
    }
    *reply = dbus_connection_send_with_reply_and_block(bus, message, 5000, &error);
    dbus_message_unref(message);
    if (!*reply) {
        fprintf(stderr, "clipboard-read-test: %s failed: %s\n", method, error.message ? error.message : "unknown");
        dbus_error_free(&error);
        return NULL;
    }
    return *reply;
}

int main(void)
{
    DBusError error;
    dbus_error_init(&error);
    bus = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!bus) {
        fprintf(stderr, "clipboard-read-test: no session bus\n");
        return 1;
    }
    char const* session = "/org/freedesktop/portal/desktop/session/1/clip_read";
    DBusMessage* reply = NULL;
    if (!call("org.freedesktop.impl.portal.RemoteDesktop", "CreateSession", &reply, session))
        return 1;
    dbus_message_unref(reply);
    if (!call("org.freedesktop.impl.portal.Clipboard", "RequestClipboard", &reply, session))
        return 1;
    dbus_message_unref(reply);
    if (!call("org.freedesktop.impl.portal.RemoteDesktop", "Start", &reply, session))
        return 1;
    dbus_message_unref(reply);
    if (!call("org.freedesktop.impl.portal.Clipboard", "SelectionRead", &reply, session))
        return 1;
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UNIX_FD) {
        fprintf(stderr, "clipboard-read-test: SelectionRead did not return an fd\n");
        dbus_message_unref(reply);
        return 1;
    }
    int fd = -1;
    dbus_message_iter_get_basic(&iter, &fd);
    dbus_message_unref(reply);
    char buffer[32] = { };
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n != 19 || memcmp(buffer, "clipboard-portal-ok", 19) != 0) {
        fprintf(stderr, "clipboard-read-test: wrong contents (%zd bytes: '%s')\n", n, buffer);
        return 1;
    }
    printf("clipboard-read-test: read '%s'\n", buffer);
    return 0;
}
