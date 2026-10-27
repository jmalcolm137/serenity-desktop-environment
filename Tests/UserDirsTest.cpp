/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Checks that Core::StandardPaths honours the XDG User Directories spec
// ($XDG_CONFIG_HOME/user-dirs.dirs) before the built-in $HOME/... fallbacks,
// while keeping the historical environment override.

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibMain/Main.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int s_failures = 0;

void expect(ByteString const& actual, ByteString const& expected, StringView what)
{
    if (actual == expected) {
        outln("  ok: {} = {}", what, actual);
        return;
    }
    outln("  FAIL: {} = {} (expected {})", what, actual, expected);
    s_failures++;
}

bool write_file(ByteString const& path, StringView contents)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Write);
    if (file.is_error())
        return false;
    return !file.value()->write_until_depleted(contents.bytes()).is_error();
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    char root_template[] = "/tmp/sde-user-dirs-test.XXXXXX";
    auto* root_c = mkdtemp(root_template);
    if (!root_c) {
        outln("FAIL: mkdtemp");
        return 1;
    }

    ByteString root { root_c };
    auto home = ByteString::formatted("{}/home", root);
    auto config = ByteString::formatted("{}/config", root);
    if (mkdir(home.characters(), 0700) != 0 || mkdir(config.characters(), 0700) != 0) {
        outln("FAIL: mkdir");
        return 1;
    }

    setenv("HOME", home.characters(), 1);
    setenv("XDG_CONFIG_HOME", config.characters(), 1);
    for (auto* name : {
             "XDG_DESKTOP_DIR", "XDG_DOWNLOAD_DIR", "XDG_TEMPLATES_DIR",
             "XDG_PUBLICSHARE_DIR", "XDG_DOCUMENTS_DIR", "XDG_MUSIC_DIR",
             "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR" })
        unsetenv(name);

    auto user_dirs = ByteString::formatted("{}/user-dirs.dirs", config);
    if (!write_file(user_dirs,
            "XDG_DESKTOP_DIR=\"$HOME/MyDesktop\"\n"
            "XDG_DOWNLOAD_DIR=\"${HOME}/Inbox\"\n"
            "XDG_MUSIC_DIR=\"$HOME\"\n"sv)) {
        outln("FAIL: could not write {}", user_dirs);
        return 1;
    }

    expect(Core::StandardPaths::desktop_directory(), ByteString::formatted("{}/MyDesktop", home), "desktop"sv);
    expect(Core::StandardPaths::downloads_directory(), ByteString::formatted("{}/Inbox", home), "downloads"sv);
    expect(Core::StandardPaths::music_directory(), home, "music disabled"sv);
    expect(Core::StandardPaths::pictures_directory(), ByteString::formatted("{}/Pictures", home), "pictures fallback"sv);
    expect(Core::StandardPaths::templates_directory(), ByteString::formatted("{}/Templates", home), "templates fallback"sv);
    expect(Core::StandardPaths::public_share_directory(), ByteString::formatted("{}/Public", home), "public fallback"sv);

    // The environment override stays the highest priority (scripts/tests).
    setenv("XDG_PICTURES_DIR", "/tmp/sde-user-dirs-env", 1);
    expect(Core::StandardPaths::pictures_directory(), "/tmp/sde-user-dirs-env", "env override"sv);

    unlink(user_dirs.characters());
    rmdir(config.characters());
    rmdir(home.characters());
    rmdir(root.characters());

    if (s_failures != 0) {
        outln("{} failure(s)", s_failures);
        return 1;
    }
    outln("user dirs test passed");
    return 0;
}
