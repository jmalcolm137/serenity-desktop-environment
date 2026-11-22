/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace XDG {

class MimeDatabase {
public:
    // Load shared-mime-info files (globs2, aliases, subclasses) from
    // $XDG_DATA_HOME/mime and $XDG_DATA_DIRS/mime.
    static MimeDatabase load();

    // Resolve alias -> canonical MIME type (returns input lowercased if none).
    ByteString canonical_mime_type(StringView mime_type) const;

    // Return transitive superclasses in BFS order (e.g. text/x-c++src -> text/x-csrc -> text/plain).
    Vector<ByteString> superclasses(StringView mime_type) const;

    // Guess MIME type from a filename/path using globs2 (weight, then pattern length).
    ByteString mime_type_for_filename(StringView path_or_filename) const;

    // Resolve MIME type for a file path or URI:
    //   - http://... / mailto:... -> x-scheme-handler/<scheme>
    //   - directory path -> inode/directory
    //   - file://... or local path -> mime_type_for_filename
    ByteString mime_type_for_uri(StringView uri_or_path) const;

private:
    struct GlobRule {
        int weight { 50 };
        ByteString mime_type;
        ByteString pattern;
        bool case_sensitive { false };
    };

    void load_mime_dir(ByteString const& mime_dir);

    Vector<GlobRule> m_globs;
    HashMap<ByteString, ByteString> m_aliases;
    HashMap<ByteString, Vector<ByteString>> m_subclasses;
};

}
