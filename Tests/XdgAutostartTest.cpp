/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibXDG/Autostart.h>

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

static ErrorOr<void> write_text(ByteString const& path, StringView contents)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(contents.bytes()));
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    outln("=== XDG Autostart tests ===");

    // 1. Exec= parsing, quoting and field code stripping.
    {
        auto argv = XDG::parse_exec_command(R"(/usr/bin/app --title "Hello World" %U --pct 100%% %f)"sv);
        check(argv.size() == 5
                && argv[0] == "/usr/bin/app"sv
                && argv[1] == "--title"sv
                && argv[2] == "Hello World"sv
                && argv[3] == "--pct"sv
                && argv[4] == "100%"sv,
            "parse_exec_command handles quotes, field codes and %%");
    }

    // 2. Full autostart discovery with system/user precedence and filtering.
    auto const* tmpdir = getenv("TMPDIR");
    ByteString root_pattern = tmpdir && *tmpdir
        ? ByteString::formatted("{}/sde-xdg-autostart.XXXXXX", tmpdir)
        : ByteString { "/tmp/sde-xdg-autostart.XXXXXX" };
    Vector<char> buf;
    buf.resize(root_pattern.length() + 1);
    memcpy(buf.data(), root_pattern.characters(), root_pattern.length() + 1);
    char* root_c = mkdtemp(buf.data());
    if (!root_c)
        return Error::from_errno(errno);
    ByteString root { root_c };

    auto user_cfg = ByteString::formatted("{}/user-cfg", root);
    auto sys_cfg = ByteString::formatted("{}/sys-cfg", root);
    auto user_auto = ByteString::formatted("{}/autostart", user_cfg);
    auto sys_auto = ByteString::formatted("{}/autostart", sys_cfg);
    TRY(Core::Directory::create(user_auto, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(sys_auto, Core::Directory::CreateDirectories::Yes));

    // System entry that should run.
    TRY(write_text(ByteString::formatted("{}/a-sys.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nName=SysApp\nExec=/bin/true --sys %u\nPath=/tmp\n"sv));

    // System entry that user hides via Hidden=true with same basename.
    TRY(write_text(ByteString::formatted("{}/b-hidden.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nName=ShouldHide\nExec=/bin/true\n"sv));
    TRY(write_text(ByteString::formatted("{}/b-hidden.desktop", user_auto),
        "[Desktop Entry]\nType=Application\nHidden=true\n"sv));

    // System entry that user overrides with different Exec.
    TRY(write_text(ByteString::formatted("{}/c-override.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/false\n"sv));
    TRY(write_text(ByteString::formatted("{}/c-override.desktop", user_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/true --user-won\nTerminal=true\n"sv));

    // OnlyShowIn=GNOME should be skipped on SDE.
    TRY(write_text(ByteString::formatted("{}/d-gnome.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/true\nOnlyShowIn=GNOME;\n"sv));

    // NotShowIn=SDE should be skipped on SDE.
    TRY(write_text(ByteString::formatted("{}/e-notsde.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/true\nNotShowIn=KDE;SDE;\n"sv));

    // TryExec pointing to non-existent binary should be skipped.
    TRY(write_text(ByteString::formatted("{}/f-missing.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/true\nTryExec=/nonexistent/sde-no-such-bin\n"sv));

    // X-GNOME-Autostart-enabled=false should be skipped.
    TRY(write_text(ByteString::formatted("{}/g-disabled.desktop", user_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/true\nX-GNOME-Autostart-enabled=false\n"sv));

    // OnlyShowIn=SDE with existing TryExec=/bin/sh should run.
    TRY(write_text(ByteString::formatted("{}/h-sde-only.desktop", sys_auto),
        "[Desktop Entry]\nType=Application\nExec=/bin/true --sde\nOnlyShowIn=SDE;\nTryExec=/bin/sh\n"sv));

    setenv("XDG_CONFIG_HOME", user_cfg.characters(), 1);
    setenv("XDG_CONFIG_DIRS", sys_cfg.characters(), 1);
    setenv("XDG_CURRENT_DESKTOP", "SDE", 1);

    auto entries = XDG::discover_autostart_entries();
    check(entries.size() == 3, "discovered exactly 3 enabled entries");

    auto find_entry = [&](StringView id) -> XDG::AutostartEntry const* {
        for (auto const& e : entries) {
            if (e.id == id)
                return &e;
        }
        return nullptr;
    };

    auto const* sys_entry = find_entry("a-sys.desktop"sv);
    check(sys_entry && sys_entry->argv.size() == 2 && sys_entry->argv[1] == "--sys"sv
            && sys_entry->working_directory == "/tmp"sv,
        "system entry resolved with Path= and stripped %u");

    auto const* ov_entry = find_entry("c-override.desktop"sv);
    check(ov_entry && ov_entry->argv.size() == 2 && ov_entry->argv[1] == "--user-won"sv && ov_entry->terminal,
        "user entry overrode system entry and preserved Terminal=true");

    auto const* sde_entry = find_entry("h-sde-only.desktop"sv);
    check(sde_entry && sde_entry->argv.size() == 2 && sde_entry->argv[1] == "--sde"sv,
        "OnlyShowIn=SDE with valid TryExec included");

    check(!find_entry("b-hidden.desktop"sv), "user Hidden=true suppressed system entry");
    check(!find_entry("d-gnome.desktop"sv), "OnlyShowIn=GNOME excluded");
    check(!find_entry("e-notsde.desktop"sv), "NotShowIn=SDE excluded");
    check(!find_entry("f-missing.desktop"sv), "missing TryExec excluded");
    check(!find_entry("g-disabled.desktop"sv), "X-GNOME-Autostart-enabled=false excluded");

    for (auto name : { "a-sys.desktop"sv, "b-hidden.desktop"sv, "c-override.desktop"sv, "d-gnome.desktop"sv, "e-notsde.desktop"sv, "f-missing.desktop"sv, "h-sde-only.desktop"sv })
        ::unlink(ByteString::formatted("{}/{}", sys_auto, name).characters());
    for (auto name : { "b-hidden.desktop"sv, "c-override.desktop"sv, "g-disabled.desktop"sv })
        ::unlink(ByteString::formatted("{}/{}", user_auto, name).characters());
    ::rmdir(user_auto.characters());
    ::rmdir(sys_auto.characters());
    ::rmdir(user_cfg.characters());
    ::rmdir(sys_cfg.characters());
    ::rmdir(root.characters());

    if (g_failures > 0) {
        outln("==> XDG Autostart tests FAILED ({} failure(s))", g_failures);
        return 1;
    }
    outln("==> XDG Autostart tests PASSED");
    return 0;
}
