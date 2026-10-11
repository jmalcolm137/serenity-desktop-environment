/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <gio/gio.h>

// R5 GlobalShortcuts backend (org.freedesktop.impl.portal.GlobalShortcuts): the
// requested shortcuts are stored in the SDE config system (global-shortcuts.ini)
// for the compositor to bind, and activations come back over a session FIFO
// which this backend turns into the Activated signal.
extern const char portal_global_shortcuts_introspection[];
extern const char portal_global_shortcuts_interface_name[];

void portal_global_shortcuts_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data);

GVariant* portal_global_shortcuts_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data);
