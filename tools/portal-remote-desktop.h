/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <gio/gio.h>

// R5 RemoteDesktop backend (org.freedesktop.impl.portal.RemoteDesktop): injects
// pointer/keyboard input into the compositor via wlr-virtual-pointer and
// virtual-keyboard, and returns the paired ScreenCast stream.
extern const char portal_remote_desktop_introspection[];

void portal_remote_desktop_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data);

GVariant* portal_remote_desktop_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data);
