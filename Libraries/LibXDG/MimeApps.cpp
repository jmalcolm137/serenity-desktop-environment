/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>
#include <LibXDG/MimeApps.h>

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>

#include <unistd.h>

namespace XDG {

struct MimeAppsFile {
    HashMap<ByteString, Vector<ByteString>> defaults;
    HashMap<ByteString, Vector<ByteString>> added;
    HashMap<ByteString, Vector<ByteString>> removed;
};

static Vector<ByteString> split_desktop_ids(StringView value)
{
    Vector<ByteString> ids;
    for (auto part : value.split_view(';')) {
        auto trimmed = part.trim_whitespace();
        if (!trimmed.is_empty())
            ids.append(trimmed.to_byte_string());
    }
    return ids;
}

static Optional<MimeAppsFile> parse_mimeapps_file(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    MimeAppsFile parsed;
    StringView text { contents.value() };
    ByteString section;

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
        auto mime = line.substring_view(0, eq.value()).trim_whitespace().to_byte_string().to_lowercase();
        auto ids = split_desktop_ids(line.substring_view(eq.value() + 1));
        if (mime.is_empty() || ids.is_empty())
            continue;

        HashMap<ByteString, Vector<ByteString>>* target = nullptr;
        if (section == "Default Applications"sv)
            target = &parsed.defaults;
        else if (section == "Added Associations"sv)
            target = &parsed.added;
        else if (section == "Removed Associations"sv)
            target = &parsed.removed;

        if (!target)
            continue;
        auto& list = target->ensure(mime);
        for (auto& id : ids) {
            if (!list.contains_slow(id))
                list.append(move(id));
        }
    }
    return parsed;
}

Vector<ByteString> mimeapps_search_paths(Optional<Vector<ByteString>> desktops_override)
{
    auto desktops = desktops_override.has_value() ? desktops_override.value() : current_desktops();
    Vector<ByteString> lower_desktops;
    for (auto const& d : desktops)
        lower_desktops.append(d.to_lowercase());

    Vector<ByteString> paths;
    auto append_dir = [&](ByteString const& dir) {
        for (auto const& d : lower_desktops)
            paths.append(ByteString::formatted("{}/{}-mimeapps.list", dir, d));
        paths.append(ByteString::formatted("{}/mimeapps.list", dir));
    };

    append_dir(config_home());
    for (auto const& dir : config_dirs())
        append_dir(dir);
    append_dir(ByteString::formatted("{}/applications", data_home()));
    for (auto const& dir : data_dirs())
        append_dir(ByteString::formatted("{}/applications", dir));

    return paths;
}

static Optional<DesktopEntry> resolve_default_for_exact_mime(
    ByteString const& mime,
    Vector<MimeAppsFile> const& files,
    Vector<ByteString> const& desktops)
{
    for (auto const& file : files) {
        auto it = file.defaults.find(mime);
        if (it == file.defaults.end())
            continue;
        for (auto const& id : it->value) {
            if (auto entry = find_desktop_entry(id); entry.has_value() && entry->is_available_on(desktops))
                return entry;
        }
    }
    return {};
}

static Vector<DesktopEntry> resolve_associated_for_exact_mime(
    ByteString const& mime,
    Vector<MimeAppsFile> const& files,
    Vector<DesktopEntry> const& all_entries,
    Vector<ByteString> const& desktops)
{
    HashTable<ByteString> blacklist;
    HashTable<ByteString> seen;
    Vector<DesktopEntry> result;

    auto try_add_id = [&](ByteString const& id) {
        if (blacklist.contains(id) || seen.contains(id))
            return;
        if (auto entry = find_desktop_entry(id); entry.has_value() && entry->is_available_on(desktops)) {
            seen.set(id);
            result.append(entry.release_value());
        }
    };

    // Spec order: [Default Applications] first, then [Added Associations] while
    // accumulating [Removed Associations] from higher-priority files.
    for (auto const& file : files) {
        if (auto it = file.defaults.find(mime); it != file.defaults.end()) {
            for (auto const& id : it->value)
                try_add_id(id);
        }
    }

    for (auto const& file : files) {
        if (auto it = file.added.find(mime); it != file.added.end()) {
            for (auto const& id : it->value)
                try_add_id(id);
        }
        if (auto it = file.removed.find(mime); it != file.removed.end()) {
            for (auto const& id : it->value)
                blacklist.set(id);
        }
    }

    // Finally, .desktop files declaring MimeType= containing this MIME type.
    for (auto const& entry : all_entries) {
        if (blacklist.contains(entry.id) || seen.contains(entry.id))
            continue;
        if (!entry.is_available_on(desktops))
            continue;
        for (auto const& declared : entry.mime_types) {
            if (declared.equals_ignoring_ascii_case(mime)) {
                seen.set(entry.id);
                result.append(entry);
                break;
            }
        }
    }

    return result;
}

