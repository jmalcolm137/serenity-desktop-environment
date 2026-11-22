/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibXDG/DesktopEntry.h>
#include <LibXDG/MimeDatabase.h>

namespace XDG {

// freedesktop.org MIME Applications Associations Specification (v1.0.1).
//
// Resolves default and associated .desktop applications for a MIME type across
// $XDG_CONFIG_HOME, $XDG_CONFIG_DIRS, $XDG_DATA_HOME/applications and
// $XDG_DATA_DIRS/applications, honouring per-desktop ($desktop-mimeapps.list),
// [Default Applications], [Added Associations], [Removed Associations],
// .desktop MimeType= declarations, and shared-mime-info subclass inheritance.

Vector<ByteString> mimeapps_search_paths(Optional<Vector<ByteString>> desktops_override = {});

Optional<DesktopEntry> default_application_for_mime(
    StringView mime_type,
    MimeDatabase const& mime_db,
    Optional<Vector<ByteString>> desktops_override = {});

Vector<DesktopEntry> associated_applications_for_mime(
    StringView mime_type,
    MimeDatabase const& mime_db,
    Optional<Vector<ByteString>> desktops_override = {});

// Persist a user default in $XDG_CONFIG_HOME/mimeapps.list under
// [Default Applications].
ErrorOr<void> set_default_application_for_mime(StringView mime_type, StringView desktop_id);

}
