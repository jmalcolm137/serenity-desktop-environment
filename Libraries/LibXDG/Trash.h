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

struct TrashEntry {
    ByteString id; // Filename inside Trash/files/ (matches info/<id>.trashinfo)
    ByteString original_path;
    ByteString trashed_file_path;
    ByteString deletion_date; // YYYY-MM-DDTHH:MM:SS
};

// $XDG_DATA_HOME/Trash
ByteString trash_directory();

// Move `path` to $XDG_DATA_HOME/Trash per the freedesktop.org Trash spec (v1.0).
// Returns the assigned trash entry id (basename in Trash/files/).
ErrorOr<ByteString> trash_file(StringView path);

// List all entries currently in $XDG_DATA_HOME/Trash.
Vector<TrashEntry> list_trash();

// Restore a trashed entry by id back to its recorded original path.
ErrorOr<void> restore_trashed_file(StringView id);

// Permanently remove all files and metadata from $XDG_DATA_HOME/Trash.
ErrorOr<void> empty_trash();

}
