/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/Autostart.h>
#include <LibXDG/BaseDirs.h>

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace XDG {

static bool is_executable_file(ByteString const& path)
{
    struct stat st {};
    if (::stat(path.characters(), &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode))
        return false;
    return ::access(path.characters(), X_OK) == 0;
}

bool try_exec_available(StringView try_exec)
{
    auto trimmed = try_exec.trim_whitespace();
    if (trimmed.is_empty())
        return true;

    if (trimmed.contains('/'))
        return is_executable_file(trimmed.to_byte_string());

    auto const* path_env = getenv("PATH");
    if (!path_env || !*path_env)
        return false;

    StringView path_view { path_env, __builtin_strlen(path_env) };
    for (auto dir : path_view.split_view(':')) {
        if (dir.is_empty())
            continue;
        auto candidate = ByteString::formatted("{}/{}", dir, trimmed);
        if (is_executable_file(candidate))
            return true;
    }
    return false;
}

Vector<ByteString> parse_exec_command(StringView exec_line)
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

    // Strip Desktop Entry field codes (%f, %F, %u, %U, %d, %D, %n, %N, %i, %c,
    // %k, %v, %m) for autostart launches; unescape %% -> %.
    Vector<ByteString> argv;
    for (auto const& arg : raw_args) {
        if (arg.length() == 2 && arg[0] == '%' && arg[1] != '%') {
            char code = arg[1];
            if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z'))
                continue;
        }
        StringBuilder cleaned;
        for (size_t i = 0; i < arg.length(); ++i) {
            if (arg[i] == '%' && i + 1 < arg.length()) {
                char next = arg[i + 1];
                if (next == '%') {
                    cleaned.append('%');
                    ++i;
                    continue;
                }
                if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
                    ++i;
                    continue;
                }
            }
            cleaned.append(arg[i]);
        }
        auto token = cleaned.to_byte_string();
        if (!token.is_empty())
            argv.append(move(token));
    }
    return argv;
}

static bool desktop_list_matches(StringView semicolon_list, Vector<ByteString> const& desktops)
{
    for (auto item : semicolon_list.split_view(';')) {
        auto trimmed = item.trim_whitespace();
        if (trimmed.is_empty())
            continue;
        for (auto const& desktop : desktops) {
            if (desktop == trimmed)
                return true;
        }
    }
    return false;
}

static Optional<AutostartEntry> parse_autostart_file(ByteString const& id, ByteString const& path, Vector<ByteString> const& desktops)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};

    StringView text { contents.value() };
    bool in_desktop_entry = false;
    bool saw_desktop_entry = false;

    ByteString type;
    ByteString name;
    ByteString exec_line;
    ByteString try_exec;
    ByteString working_dir;
    ByteString only_show_in;
    ByteString not_show_in;
    bool hidden = false;
    bool gnome_enabled = true;
    bool terminal = false;

    for (auto raw_line : text.lines()) {
        auto line = raw_line.trim_whitespace();
        if (line.is_empty() || line.starts_with('#'))
            continue;
        if (line.starts_with('[') && line.ends_with(']')) {
            if (line == "[Desktop Entry]"sv) {
                in_desktop_entry = true;
                saw_desktop_entry = true;
            } else {
                in_desktop_entry = false;
            }
            continue;
        }
        if (!in_desktop_entry)
            continue;

        auto eq = line.find('=');
        if (!eq.has_value())
            continue;
        auto key = line.substring_view(0, eq.value()).trim_whitespace();
        auto val = line.substring_view(eq.value() + 1).trim_whitespace();

        if (key == "Type"sv)
            type = val.to_byte_string();
        else if (key == "Name"sv)
            name = val.to_byte_string();
        else if (key == "Exec"sv)
            exec_line = val.to_byte_string();
        else if (key == "TryExec"sv)
            try_exec = val.to_byte_string();
        else if (key == "Path"sv)
            working_dir = val.to_byte_string();
        else if (key == "OnlyShowIn"sv)
            only_show_in = val.to_byte_string();
        else if (key == "NotShowIn"sv)
            not_show_in = val.to_byte_string();
        else if (key == "Hidden"sv)
            hidden = val.equals_ignoring_ascii_case("true"sv);
        else if (key == "X-GNOME-Autostart-enabled"sv)
            gnome_enabled = !val.equals_ignoring_ascii_case("false"sv);
        else if (key == "Terminal"sv)
            terminal = val.equals_ignoring_ascii_case("true"sv);
    }

    if (!saw_desktop_entry)
        return {};
    if (hidden || !gnome_enabled)
        return {};
    if (!type.is_empty() && type != "Application"sv)
        return {};
    if (!only_show_in.is_empty() && !desktop_list_matches(only_show_in, desktops))
        return {};
    if (!not_show_in.is_empty() && desktop_list_matches(not_show_in, desktops))
        return {};
    if (!try_exec.is_empty() && !try_exec_available(try_exec))
        return {};

    auto argv = parse_exec_command(exec_line);
    if (argv.is_empty())
        return {};

    AutostartEntry entry;
    entry.id = id;
    entry.name = name.is_empty() ? id : move(name);
    entry.desktop_file_path = path;
    entry.argv = move(argv);
    if (!working_dir.is_empty())
        entry.working_directory = move(working_dir);
    entry.terminal = terminal;
    return entry;
}

static Vector<ByteString> list_desktop_files(ByteString const& dir_path)
{
    Vector<ByteString> files;
    DIR* dir = ::opendir(dir_path.characters());
    if (!dir)
        return files;
    while (auto* ent = ::readdir(dir)) {
        StringView name { ent->d_name, __builtin_strlen(ent->d_name) };
        if (name.ends_with(".desktop"sv))
            files.append(name.to_byte_string());
    }
    ::closedir(dir);
    quick_sort(files);
    return files;
}

Vector<AutostartEntry> discover_autostart_entries(
    Optional<ByteString> user_autostart_dir_override,
    Optional<Vector<ByteString>> desktops_override)
{
    Vector<ByteString> search_dirs;
    if (user_autostart_dir_override.has_value() && !user_autostart_dir_override->is_empty())
        search_dirs.append(user_autostart_dir_override.value());
    else
        search_dirs.append(ByteString::formatted("{}/autostart", config_home()));

    for (auto const& sys_dir : config_dirs())
        search_dirs.append(ByteString::formatted("{}/autostart", sys_dir));

    auto desktops = desktops_override.has_value() ? desktops_override.value() : current_desktops();

    HashTable<ByteString> seen_ids;
    Vector<AutostartEntry> entries;

    for (auto const& dir : search_dirs) {
        for (auto const& filename : list_desktop_files(dir)) {
            // Higher-priority directory owns this basename even if Hidden=true.
            if (seen_ids.contains(filename))
                continue;
            seen_ids.set(filename);

            auto full_path = ByteString::formatted("{}/{}", dir, filename);
            if (auto entry = parse_autostart_file(filename, full_path, desktops); entry.has_value())
                entries.append(entry.release_value());
        }
    }

    return entries;
}

}
