/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <gio/gio.h>

// R5 ScreenCast backend (org.freedesktop.impl.portal.ScreenCast): captures an
// output via ext-image-copy-capture-v1 and publishes a PipeWire video stream the
// frontend hands to the client. The main backend registers this interface.
extern const char portal_screencast_introspection[];

void portal_screencast_method_call(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* method_name, GVariant* parameters,
    GDBusMethodInvocation* invocation, gpointer user_data);

GVariant* portal_screencast_get_property(GDBusConnection* connection, gchar const* sender, gchar const* object_path,
    gchar const* interface_name, gchar const* property_name, GError** error, gpointer user_data);

// One-shot capture of the primary output into a PNG at `path`; returns 0 on
// success. Used by the Screenshot portal.
int portal_screencast_capture_png(char const* path);

// Start the capture for an existing ScreenCast session (identified by its object
// path) and report the resulting PipeWire stream. Used by the RemoteDesktop
// portal, which pairs input with the ScreenCast video. Returns 0 on success.
int portal_screencast_start_for_session(char const* session_handle, guint32* node_id, int* width, int* height, guint32* source_type);

// Restore-token persistence (screencast-tokens in the state directory), shared
// with the RemoteDesktop portal.
void portal_restore_token_store(char const* token, guint32 types, guint32 cursor_mode, char const* source_id);
gboolean portal_restore_token_load(char const* token, guint32* types, guint32* cursor_mode, char** source_id);
