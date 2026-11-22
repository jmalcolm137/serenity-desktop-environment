/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibXDG/DesktopEntry.h>
#include <LibXDG/MimeApps.h>
#include <LibXDG/MimeDatabase.h>

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
    outln("=== XDG MIME, MimeApps & DesktopEntry tests ===");

    auto const* tmpdir = getenv("TMPDIR");
    ByteString root_pattern = tmpdir && *tmpdir
        ? ByteString::formatted("{}/sde-xdg-mime.XXXXXX", tmpdir)
        : ByteString { "/tmp/sde-xdg-mime.XXXXXX" };
    Vector<char> buf;
    buf.resize(root_pattern.length() + 1);
    memcpy(buf.data(), root_pattern.characters(), root_pattern.length() + 1);
    char* root_c = mkdtemp(buf.data());
    if (!root_c)
        return Error::from_errno(errno);
    ByteString root { root_c };

    auto cfg_home = ByteString::formatted("{}/cfg-home", root);
    auto cfg_sys = ByteString::formatted("{}/cfg-sys", root);
    auto data_home = ByteString::formatted("{}/data-home", root);
    auto data_sys = ByteString::formatted("{}/data-sys", root);
    auto mime_dir = ByteString::formatted("{}/mime", data_sys);
    auto apps_home = ByteString::formatted("{}/applications", data_home);
    auto apps_sys = ByteString::formatted("{}/applications", data_sys);

    TRY(Core::Directory::create(cfg_home, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(cfg_sys, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(mime_dir, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(apps_home, Core::Directory::CreateDirectories::Yes));
    TRY(Core::Directory::create(apps_sys, Core::Directory::CreateDirectories::Yes));

    // 1. shared-mime-info database: globs2, aliases, subclasses.
    TRY(write_text(ByteString::formatted("{}/globs2", mime_dir),
        "# comment\n"
        "50:text/plain:*.txt\n"
        "50:text/x-c++src:*.cpp\n"
        "50:application/gzip:*.gz\n"
        "80:application/x-compressed-tar:*.tar.gz\n"
        "50:application/pdf:*.pdf\n"sv));
    TRY(write_text(ByteString::formatted("{}/aliases", mime_dir),
        "application/x-pdf application/pdf\n"sv));
    TRY(write_text(ByteString::formatted("{}/subclasses", mime_dir),
        "text/x-c++src text/x-csrc\n"
        "text/x-csrc text/plain\n"sv));

    // 2. .desktop entries.
    TRY(write_text(ByteString::formatted("{}/editor.desktop", apps_sys),
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=MyEditor\n"
        "Icon=my-editor\n"
        "Exec=/bin/true --open %i %c %f\n"
        "MimeType=text/plain;\n"
        "Actions=NewWindow;\n"
        "\n"
        "[Desktop Action NewWindow]\n"
        "Name=New Window\n"
        "Exec=/bin/true --new-window\n"sv));
    TRY(write_text(ByteString::formatted("{}/alt-editor.desktop", apps_sys),
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=AltEditor\n"
        "Exec=/bin/true --alt %U\n"
        "MimeType=text/plain;\n"sv));
    TRY(write_text(ByteString::formatted("{}/browser.desktop", apps_sys),
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=WebBrowser\n"
        "Exec=/bin/true --url %u\n"
        "MimeType=x-scheme-handler/https;text/html;\n"sv));
    TRY(write_text(ByteString::formatted("{}/pdfviewer.desktop", apps_sys),
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=PDFViewer\n"
        "Exec=/bin/true %f\n"
        "MimeType=application/pdf;\n"sv));

    // 3. mimeapps.list hierarchy (system default vs user override & removal).
    TRY(write_text(ByteString::formatted("{}/sde-mimeapps.list", cfg_sys),
        "[Default Applications]\n"
        "text/plain=editor.desktop;\n"
        "x-scheme-handler/https=browser.desktop;\n"sv));
    TRY(write_text(ByteString::formatted("{}/mimeapps.list", cfg_home),
        "[Removed Associations]\n"
        "text/plain=alt-editor.desktop;\n"sv));

    setenv("XDG_CONFIG_HOME", cfg_home.characters(), 1);
    setenv("XDG_CONFIG_DIRS", cfg_sys.characters(), 1);
    setenv("XDG_DATA_HOME", data_home.characters(), 1);
    setenv("XDG_DATA_DIRS", data_sys.characters(), 1);
    setenv("XDG_CURRENT_DESKTOP", "SDE", 1);

    auto db = XDG::MimeDatabase::load();
    check(db.mime_type_for_filename("notes.txt"sv) == "text/plain"sv, "globs2 matches *.txt");
    check(db.mime_type_for_filename("archive.tar.gz"sv) == "application/x-compressed-tar"sv, "globs2 higher weight wins for *.tar.gz");
    check(db.canonical_mime_type("application/x-pdf"sv) == "application/pdf"sv, "alias resolves application/x-pdf -> application/pdf");
    check(db.mime_type_for_uri("https://serenityos.org"sv) == "x-scheme-handler/https"sv, "URI scheme resolves to x-scheme-handler/https");
    check(db.mime_type_for_uri(root) == "inode/directory"sv, "directory path resolves to inode/directory");

    auto supers = db.superclasses("text/x-c++src"sv);
    check(supers.size() == 2 && supers[0] == "text/x-csrc"sv && supers[1] == "text/plain"sv, "transitive subclasses resolved");

    // Default application resolution (including subclass inheritance & alias!).
    auto txt_app = XDG::default_application_for_mime("text/plain"sv, db);
    check(txt_app.has_value() && txt_app->id == "editor.desktop"sv, "sde-mimeapps.list default resolved for text/plain");

    auto cpp_app = XDG::default_application_for_mime("text/x-c++src"sv, db);
    check(cpp_app.has_value() && cpp_app->id == "editor.desktop"sv, "subclass inheritance resolves text/x-c++src -> editor.desktop");

    auto pdf_app = XDG::default_application_for_mime("application/x-pdf"sv, db);
    check(pdf_app.has_value() && pdf_app->id == "pdfviewer.desktop"sv, "alias + .desktop MimeType= resolves application/x-pdf -> pdfviewer.desktop");

    // [Removed Associations] excludes alt-editor.desktop.
    auto assoc = XDG::associated_applications_for_mime("text/plain"sv, db);
    check(assoc.size() == 1 && assoc[0].id == "editor.desktop"sv, "[Removed Associations] filters out alt-editor.desktop");

    // DesktopEntry field code expansion (%i, %c, %f) and Actions.
    check(txt_app.has_value() && txt_app->actions.size() == 1 && txt_app->actions[0].id == "NewWindow"sv, "Desktop Action parsed");
    auto argv = txt_app->expand_exec({ "file:///tmp/hello.txt" });
    check(argv.size() == 6
            && argv[0] == "/bin/true"sv
            && argv[1] == "--open"sv
            && argv[2] == "--icon"sv
            && argv[3] == "my-editor"sv
            && argv[4] == "MyEditor"sv
            && argv[5] == "/tmp/hello.txt"sv,
        "Exec= expands %i, %c and %f (stripping file://)");

    // set_default_application_for_mime persists override to $XDG_CONFIG_HOME/mimeapps.list.
    TRY(XDG::set_default_application_for_mime("text/plain"sv, "alt-editor.desktop"sv));
    auto updated = XDG::default_application_for_mime("text/plain"sv, db);
    check(updated.has_value() && updated->id == "alt-editor.desktop"sv, "set_default_application_for_mime overrides system default");

    // Cleanup.
    for (auto p : {
             ByteString::formatted("{}/globs2", mime_dir),
             ByteString::formatted("{}/aliases", mime_dir),
             ByteString::formatted("{}/subclasses", mime_dir),
             ByteString::formatted("{}/editor.desktop", apps_sys),
             ByteString::formatted("{}/alt-editor.desktop", apps_sys),
             ByteString::formatted("{}/browser.desktop", apps_sys),
             ByteString::formatted("{}/pdfviewer.desktop", apps_sys),
             ByteString::formatted("{}/sde-mimeapps.list", cfg_sys),
             ByteString::formatted("{}/mimeapps.list", cfg_home),
         })
        ::unlink(p.characters());
    for (auto d : { mime_dir, apps_home, apps_sys, cfg_home, cfg_sys, data_home, data_sys, root })
        ::rmdir(d.characters());

    if (g_failures > 0) {
        outln("==> XDG MIME tests FAILED ({} failure(s))", g_failures);
        return 1;
    }
    outln("==> XDG MIME tests PASSED");
    return 0;
}
