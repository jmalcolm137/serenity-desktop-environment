/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace XDG {

struct RecentEntry {
    ByteString uri;
    ByteString mime_type;
    ByteString added;
    ByteString modified;
    ByteString visited;
    ByteString app_name;
    ByteString app_exec;
};

// $XDG_DATA_HOME/recently-used.xbel
ByteString recently_used_path();

// Read entries from $XDG_DATA_HOME/recently-used.xbel (newest first, up to max_entries).
Vector<RecentEntry> read_recently_used(size_t max_entries = 50);

// Record or update a URI in $XDG_DATA_HOME/recently-used.xbel (bounded to 200
// entries, written atomically via temp file + rename).
ErrorOr<void> record_recently_used(
    StringView uri_or_path,
    StringView mime_type,
    StringView app_name = "SDE"sv,
    StringView app_exec = "sde-open %u"sv);

}
