/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibXDG/BaseDirs.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

static void check(bool condition, char const* label)
{
    if (condition) {
        outln("  ok: {}", label);
    } else {
        outln("  FAIL: {}", label);
        ++g_failures;
    }
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    outln("=== XDG Base Directory tests ===");

    char tmpl[] = "/tmp/sde-xdg-basedirs.XXXXXX";
    auto const* tmpdir = getenv("TMPDIR");
    ByteString root_pattern = tmpdir && *tmpdir
        ? ByteString::formatted("{}/sde-xdg-basedirs.XXXXXX", tmpdir)
        : ByteString(tmpl);
    Vector<char> buf;
    buf.resize(root_pattern.length() + 1);
    memcpy(buf.data(), root_pattern.characters(), root_pattern.length() + 1);
    char* root_c = mkdtemp(buf.data());
    if (!root_c)
        return Error::from_errno(errno);
    ByteString root { root_c };

    setenv("HOME", root.characters(), 1);
    unsetenv("SDE_STATE_DIR");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_STATE_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_CONFIG_DIRS");
    unsetenv("XDG_DATA_DIRS");
    unsetenv("XDG_CURRENT_DESKTOP");

    // Defaults when unset.
    check(XDG::config_home() == ByteString::formatted("{}/.config", root), "default XDG_CONFIG_HOME");
    check(XDG::data_home() == ByteString::formatted("{}/.local/share", root), "default XDG_DATA_HOME");
    check(XDG::state_home() == ByteString::formatted("{}/.local/state", root), "default XDG_STATE_HOME");
    check(XDG::cache_home() == ByteString::formatted("{}/.cache", root), "default XDG_CACHE_HOME");
    check(XDG::sde_config_dir() == ByteString::formatted("{}/.config/sde", root), "default sde_config_dir");
    check(XDG::sde_state_dir() == ByteString::formatted("{}/.local/state/sde", root), "default sde_state_dir");
    check(XDG::sde_cache_dir() == ByteString::formatted("{}/.cache/sde", root), "default sde_cache_dir");
    check(XDG::config_dirs().size() == 1 && XDG::config_dirs()[0] == "/etc/xdg"sv, "default XDG_CONFIG_DIRS");
    check(XDG::data_dirs().size() == 2 && XDG::data_dirs()[0] == "/usr/local/share"sv && XDG::data_dirs()[1] == "/usr/share"sv, "default XDG_DATA_DIRS");
    check(XDG::current_desktops().size() == 1 && XDG::current_desktops()[0] == "SDE"sv, "default XDG_CURRENT_DESKTOP");

    // Relative paths must be ignored per spec.
    setenv("XDG_CONFIG_HOME", "relative/config", 1);
    setenv("XDG_STATE_HOME", "relative/state", 1);
    setenv("XDG_CONFIG_DIRS", "rel1:/opt/xdg:rel2:/etc/xdg2", 1);
    check(XDG::config_home() == ByteString::formatted("{}/.config", root), "relative XDG_CONFIG_HOME ignored");
    check(XDG::state_home() == ByteString::formatted("{}/.local/state", root), "relative XDG_STATE_HOME ignored");
    auto cdirs = XDG::config_dirs();
    check(cdirs.size() == 2 && cdirs[0] == "/opt/xdg"sv && cdirs[1] == "/etc/xdg2"sv, "relative entries filtered from XDG_CONFIG_DIRS");

    // Absolute overrides and config file priority order.
    auto user_cfg = ByteString::formatted("{}/cfg-home", root);
    auto sys_cfg_1 = ByteString::formatted("{}/sys1", root);
    auto sys_cfg_2 = ByteString::formatted("{}/sys2", root);
    TRY(Core::Directory::create(ByteString::formatted("{}/sde", user_cfg), Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(ByteString::formatted("{}/sde", sys_cfg_2), Core::Directory::CreateDirectories::Yes));
    TRY(Core::File::open(ByteString::formatted("{}/sde/WindowServer.ini", user_cfg), Core::File::OpenMode::Write));
    TRY(Core::File::open(ByteString::formatted("{}/sde/WindowServer.ini", sys_cfg_2), Core::File::OpenMode::Write));

    setenv("XDG_CONFIG_HOME", user_cfg.characters(), 1);
    auto cdir_val = ByteString::formatted("{}:{}", sys_cfg_1, sys_cfg_2);
    setenv("XDG_CONFIG_DIRS", cdir_val.characters(), 1);
    auto found = XDG::find_config_files("sde/WindowServer.ini"sv);
    check(found.size() == 2
            && found[0] == ByteString::formatted("{}/sde/WindowServer.ini", user_cfg)
            && found[1] == ByteString::formatted("{}/sde/WindowServer.ini", sys_cfg_2),
        "find_config_files returns user then system in priority order");

    // SDE_STATE_DIR override takes precedence.
    auto custom_state = ByteString::formatted("{}/override", root);
    setenv("SDE_STATE_DIR", custom_state.characters(), 1);
    check(XDG::sde_config_dir() == custom_state && XDG::sde_state_dir() == custom_state, "SDE_STATE_DIR overrides both config and state dir");

    // Colon-separated XDG_CURRENT_DESKTOP.
    setenv("XDG_CURRENT_DESKTOP", "SDE:Serenity", 1);
    auto desktops = XDG::current_desktops();
    check(desktops.size() == 2 && desktops[0] == "SDE"sv && desktops[1] == "Serenity"sv, "colon-separated XDG_CURRENT_DESKTOP");

    // Cleanup temp files.
    ::unlink(ByteString::formatted("{}/sde/WindowServer.ini", user_cfg).characters());
    ::unlink(ByteString::formatted("{}/sde/WindowServer.ini", sys_cfg_2).characters());
    ::rmdir(ByteString::formatted("{}/sde", user_cfg).characters());
    ::rmdir(ByteString::formatted("{}/sde", sys_cfg_2).characters());
    ::rmdir(user_cfg.characters());
    ::rmdir(sys_cfg_2.characters());
    ::rmdir(root.characters());

    if (g_failures > 0) {
        outln("==> XDG Base Directory tests FAILED ({} failure(s))", g_failures);
        return 1;
    }
    outln("==> XDG Base Directory tests PASSED");
    return 0;
}
