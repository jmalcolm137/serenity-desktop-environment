/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Drive RemoteDesktop.ConnectToEIS and consume the returned fd with a sender
// libei context. This checks the part gdbus cannot: the fd is a usable EIS
// endpoint, the backend advertises a seat/device, and a sender can emit motion.
//
// It is a test-only client; the portal backend must already be running on the
// session bus and a compositor must be reachable.

#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <libei.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DBusConnection* bus;

static void append_empty_dict(DBusMessageIter* iter)
{
    DBusMessageIter dict;
    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(iter, &dict);
}

static DBusMessage* call(char const* method, bool config_args, char const* session)
{
    DBusMessage* message = dbus_message_new_method_call(
        "org.freedesktop.impl.portal.desktop.serenity",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.RemoteDesktop",
        method);
    if (!message)
        return NULL;
    DBusMessageIter iter;
    dbus_message_iter_init_append(message, &iter);
    char const* request = "/org/freedesktop/portal/desktop/request/1/eis";
    char const* app_id = "org.sde.EISTest";
    char const* parent = "";
    if (strcmp(method, "CreateSession") == 0) {
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &request);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app_id);
        append_empty_dict(&iter);
    } else if (strcmp(method, "SelectDevices") == 0) {
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &request);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app_id);
        append_empty_dict(&iter);
    } else if (strcmp(method, "Start") == 0) {
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &request);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app_id);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent);
        append_empty_dict(&iter);
    } else if (strcmp(method, "ConnectToEIS") == 0) {
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &app_id);
        append_empty_dict(&iter);
    } else {
        dbus_message_unref(message);
        return NULL;
    }
    (void)config_args;
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, message, 5000, &error);
    dbus_message_unref(message);
    if (!reply) {
        fprintf(stderr, "remote-desktop-eis-test: %s failed: %s\n", method, error.message ? error.message : "unknown error");
        dbus_error_free(&error);
        return NULL;
    }
    return reply;
}

static bool check_response(DBusMessage* reply, char const* method)
{
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter)) {
        fprintf(stderr, "remote-desktop-eis-test: %s returned no response\n", method);
        return false;
    }
    dbus_uint32_t response = 2;
    dbus_message_iter_get_basic(&iter, &response);
    if (response != 0) {
        fprintf(stderr, "remote-desktop-eis-test: %s response=%u\n", method, response);
        return false;
    }
    return true;
}

int main(void)
{
    DBusError error;
    dbus_error_init(&error);
    bus = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!bus) {
        fprintf(stderr, "remote-desktop-eis-test: no session bus: %s\n", error.message ? error.message : "unknown error");
        return 1;
    }

    char const* session = "/org/freedesktop/portal/desktop/session/1/eis";
    DBusMessage* reply = call("CreateSession", true, session);
    if (!reply || !check_response(reply, "CreateSession"))
        return 1;
    dbus_message_unref(reply);
    reply = call("SelectDevices", true, session);
    if (!reply || !check_response(reply, "SelectDevices"))
        return 1;
    dbus_message_unref(reply);
    reply = call("Start", true, session);
    if (!reply || !check_response(reply, "Start"))
        return 1;
    dbus_message_unref(reply);
    reply = call("ConnectToEIS", false, session);
    if (!reply) {
        fprintf(stderr, "remote-desktop-eis-test: ConnectToEIS failed\n");
        return 1;
    }
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UNIX_FD) {
        fprintf(stderr, "remote-desktop-eis-test: ConnectToEIS did not return a unix fd\n");
        dbus_message_unref(reply);
        return 1;
    }
    int fd = -1;
    dbus_message_iter_get_basic(&iter, &fd);
    dbus_message_unref(reply);
    if (fd < 0) {
        fprintf(stderr, "remote-desktop-eis-test: invalid EIS fd\n");
        return 1;
    }

    struct ei* ei = ei_new_sender(NULL);
    if (!ei) {
        fprintf(stderr, "remote-desktop-eis-test: ei_new_sender failed\n");
        return 1;
    }
    ei_configure_name(ei, "remote-desktop-eis-test");
    if (ei_setup_backend_fd(ei, fd) < 0) {
        fprintf(stderr, "remote-desktop-eis-test: ei_setup_backend_fd failed: %s\n", strerror(errno));
        return 1;
    }

    struct ei_seat* seat = NULL;
    struct ei_device* pointer = NULL;
    struct ei_device* keyboard = NULL;
    int got_seat = 0;
    int got_pointer = 0;
    int got_keyboard = 0;
    int sent_motion = 0;
    for (int i = 0; i < 150 && !sent_motion; ++i) {
        struct pollfd pfd = { .fd = ei_get_fd(ei), .events = POLLIN };
        if (poll(&pfd, 1, 100) < 0)
            break;
        if (pfd.revents & POLLIN)
            ei_dispatch(ei);
        struct ei_event* event = NULL;
        while ((event = ei_get_event(ei))) {
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                seat = ei_event_get_seat(event);
                got_seat = 1;
                ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_KEYBOARD, NULL);
                ei_seat_request_device_with_capabilities(seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_KEYBOARD, NULL);
                break;
            case EI_EVENT_DEVICE_ADDED:
                if (!pointer && ei_device_has_capability(ei_event_get_device(event), EI_DEVICE_CAP_POINTER)) {
                    pointer = ei_event_get_device(event);
                    got_pointer = 1;
                }
                if (!keyboard && ei_device_has_capability(ei_event_get_device(event), EI_DEVICE_CAP_KEYBOARD)) {
                    keyboard = ei_event_get_device(event);
                    got_keyboard = 1;
                }
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (pointer && keyboard && !sent_motion) {
                    ei_device_start_emulating(pointer, 1);
                    ei_device_start_emulating(keyboard, 1);
                    ei_device_pointer_motion(pointer, 12.0, 7.0);
                    ei_device_button_button(pointer, 0x110, true);
                    ei_device_button_button(pointer, 0x110, false);
                    ei_device_frame(pointer, 0);
                    ei_device_keyboard_key(keyboard, 30, true);
                    ei_device_keyboard_key(keyboard, 30, false);
                    ei_device_frame(keyboard, 0);
                    sent_motion = 1;
                }
                break;
            default:
                break;
            }
            ei_event_unref(event);
        }
    }
    ei_unref(ei);
    printf("remote-desktop-eis-test: seat=%d pointer=%d keyboard=%d motion=%d\n", got_seat, got_pointer, got_keyboard, sent_motion);
    return got_seat && got_pointer && got_keyboard && sent_motion ? 0 : 1;
}
