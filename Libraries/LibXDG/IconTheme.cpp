/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>
#include <LibXDG/DesktopEntry.h>
#include <LibXDG/IconTheme.h>

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Vector.h>
#include <LibCore/File.h>

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

namespace XDG {

namespace {

enum class DirType {
    Threshold,
    Scalable,
    Fixed,
};

struct IconDirMeta {
    ByteString subdir;
    int size { 0 };
    int scale { 1 };
    int min_size { 0 };
    int max_size { 0 };
    int threshold { 2 };
    DirType type { DirType::Threshold };
};

struct ThemeMeta {
    Vector<ByteString> inherits;
    Vector<IconDirMeta> directories;
};

static Vector<ByteString> icon_base_dirs()
{
    Vector<ByteString> dirs;
    dirs.append(ByteString::formatted("{}/icons", data_home()));
    dirs.append(ByteString::formatted("{}/.icons", home_directory()));
    for (auto const& d : data_dirs())
        dirs.append(ByteString::formatted("{}/icons", d));
    return dirs;
}

static bool directory_matches_size(IconDirMeta const& dir, int size, int scale)
{
    if (dir.scale != scale)
        return false;
    switch (dir.type) {
    case DirType::Fixed:
        return dir.size == size;
    case DirType::Scalable:
        return size >= dir.min_size && size <= dir.max_size;
    case DirType::Threshold:
        return size >= dir.size - dir.threshold && size <= dir.size + dir.threshold;
    }
    return false;
}

static int directory_size_distance(IconDirMeta const& dir, int size, int scale)
{
    int const eff_size = dir.size * dir.scale;
    int const eff_target = size * scale;
    switch (dir.type) {
    case DirType::Fixed:
        return abs(eff_size - eff_target);
    case DirType::Scalable: {
        int const eff_min = dir.min_size * dir.scale;
        int const eff_max = dir.max_size * dir.scale;
        if (eff_target < eff_min)
            return eff_min - eff_target;
        if (eff_target > eff_max)
            return eff_target - eff_max;
        return 0;
    }
    case DirType::Threshold: {
        int const eff_min = (dir.size - dir.threshold) * dir.scale;
        int const eff_max = (dir.size + dir.threshold) * dir.scale;
        if (eff_target < eff_min)
            return eff_min - eff_target;
        if (eff_target > eff_max)
            return eff_target - eff_max;
        return 0;
    }
    }
    return INT_MAX;
}

static Optional<ThemeMeta> parse_index_theme(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    ThemeMeta meta;
    Vector<ByteString> dir_names;
    HashMap<ByteString, IconDirMeta> dir_map;
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
        auto eq = line.find('=');
        if (!eq.has_value())
            continue;
        auto key = line.substring_view(0, eq.value()).trim_whitespace();
        auto val = line.substring_view(eq.value() + 1).trim_whitespace();

        if (section == "Icon Theme"sv) {
            if (key == "Inherits"sv) {
                for (auto part : val.split_view(',')) {
                    auto trimmed = part.trim_whitespace();
                    if (!trimmed.is_empty())
                        meta.inherits.append(trimmed.to_byte_string());
                }
            } else if (key == "Directories"sv || key == "ScaledDirectories"sv) {
                for (auto part : val.split_view(',')) {
                    auto trimmed = part.trim_whitespace();
                    if (!trimmed.is_empty())
                        dir_names.append(trimmed.to_byte_string());
                }
            }
        } else if (!section.is_empty()) {
            auto& d = dir_map.ensure(section, [&] {
                IconDirMeta m;
                m.subdir = section;
                return m;
            });
            if (key == "Size"sv)
                d.size = val.to_number<int>().value_or(0);
            else if (key == "Scale"sv)
                d.scale = val.to_number<int>().value_or(1);
            else if (key == "MinSize"sv)
                d.min_size = val.to_number<int>().value_or(0);
            else if (key == "MaxSize"sv)
                d.max_size = val.to_number<int>().value_or(0);
            else if (key == "Threshold"sv)
                d.threshold = val.to_number<int>().value_or(2);
            else if (key == "Type"sv) {
                if (val == "Fixed"sv)
                    d.type = DirType::Fixed;
                else if (val == "Scalable"sv)
                    d.type = DirType::Scalable;
                else
                    d.type = DirType::Threshold;
            }
        }
    }

