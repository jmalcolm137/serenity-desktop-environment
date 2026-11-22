/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibXDG/Trash.h>

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
    outln("=== XDG Trash tests ===");

    auto const* tmpdir = getenv("TMPDIR");
    ByteString root_pattern = tmpdir && *tmpdir
        ? ByteString::formatted("{}/sde-xdg-trash.XXXXXX", tmpdir)
        : ByteString { "/tmp/sde-xdg-trash.XXXXXX" };
    Vector<char> buf;
    buf.resize(root_pattern.length() + 1);
    memcpy(buf.data(), root_pattern.characters(), root_pattern.length() + 1);
    char* root_c = mkdtemp(buf.data());
    if (!root_c)
        return Error::from_errno(errno);
    ByteString root { root_c };

    auto data_home = ByteString::formatted("{}/data", root);
    auto work_dir = ByteString::formatted("{}/work", root);
    TRY(Core::Directory::create(data_home, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(work_dir, Core::Directory::CreateDirectories::Yes));

    setenv("XDG_DATA_HOME", data_home.characters(), 1);

    // 1. Trash a file with spaces in its name (exercises percent-encoding).
    auto file1 = ByteString::formatted("{}/hello world.txt", work_dir);
    TRY(write_text(file1, "payload-1"sv));
    auto id1 = TRY(XDG::trash_file(file1));
    check(id1 == "hello world.txt"sv, "first trashed file keeps basename id");
    check(::access(file1.characters(), F_OK) != 0, "original file moved out of work dir");

    // 2. Collision numbering when trashing a second file with the same name.
    TRY(write_text(file1, "payload-2"sv));
    auto id2 = TRY(XDG::trash_file(file1));
    check(id2 == "hello world.txt.2"sv, "duplicate filename gets .2 suffix");

    // 3. List trash entries and verify decoded Path= and ISO-8601 DeletionDate=.
    auto entries = XDG::list_trash();
    check(entries.size() == 2, "list_trash returns both entries");
    bool saw_orig = false;
    for (auto const& e : entries) {
        if (e.id == id1 && e.original_path == file1 && e.deletion_date.contains('T'))
            saw_orig = true;
    }
    check(saw_orig, "trashinfo preserves percent-decoded original path and ISO-8601 date");

    // 4. Restore first file and verify contents.
    TRY(XDG::restore_trashed_file(id1));
    check(::access(file1.characters(), F_OK) == 0, "restored file exists at original path");
    {
        auto f = TRY(Core::File::open(file1, Core::File::OpenMode::Read));
        auto bytes = TRY(f->read_until_eof());
        check(StringView { bytes } == "payload-1"sv, "restored file contents intact");
    }
    check(XDG::list_trash().size() == 1, "restored entry removed from trash");

    // 5. Empty trash removes remaining entries.
    TRY(XDG::empty_trash());
    check(XDG::list_trash().is_empty(), "empty_trash clears all entries");

    // Cleanup.
    ::unlink(file1.characters());
    ::rmdir(ByteString::formatted("{}/Trash/files", data_home).characters());
    ::rmdir(ByteString::formatted("{}/Trash/info", data_home).characters());
    ::rmdir(ByteString::formatted("{}/Trash", data_home).characters());
    ::rmdir(data_home.characters());
    ::rmdir(work_dir.characters());
    ::rmdir(root.characters());

    if (g_failures > 0) {
        outln("==> XDG Trash tests FAILED ({} failure(s))", g_failures);
        return 1;
    }
    outln("==> XDG Trash tests PASSED");
    return 0;
}
