/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>
#include <LibXDG/SoundTheme.h>

#include <AK/HashTable.h>
#include <AK/Vector.h>
#include <LibCore/File.h>

#include <unistd.h>

namespace XDG {

namespace {

struct SoundThemeMeta {
    Vector<ByteString> inherits;
    Vector<ByteString> directories;
};

static Vector<ByteString> sound_base_dirs()
{
    Vector<ByteString> dirs;
    dirs.append(ByteString::formatted("{}/sounds", data_home()));
    for (auto const& d : data_dirs())
        dirs.append(ByteString::formatted("{}/sounds", d));
    return dirs;
}

static Optional<SoundThemeMeta> parse_sound_index(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    SoundThemeMeta meta;
    ByteString section;
    StringView text { contents.value() };
    for (auto raw_line : text.lines()) {
        auto line = raw_line.trim_whitespace();
        if (line.is_empty() || line.starts_with('#'))
            continue;
        if (line.starts_with('[') && line.ends_with(']')) {
            section = line.substring_view(1, line.length() - 2).to_byte_string();
            continue;
        }
        if (section != "Sound Theme"sv)
            continue;
        auto eq = line.find('=');
        if (!eq.has_value())
            continue;
        auto key = line.substring_view(0, eq.value()).trim_whitespace();
        auto val = line.substring_view(eq.value() + 1).trim_whitespace();
        if (key == "Inherits"sv) {
            for (auto part : val.split_view(',')) {
                auto trimmed = part.trim_whitespace();
                if (!trimmed.is_empty())
                    meta.inherits.append(trimmed.to_byte_string());
            }
        } else if (key == "Directories"sv) {
            for (auto part : val.split_view(',')) {
                auto trimmed = part.trim_whitespace();
                if (!trimmed.is_empty())
                    meta.directories.append(trimmed.to_byte_string());
            }
        }
    }
    return meta;
}

static Optional<ByteString> probe_sound_file(ByteString const& dir, StringView name)
{
    for (auto ext : { ".oga"sv, ".ogg"sv, ".wav"sv }) {
        auto candidate = ByteString::formatted("{}/{}{}", dir, name, ext);
        if (::access(candidate.characters(), R_OK) == 0)
            return candidate;
    }
    return {};
}

static Optional<ByteString> lookup_sound_in_theme(
    StringView event_name,
    ByteString const& theme_name,
    Vector<ByteString> const& base_dirs,
    HashTable<ByteString>& visited)
{
    if (theme_name.is_empty() || visited.contains(theme_name))
        return {};
    visited.set(theme_name);

    Optional<SoundThemeMeta> meta;
    for (auto const& base : base_dirs) {
        auto index_path = ByteString::formatted("{}/{}/index.theme", base, theme_name);
        if (::access(index_path.characters(), R_OK) == 0) {
            meta = parse_sound_index(index_path);
            if (meta.has_value())
                break;
        }
    }

    Vector<ByteString> subdirs;
    if (meta.has_value() && !meta->directories.is_empty()) {
        subdirs = meta->directories;
    } else {
        subdirs.append("stereo");
        subdirs.append(".");
    }

    for (auto const& sub : subdirs) {
        for (auto const& base : base_dirs) {
            auto dir = sub == "."sv
                ? ByteString::formatted("{}/{}", base, theme_name)
                : ByteString::formatted("{}/{}/{}", base, theme_name, sub);
            if (auto found = probe_sound_file(dir, event_name); found.has_value())
                return found;
        }
    }

    if (meta.has_value()) {
        for (auto const& parent : meta->inherits) {
            if (auto found = lookup_sound_in_theme(event_name, parent, base_dirs, visited); found.has_value())
                return found;
        }
    }
    return {};
}

}

Optional<ByteString> find_sound(StringView event_name, StringView theme_name)
{
    auto trimmed = event_name.trim_whitespace();
    if (trimmed.is_empty())
        return {};

    auto base_dirs = sound_base_dirs();

    // Spec fallback chain: "message-new-instant" -> "message-new" -> "message".
    StringView candidate = trimmed;
    for (;;) {
        HashTable<ByteString> visited;
        if (!theme_name.is_empty()) {
            if (auto found = lookup_sound_in_theme(candidate, theme_name.to_byte_string(), base_dirs, visited); found.has_value())
                return found;
        }
        if (!visited.contains("freedesktop")) {
            if (auto found = lookup_sound_in_theme(candidate, "freedesktop", base_dirs, visited); found.has_value())
                return found;
        }
        auto dash = candidate.find_last('-');
        if (!dash.has_value() || dash.value() == 0)
            break;
        candidate = candidate.substring_view(0, dash.value());
    }

    return {};
}

}
