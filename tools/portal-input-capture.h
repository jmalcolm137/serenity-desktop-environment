/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <gio/gio.h>

// R5 InputCapture backend (org.freedesktop.impl.portal.InputCapture): bridges
// the freedesktop InputCapture portal to the compositor's compositor-hosted EIS
// implementation over the private serenity_input_capture protocol
// (docs/17-input-capture.md). The app receives captured input over EIS; this
// backend only relays session control and the EIS file descriptor.
extern const char portal_input_capture_introspection[];

void portal_input_capture_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data);

GVariant* portal_input_capture_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data);
