/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/Autostart.h>
#include <LibXDG/BaseDirs.h>
#include <LibXDG/DesktopEntry.h>

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace XDG {

static Vector<ByteString> split_semicolon_list(StringView value)
{
    Vector<ByteString> out;
    for (auto item : value.split_view(';')) {
        auto trimmed = item.trim_whitespace();
        if (!trimmed.is_empty())
            out.append(trimmed.to_byte_string());
    }
    return out;
}

static Vector<ByteString> tokenize_exec_raw(StringView exec_line)
{
    Vector<ByteString> raw_args;
    StringBuilder current;
    bool in_quotes = false;
    bool has_token = false;

    for (size_t i = 0; i < exec_line.length(); ++i) {
        char c = exec_line[i];
        if (in_quotes) {
            if (c == '"') {
                in_quotes = false;
            } else if (c == '\\' && i + 1 < exec_line.length()) {
                char next = exec_line[i + 1];
                if (next == '"' || next == '`' || next == '$' || next == '\\') {
                    current.append(next);
                    ++i;
                } else {
                    current.append(c);
                }
            } else {
                current.append(c);
            }
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (has_token) {
                raw_args.append(current.to_byte_string());
                current.clear();
                has_token = false;
            }
            continue;
        }

        has_token = true;
        if (c == '"') {
            in_quotes = true;
        } else if (c == '\\' && i + 1 < exec_line.length()) {
            current.append(exec_line[++i]);
        } else {
            current.append(c);
        }
    }
    if (has_token)
        raw_args.append(current.to_byte_string());
    return raw_args;
}

static ByteString uri_to_local_path(ByteString const& arg)
{
    if (arg.starts_with("file://"sv))
        return arg.substring(7);
    return arg;
}

static ByteString path_to_uri(ByteString const& arg)
{
    if (arg.contains("://"sv))
        return arg;
    if (arg.starts_with('/'))
        return ByteString::formatted("file://{}", arg);
    return arg;
}

bool DesktopEntry::is_available_on(Vector<ByteString> const& desktops) const
{
    if (hidden)
        return false;
    if (!type.is_empty() && type != "Application"sv)
        return false;
    if (!only_show_in.is_empty()) {
        bool matched = false;
        for (auto const& allowed : only_show_in) {
            for (auto const& d : desktops) {
                if (allowed == d) {
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
        }
        if (!matched)
            return false;
    }
    if (!not_show_in.is_empty()) {
        for (auto const& excluded : not_show_in) {
            for (auto const& d : desktops) {
                if (excluded == d)
                    return false;
            }
        }
    }
    if (!try_exec.is_empty() && !try_exec_available(try_exec))
        return false;
    return !exec.is_empty() || dbus_activatable;
}

Vector<ByteString> DesktopEntry::expand_exec(Vector<ByteString> const& args) const
{
    auto raw_tokens = tokenize_exec_raw(exec);
    Vector<ByteString> argv;
    bool consumed_args = false;

    for (auto const& token : raw_tokens) {
        if (token == "%F"sv) {
            for (auto const& arg : args)
                argv.append(uri_to_local_path(arg));
            consumed_args = true;
            continue;
        }
        if (token == "%U"sv) {
            for (auto const& arg : args)
                argv.append(path_to_uri(arg));
            consumed_args = true;
            continue;
        }
        if (token == "%f"sv) {
            if (!args.is_empty())
                argv.append(uri_to_local_path(args[0]));
            consumed_args = true;
            continue;
        }
        if (token == "%u"sv) {
            if (!args.is_empty())
                argv.append(path_to_uri(args[0]));
            consumed_args = true;
            continue;
        }
        if (token == "%i"sv) {
            if (!icon.is_empty()) {
                argv.append("--icon");
                argv.append(icon);
            }
            continue;
        }
        if (token == "%c"sv) {
            argv.append(name);
            continue;
        }
        if (token == "%k"sv) {
            argv.append(file_path);
            continue;
        }
        if (token.length() == 2 && token[0] == '%' && token[1] != '%') {
            // Deprecated field codes (%d, %D, %n, %N, %v, %m) are ignored.
            continue;
        }

        // Inline expansions (e.g. --file=%f or 100%%).
        StringBuilder out;
        for (size_t i = 0; i < token.length(); ++i) {
            if (token[i] == '%' && i + 1 < token.length()) {
                char code = token[++i];
                if (code == '%') {
                    out.append('%');
                } else if (code == 'f' && !args.is_empty()) {
                    out.append(uri_to_local_path(args[0]));
                    consumed_args = true;
                } else if (code == 'u' && !args.is_empty()) {
                    out.append(path_to_uri(args[0]));
                    consumed_args = true;
                } else if (code == 'c') {
                    out.append(name);
                } else if (code == 'k') {
                    out.append(file_path);
                }
            } else {
                out.append(token[i]);
            }
        }
        auto expanded = out.to_byte_string();
        if (!expanded.is_empty())
            argv.append(move(expanded));
    }

    // If Exec had no %f/%F/%u/%U field code, append any supplied arguments.
    if (!consumed_args) {
        for (auto const& arg : args)
            argv.append(arg);
    }
    return argv;
}

Optional<DesktopEntry> parse_desktop_entry(ByteString const& id, ByteString const& file_path)
{
    auto file = Core::File::open(file_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    DesktopEntry entry;
    entry.id = id;
    entry.file_path = file_path;

    StringView text { contents.value() };
    ByteString current_group;
    bool saw_desktop_entry = false;
    Vector<ByteString> action_order;
    HashMap<ByteString, DesktopAction> action_map;

    for (auto raw_line : text.lines()) {
        auto line = raw_line.trim_whitespace();
        if (line.is_empty() || line.starts_with('#'))
            continue;
        if (line.starts_with('[') && line.ends_with(']')) {
            current_group = line.substring_view(1, line.length() - 2).to_byte_string();
            if (current_group == "Desktop Entry"sv)
                saw_desktop_entry = true;
            continue;
        }
        auto eq = line.find('=');
        if (!eq.has_value())
            continue;
        auto key = line.substring_view(0, eq.value()).trim_whitespace();
        auto val = line.substring_view(eq.value() + 1).trim_whitespace();

        if (current_group == "Desktop Entry"sv) {
            if (key == "Type"sv)
                entry.type = val.to_byte_string();
            else if (key == "Name"sv)
                entry.name = val.to_byte_string();
            else if (key == "GenericName"sv)
                entry.generic_name = val.to_byte_string();
            else if (key == "Comment"sv)
                entry.comment = val.to_byte_string();
            else if (key == "Icon"sv)
                entry.icon = val.to_byte_string();
            else if (key == "Exec"sv)
                entry.exec = val.to_byte_string();
            else if (key == "TryExec"sv)
                entry.try_exec = val.to_byte_string();
            else if (key == "Path"sv)
                entry.path = val.to_byte_string();
            else if (key == "Categories"sv)
                entry.categories = split_semicolon_list(val);
            else if (key == "MimeType"sv)
                entry.mime_types = split_semicolon_list(val);
            else if (key == "OnlyShowIn"sv)
                entry.only_show_in = split_semicolon_list(val);
            else if (key == "NotShowIn"sv)
                entry.not_show_in = split_semicolon_list(val);
            else if (key == "Actions"sv)
                action_order = split_semicolon_list(val);
            else if (key == "Hidden"sv)
                entry.hidden = val.equals_ignoring_ascii_case("true"sv);
            else if (key == "NoDisplay"sv)
                entry.no_display = val.equals_ignoring_ascii_case("true"sv);
            else if (key == "Terminal"sv)
                entry.terminal = val.equals_ignoring_ascii_case("true"sv);
            else if (key == "StartupNotify"sv)
                entry.startup_notify = val.equals_ignoring_ascii_case("true"sv);
            else if (key == "DBusActivatable"sv)
                entry.dbus_activatable = val.equals_ignoring_ascii_case("true"sv);
        } else if (current_group.starts_with("Desktop Action "sv)) {
            auto action_id = current_group.substring(15);
            auto& action = action_map.ensure(action_id, [&] { return DesktopAction { action_id, {}, {}, {} }; });
            if (key == "Name"sv)
                action.name = val.to_byte_string();
            else if (key == "Icon"sv)
                action.icon = val.to_byte_string();
            else if (key == "Exec"sv)
                action.exec = val.to_byte_string();
        }
    }

    if (!saw_desktop_entry)
        return {};
    if (entry.name.is_empty())
        entry.name = id;

    for (auto const& action_id : action_order) {
        if (auto it = action_map.find(action_id); it != action_map.end())
            entry.actions.append(it->value);
    }

    return entry;
}

static Vector<ByteString> application_dirs()
{
    Vector<ByteString> dirs;
    dirs.append(ByteString::formatted("{}/applications", data_home()));
    for (auto const& d : data_dirs())
        dirs.append(ByteString::formatted("{}/applications", d));
    return dirs;
}

Optional<DesktopEntry> find_desktop_entry(StringView desktop_id)
{
    if (desktop_id.is_empty())
        return {};
    for (auto const& dir : application_dirs()) {
        auto direct = ByteString::formatted("{}/{}", dir, desktop_id);
        if (::access(direct.characters(), R_OK) == 0) {
            if (auto parsed = parse_desktop_entry(desktop_id.to_byte_string(), direct); parsed.has_value())
                return parsed;
        }
        // Desktop Entry spec: '-' in desktop-id may correspond to subdirectories.
        if (desktop_id.contains('-')) {
            auto slashed = desktop_id.to_byte_string().replace("-"sv, "/"sv, ReplaceMode::All);
            auto nested = ByteString::formatted("{}/{}", dir, slashed);
            if (::access(nested.characters(), R_OK) == 0) {
                if (auto parsed = parse_desktop_entry(desktop_id.to_byte_string(), nested); parsed.has_value())
                    return parsed;
            }
        }
    }
    return {};
}

static void scan_desktop_dir(ByteString const& root, ByteString const& prefix, HashTable<ByteString>& seen, Vector<DesktopEntry>& out)
{
    DIR* dir = ::opendir(root.characters());
    if (!dir)
        return;

    Vector<ByteString> names;
    while (auto* ent = ::readdir(dir)) {
        StringView name { ent->d_name, __builtin_strlen(ent->d_name) };
        if (name == "."sv || name == ".."sv)
            continue;
        names.append(name.to_byte_string());
    }
    ::closedir(dir);
    quick_sort(names);

    for (auto const& name : names) {
        auto full = ByteString::formatted("{}/{}", root, name);
        struct stat st {};
        if (::stat(full.characters(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            auto next_prefix = prefix.is_empty() ? name : ByteString::formatted("{}-{}", prefix, name);
            scan_desktop_dir(full, next_prefix, seen, out);
            continue;
        }
        if (!name.ends_with(".desktop"sv))
            continue;
        auto id = prefix.is_empty() ? name : ByteString::formatted("{}-{}", prefix, name);
        if (seen.contains(id))
            continue;
        seen.set(id);
        if (auto parsed = parse_desktop_entry(id, full); parsed.has_value())
            out.append(parsed.release_value());
    }
}

Vector<DesktopEntry> all_desktop_entries()
{
    HashTable<ByteString> seen;
    Vector<DesktopEntry> entries;
    for (auto const& dir : application_dirs())
        scan_desktop_dir(dir, {}, seen, entries);
    return entries;
}

}