static Vector<MimeAppsFile> load_mimeapps_files(Optional<Vector<ByteString>> const& desktops_override)
{
    Vector<MimeAppsFile> files;
    for (auto const& path : mimeapps_search_paths(desktops_override)) {
        if (::access(path.characters(), R_OK) != 0)
            continue;
        if (auto parsed = parse_mimeapps_file(path); parsed.has_value())
            files.append(parsed.release_value());
    }
    return files;
}

Optional<DesktopEntry> default_application_for_mime(
    StringView mime_type,
    MimeDatabase const& mime_db,
    Optional<Vector<ByteString>> desktops_override)
{
    auto desktops = desktops_override.has_value() ? desktops_override.value() : current_desktops();
    auto files = load_mimeapps_files(desktops);

    Vector<ByteString> chain;
    chain.append(mime_db.canonical_mime_type(mime_type));
    for (auto& parent : mime_db.superclasses(chain[0]))
        chain.append(move(parent));

    // 1. Explicit [Default Applications] along the MIME inheritance chain.
    for (auto const& candidate_mime : chain) {
        if (auto found = resolve_default_for_exact_mime(candidate_mime, files, desktops); found.has_value())
            return found;
    }

    // 2. Fall back to the highest-priority associated application.
    auto all_entries = all_desktop_entries();
    for (auto const& candidate_mime : chain) {
        auto assoc = resolve_associated_for_exact_mime(candidate_mime, files, all_entries, desktops);
        if (!assoc.is_empty())
            return assoc[0];
    }

    return {};
}

Vector<DesktopEntry> associated_applications_for_mime(
    StringView mime_type,
    MimeDatabase const& mime_db,
    Optional<Vector<ByteString>> desktops_override)
{
    auto desktops = desktops_override.has_value() ? desktops_override.value() : current_desktops();
    auto files = load_mimeapps_files(desktops);
    auto all_entries = all_desktop_entries();

    Vector<ByteString> chain;
    chain.append(mime_db.canonical_mime_type(mime_type));
    for (auto& parent : mime_db.superclasses(chain[0]))
        chain.append(move(parent));

    HashTable<ByteString> seen;
    Vector<DesktopEntry> result;
    for (auto const& candidate_mime : chain) {
        for (auto& entry : resolve_associated_for_exact_mime(candidate_mime, files, all_entries, desktops)) {
            if (seen.contains(entry.id))
                continue;
            seen.set(entry.id);
            result.append(move(entry));
        }
    }
    return result;
}

ErrorOr<void> set_default_application_for_mime(StringView mime_type, StringView desktop_id)
{
    auto cfg_dir = config_home();
    TRY(Core::Directory::create(cfg_dir, Core::Directory::CreateDirectories::Yes));
    auto path = ByteString::formatted("{}/mimeapps.list", cfg_dir);

    auto target_mime = mime_type.trim_whitespace().to_byte_string().to_lowercase();
    auto target_id = desktop_id.trim_whitespace().to_byte_string();
    auto replacement_line = ByteString::formatted("{}={};", target_mime, target_id);

    Vector<ByteString> lines;
    if (auto existing = Core::File::open(path, Core::File::OpenMode::Read); !existing.is_error()) {
        if (auto bytes = existing.value()->read_until_eof(); !bytes.is_error()) {
            StringView text { bytes.value() };
            for (auto line : text.lines())
                lines.append(line.to_byte_string());
        }
    }

    bool in_defaults = false;
    bool saw_defaults = false;
    bool replaced = false;
    size_t insert_pos = lines.size();

    for (size_t i = 0; i < lines.size(); ++i) {
        auto trimmed = lines[i].view().trim_whitespace();
        if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
            if (in_defaults && !replaced)
                insert_pos = i;
            in_defaults = (trimmed == "[Default Applications]"sv);
            if (in_defaults)
                saw_defaults = true;
            continue;
        }
        if (in_defaults) {
            insert_pos = i + 1;
            if (auto eq = trimmed.find('='); eq.has_value()) {
                auto key = trimmed.substring_view(0, eq.value()).trim_whitespace().to_byte_string().to_lowercase();
                if (key == target_mime) {
                    lines[i] = replacement_line;
                    replaced = true;
                }
            }
        }
    }

    if (!saw_defaults) {
        if (!lines.is_empty() && !lines.last().is_empty())
            lines.append("");
        lines.append("[Default Applications]");
        lines.append(replacement_line);
    } else if (!replaced) {
        lines.insert(insert_pos, replacement_line);
    }

    StringBuilder out;
    for (auto const& line : lines) {
        out.append(line);
        out.append('\n');
    }

    auto out_file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(out_file->write_until_depleted(out.string_view().bytes()));
    return {};
}

}