    for (auto const& name : dir_names) {
        if (auto it = dir_map.find(name); it != dir_map.end()) {
            auto d = it->value;
            if (d.min_size == 0)
                d.min_size = d.size;
            if (d.max_size == 0)
                d.max_size = d.size;
            meta.directories.append(move(d));
        }
    }
    return meta;
}

static Optional<ByteString> probe_icon_file(ByteString const& dir, StringView icon_name)
{
    // Prefer PNG (fastest for LibGfx), then SVG.
    for (auto ext : { ".png"sv, ".svg"sv }) {
        auto candidate = ByteString::formatted("{}/{}{}", dir, icon_name, ext);
        if (::access(candidate.characters(), R_OK) == 0)
            return candidate;
    }
    return {};
}

static Optional<ByteString> lookup_in_theme(
    StringView icon_name,
    int desired_size,
    int desired_scale,
    ByteString const& theme_name,
    Vector<ByteString> const& base_dirs,
    HashTable<ByteString>& visited_themes)
{
    if (theme_name.is_empty() || visited_themes.contains(theme_name))
        return {};
    visited_themes.set(theme_name);

    Optional<ThemeMeta> theme_meta;
    for (auto const& base : base_dirs) {
        auto index_path = ByteString::formatted("{}/{}/index.theme", base, theme_name);
        if (::access(index_path.characters(), R_OK) == 0) {
            theme_meta = parse_index_theme(index_path);
            if (theme_meta.has_value())
                break;
        }
    }

    if (theme_meta.has_value()) {
        // Pass 1: exact size/scale match.
        for (auto const& dir : theme_meta->directories) {
            if (!directory_matches_size(dir, desired_size, desired_scale))
                continue;
            for (auto const& base : base_dirs) {
                auto full_dir = ByteString::formatted("{}/{}/{}", base, theme_name, dir.subdir);
                if (auto found = probe_icon_file(full_dir, icon_name); found.has_value())
                    return found;
            }
        }

        // Pass 2: closest size distance.
        int best_distance = INT_MAX;
        Optional<ByteString> best_path;
        for (auto const& dir : theme_meta->directories) {
            int distance = directory_size_distance(dir, desired_size, desired_scale);
            if (distance >= best_distance)
                continue;
            for (auto const& base : base_dirs) {
                auto full_dir = ByteString::formatted("{}/{}/{}", base, theme_name, dir.subdir);
                if (auto found = probe_icon_file(full_dir, icon_name); found.has_value()) {
                    best_distance = distance;
                    best_path = move(found);
                    break;
                }
            }
        }
        if (best_path.has_value())
            return best_path;

        // Pass 3: inherited themes.
        for (auto const& parent : theme_meta->inherits) {
            if (auto found = lookup_in_theme(icon_name, desired_size, desired_scale, parent, base_dirs, visited_themes); found.has_value())
                return found;
        }
    } else {
        // Even without an index.theme (e.g., a minimal hicolor dir), probe standard sizes.
        for (auto subdir : { "16x16/apps"sv, "24x24/apps"sv, "32x32/apps"sv, "48x48/apps"sv, "64x64/apps"sv, "128x128/apps"sv, "scalable/apps"sv }) {
            for (auto const& base : base_dirs) {
                auto full_dir = ByteString::formatted("{}/{}/{}", base, theme_name, subdir);
                if (auto found = probe_icon_file(full_dir, icon_name); found.has_value())
                    return found;
            }
        }
    }

    return {};
}

}

Optional<ByteString> find_icon(
    StringView icon_name,
    int desired_size,
    int desired_scale,
    StringView theme_name)
{
    auto trimmed = icon_name.trim_whitespace();
    if (trimmed.is_empty())
        return {};

    // Direct path or file:// URI.
    if (trimmed.starts_with("file://"sv)) {
        auto local = trimmed.substring_view(7).to_byte_string();
        if (::access(local.characters(), R_OK) == 0)
            return local;
        return {};
    }
    if (trimmed.starts_with('/')) {
        auto local = trimmed.to_byte_string();
        if (::access(local.characters(), R_OK) == 0)
            return local;
        return {};
    }

    auto base_dirs = icon_base_dirs();
    HashTable<ByteString> visited;

    if (!theme_name.is_empty()) {
        if (auto found = lookup_in_theme(trimmed, desired_size, desired_scale, theme_name.to_byte_string(), base_dirs, visited); found.has_value())
            return found;
    }

    // Spec requirement: always fall back to "hicolor".
    if (!visited.contains("hicolor")) {
        if (auto found = lookup_in_theme(trimmed, desired_size, desired_scale, "hicolor", base_dirs, visited); found.has_value())
            return found;
    }

    // Final fallback: /usr/share/pixmaps (and $XDG_DATA_DIRS/pixmaps).
    for (auto const& d : data_dirs()) {
        if (auto found = probe_icon_file(ByteString::formatted("{}/pixmaps", d), trimmed); found.has_value())
            return found;
    }
    if (auto found = probe_icon_file("/usr/share/pixmaps", trimmed); found.has_value())
        return found;

    return {};
}

Optional<ByteString> find_app_icon(
    StringView app_id,
    int desired_size,
    int desired_scale,
    StringView theme_name)
{
    auto trimmed = app_id.trim_whitespace();
    if (trimmed.is_empty())
        return {};

    // 1. Try <app_id>.desktop (and lowercase variant, plus serenity-<app_id>.desktop).
    auto try_desktop = [&](ByteString const& desktop_id) -> Optional<ByteString> {
        auto entry = find_desktop_entry(desktop_id);
        if (!entry.has_value() || entry->icon.is_empty())
            return {};
        return find_icon(entry->icon, desired_size, desired_scale, theme_name);
    };

    if (trimmed.ends_with(".desktop"sv)) {
        if (auto found = try_desktop(trimmed.to_byte_string()); found.has_value())
            return found;
    } else {
        if (auto found = try_desktop(ByteString::formatted("{}.desktop", trimmed)); found.has_value())
            return found;
        auto lower = trimmed.to_byte_string().to_lowercase();
        if (lower != trimmed) {
            if (auto found = try_desktop(ByteString::formatted("{}.desktop", lower)); found.has_value())
                return found;
        }
        if (auto found = try_desktop(ByteString::formatted("serenity-{}.desktop", trimmed)); found.has_value())
            return found;
    }

    // 2. Fall back to looking up the app_id directly as an icon name.
    if (auto found = find_icon(trimmed, desired_size, desired_scale, theme_name); found.has_value())
        return found;
    auto lower = trimmed.to_byte_string().to_lowercase();
    if (lower != trimmed)
        return find_icon(lower, desired_size, desired_scale, theme_name);

    return {};
}

}
