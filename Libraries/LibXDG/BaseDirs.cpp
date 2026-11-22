/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXDG/BaseDirs.h>

#include <stdlib.h>
#include <unistd.h>

namespace XDG {

static bool is_absolute_path(StringView path)
{
    return !path.is_empty() && path.starts_with('/');
}

static ByteString env_absolute_dir(char const* name, ByteString fallback)
{
    if (auto const* raw = getenv(name); raw && *raw) {
        StringView view { raw, __builtin_strlen(raw) };
        if (is_absolute_path(view))
            return view.to_byte_string();
    }
    return fallback;
}

static Vector<ByteString> parse_dir_list(char const* env_name, StringView fallback)
{
    Vector<ByteString> dirs;
    StringView raw_view;
    if (auto const* raw = getenv(env_name); raw && *raw)
        raw_view = { raw, __builtin_strlen(raw) };

    auto append_from = [&](StringView list) {
        for (auto part : list.split_view(':')) {
            if (is_absolute_path(part))
                dirs.append(part.to_byte_string());
        }
    };

    if (!raw_view.is_empty())
        append_from(raw_view);
    if (dirs.is_empty())
        append_from(fallback);
    return dirs;
}

ByteString home_directory()
{
    if (auto const* home = getenv("HOME"); home && *home)
        return ByteString(home);
    return ByteString { "/tmp" };
}

ByteString config_home()
{
    return env_absolute_dir("XDG_CONFIG_HOME", ByteString::formatted("{}/.config", home_directory()));
}

ByteString data_home()
{
    return env_absolute_dir("XDG_DATA_HOME", ByteString::formatted("{}/.local/share", home_directory()));
}

ByteString state_home()
{
    return env_absolute_dir("XDG_STATE_HOME", ByteString::formatted("{}/.local/state", home_directory()));
}

ByteString cache_home()
{
    return env_absolute_dir("XDG_CACHE_HOME", ByteString::formatted("{}/.cache", home_directory()));
}

ByteString runtime_dir()
{
    return env_absolute_dir("XDG_RUNTIME_DIR", ByteString::formatted("/run/user/{}", getuid()));
}

Vector<ByteString> config_dirs()
{
    return parse_dir_list("XDG_CONFIG_DIRS", "/etc/xdg"sv);
}

Vector<ByteString> data_dirs()
{
    return parse_dir_list("XDG_DATA_DIRS", "/usr/local/share:/usr/share"sv);
}

ByteString sde_config_dir()
{
    if (auto const* override_dir = getenv("SDE_STATE_DIR"); override_dir && *override_dir)
        return ByteString(override_dir);
    return ByteString::formatted("{}/sde", config_home());
}

ByteString sde_state_dir()
{
    if (auto const* override_dir = getenv("SDE_STATE_DIR"); override_dir && *override_dir)
        return ByteString(override_dir);
    return ByteString::formatted("{}/sde", state_home());
}

ByteString sde_cache_dir()
{
    return ByteString::formatted("{}/sde", cache_home());
}

static Vector<ByteString> find_existing(ByteString const& home_dir, Vector<ByteString> const& system_dirs, StringView relative_path)
{
    Vector<ByteString> matches;
    auto check = [&](ByteString const& base) {
        auto candidate = ByteString::formatted("{}/{}", base, relative_path);
        if (::access(candidate.characters(), F_OK) == 0)
            matches.append(move(candidate));
    };
    check(home_dir);
    for (auto const& dir : system_dirs)
        check(dir);
    return matches;
}

Vector<ByteString> find_config_files(StringView relative_path)
{
    return find_existing(config_home(), config_dirs(), relative_path);
}

Vector<ByteString> find_data_files(StringView relative_path)
{
    return find_existing(data_home(), data_dirs(), relative_path);
}

Vector<ByteString> current_desktops()
{
    Vector<ByteString> result;
    if (auto const* raw = getenv("XDG_CURRENT_DESKTOP"); raw && *raw) {
        StringView view { raw, __builtin_strlen(raw) };
        for (auto part : view.split_view(':')) {
            auto trimmed = part.trim_whitespace();
            if (!trimmed.is_empty())
                result.append(trimmed.to_byte_string());
        }
    }
    if (result.is_empty())
        result.append("SDE");
    return result;
}

}
