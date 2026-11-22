/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibXDG/RecentlyUsed.h>
#include <LibXDG/SoundTheme.h>

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
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(contents.bytes()));
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    outln("=== XDG Phase F5 (RecentlyUsed & SoundTheme) tests ===");

    auto const* tmpdir = getenv("TMPDIR");
    ByteString root_pattern = tmpdir && *tmpdir
        ? ByteString::formatted("{}/sde-xdg-f5.XXXXXX", tmpdir)
        : ByteString { "/tmp/sde-xdg-f5.XXXXXX" };
    Vector<char> buf;
    buf.resize(root_pattern.length() + 1);
    memcpy(buf.data(), root_pattern.characters(), root_pattern.length() + 1);
    char* root_c = mkdtemp(buf.data());
    if (!root_c)
        return Error::from_errno(errno);
    ByteString root { root_c };

    auto data_home = ByteString::formatted("{}/data-home", root);
    auto data_sys = ByteString::formatted("{}/data-sys", root);
    TRY(Core::Directory::create(data_home, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(data_sys, Core::Directory::CreateDirectories::Yes));

    setenv("XDG_DATA_HOME", data_home.characters(), 1);
    setenv("XDG_DATA_DIRS", data_sys.characters(), 1);

    // 1. RecentlyUsed (recently-used.xbel): record, XML-escape, dedup, order.
    TRY(XDG::record_recently_used("/tmp/a & b.txt"sv, "text/plain"sv, "TextEditor"sv, "TextEditor %f"sv));
    TRY(XDG::record_recently_used("https://serenityos.org"sv, "x-scheme-handler/https"sv, "Browser"sv, "Browser %u"sv));
    // Re-recording first URI moves it to the top and preserves deduplication.
    TRY(XDG::record_recently_used("/tmp/a & b.txt"sv, "text/plain"sv, "TextEditor"sv, "TextEditor %f"sv));

    auto recents = XDG::read_recently_used();
    check(recents.size() == 2, "recently-used.xbel deduplicates repeated URIs");
    check(recents[0].uri == "file:///tmp/a & b.txt"sv && recents[0].mime_type == "text/plain"sv && recents[0].app_name == "TextEditor"sv,
        "XML escaping and newest-first ordering work");
    check(recents[1].uri == "https://serenityos.org"sv, "second entry preserved");

    // 2. SoundTheme: index.theme, Inherits=, dash truncation fallback, freedesktop fallback.
    auto custom_stereo = ByteString::formatted("{}/sounds/CustomSounds/stereo", data_home);
    auto fd_stereo = ByteString::formatted("{}/sounds/freedesktop/stereo", data_sys);
    TRY(Core::Directory::create(custom_stereo, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(fd_stereo, Core::Directory::CreateDirectories::Yes));

    TRY(write_text(ByteString::formatted("{}/sounds/CustomSounds/index.theme", data_home),
        "[Sound Theme]\nName=CustomSounds\nInherits=freedesktop\nDirectories=stereo\n"sv));
    TRY(write_text(ByteString::formatted("{}/sounds/freedesktop/index.theme", data_sys),
        "[Sound Theme]\nName=freedesktop\nDirectories=stereo\n"sv));

    auto custom_bell = ByteString::formatted("{}/bell.oga", custom_stereo);
    auto fd_msg = ByteString::formatted("{}/message.wav", fd_stereo);
    TRY(write_text(custom_bell, "ogg"sv));
    TRY(write_text(fd_msg, "wav"sv));

    check(XDG::find_sound("bell"sv, "CustomSounds"sv) == custom_bell, "exact sound match in CustomSounds");
    check(XDG::find_sound("message-new-instant"sv, "CustomSounds"sv) == fd_msg,
        "dash truncation + Inherits=freedesktop resolves message-new-instant -> message.wav");
    check(!XDG::find_sound("no-such-event"sv, "CustomSounds"sv).has_value(), "missing sound returns empty");

    // Cleanup.
    for (auto const& p : {
             XDG::recently_used_path(),
             custom_bell,
             fd_msg,
             ByteString::formatted("{}/sounds/CustomSounds/index.theme", data_home),
             ByteString::formatted("{}/sounds/freedesktop/index.theme", data_sys),
         })
        ::unlink(p.characters());
    for (auto const& d : {
             custom_stereo,
             ByteString::formatted("{}/sounds/CustomSounds", data_home),
             ByteString::formatted("{}/sounds", data_home),
             fd_stereo,
             ByteString::formatted("{}/sounds/freedesktop", data_sys),
             ByteString::formatted("{}/sounds", data_sys),
             data_home,
             data_sys,
             root,
         })
        ::rmdir(d.characters());

    if (g_failures > 0) {
        outln("==> XDG F5 tests FAILED ({} failure(s))", g_failures);
        return 1;
    }
    outln("==> XDG F5 tests PASSED");
    return 0;
}
