/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibXDG/IconTheme.h>

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
    outln("=== XDG Icon Theme tests ===");

    auto const* tmpdir = getenv("TMPDIR");
    ByteString root_pattern = tmpdir && *tmpdir
        ? ByteString::formatted("{}/sde-xdg-icons.XXXXXX", tmpdir)
        : ByteString { "/tmp/sde-xdg-icons.XXXXXX" };
    Vector<char> buf;
    buf.resize(root_pattern.length() + 1);
    memcpy(buf.data(), root_pattern.characters(), root_pattern.length() + 1);
    char* root_c = mkdtemp(buf.data());
    if (!root_c)
        return Error::from_errno(errno);
    ByteString root { root_c };

    auto data_home = ByteString::formatted("{}/data-home", root);
    auto data_sys = ByteString::formatted("{}/data-sys", root);
    auto child_16 = ByteString::formatted("{}/icons/CustomTheme/16x16/apps", data_home);
    auto child_32 = ByteString::formatted("{}/icons/CustomTheme/32x32/apps", data_home);
    auto parent_svg = ByteString::formatted("{}/icons/ParentTheme/scalable/apps", data_sys);
    auto hicolor_16 = ByteString::formatted("{}/icons/hicolor/16x16/apps", data_sys);
    auto pixmaps = ByteString::formatted("{}/pixmaps", data_sys);
    auto apps_sys = ByteString::formatted("{}/applications", data_sys);

    for (auto const& d : { child_16, child_32, parent_svg, hicolor_16, pixmaps, apps_sys })
        TRY(Core::Directory::create(d, Core::Directory::CreateDirectories::Yes));

    TRY(write_text(ByteString::formatted("{}/icons/CustomTheme/index.theme", data_home),
        "[Icon Theme]\n"
        "Name=CustomTheme\n"
        "Inherits=ParentTheme\n"
        "Directories=16x16/apps,32x32/apps\n\n"
        "[16x16/apps]\nSize=16\nType=Fixed\n\n"
        "[32x32/apps]\nSize=32\nType=Threshold\nThreshold=2\n"sv));
    TRY(write_text(ByteString::formatted("{}/icons/ParentTheme/index.theme", data_sys),
        "[Icon Theme]\n"
        "Name=ParentTheme\n"
        "Directories=scalable/apps\n\n"
        "[scalable/apps]\nSize=48\nMinSize=16\nMaxSize=256\nType=Scalable\n"sv));

    auto exact16 = ByteString::formatted("{}/demo-app.png", child_16);
    auto exact32 = ByteString::formatted("{}/demo-app.png", child_32);
    auto inherited_svg = ByteString::formatted("{}/inherited-app.svg", parent_svg);
    auto hicolor_icon = ByteString::formatted("{}/only-hicolor.png", hicolor_16);
    auto pixmap_icon = ByteString::formatted("{}/legacy-app.png", pixmaps);

    TRY(write_text(exact16, "png16"sv));
    TRY(write_text(exact32, "png32"sv));
    TRY(write_text(inherited_svg, "<svg/>"sv));
    TRY(write_text(hicolor_icon, "hicolor16"sv));
    TRY(write_text(pixmap_icon, "pixmap"sv));

    TRY(write_text(ByteString::formatted("{}/org.example.Demo.desktop", apps_sys),
        "[Desktop Entry]\nType=Application\nName=Demo\nExec=/bin/true\nIcon=only-hicolor\n"sv));

    setenv("HOME", root.characters(), 1);
    setenv("XDG_DATA_HOME", data_home.characters(), 1);
    setenv("XDG_DATA_DIRS", data_sys.characters(), 1);

    check(XDG::find_icon("demo-app"sv, 16, 1, "CustomTheme"sv) == exact16, "exact 16x16 match in CustomTheme");
    check(XDG::find_icon("demo-app"sv, 32, 1, "CustomTheme"sv) == exact32, "exact 32x32 match in CustomTheme");
    check(XDG::find_icon("demo-app"sv, 24, 1, "CustomTheme"sv) == exact16 || XDG::find_icon("demo-app"sv, 24, 1, "CustomTheme"sv) == exact32, "closest size distance match");
    check(XDG::find_icon("inherited-app"sv, 32, 1, "CustomTheme"sv) == inherited_svg, "Inherits= resolves ParentTheme scalable SVG");
    check(XDG::find_icon("only-hicolor"sv, 16, 1, "CustomTheme"sv) == hicolor_icon, "falls back to hicolor");
    check(XDG::find_icon("legacy-app"sv, 16, 1, "CustomTheme"sv) == pixmap_icon, "falls back to pixmaps");
    check(XDG::find_icon(exact16, 16, 1, "CustomTheme"sv) == exact16, "direct absolute path returned");
    check(XDG::find_app_icon("org.example.Demo"sv, 16, 1, "CustomTheme"sv) == hicolor_icon, "find_app_icon resolves .desktop Icon= via app_id");

    // Cleanup.
    for (auto const& p : {
             exact16, exact32, inherited_svg, hicolor_icon, pixmap_icon,
             ByteString::formatted("{}/icons/CustomTheme/index.theme", data_home),
             ByteString::formatted("{}/icons/ParentTheme/index.theme", data_sys),
             ByteString::formatted("{}/org.example.Demo.desktop", apps_sys),
         })
        ::unlink(p.characters());
    for (auto const& d : {
             child_16, child_32,
             ByteString::formatted("{}/icons/CustomTheme/16x16", data_home),
             ByteString::formatted("{}/icons/CustomTheme/32x32", data_home),
             ByteString::formatted("{}/icons/CustomTheme", data_home),
             ByteString::formatted("{}/icons", data_home),
             parent_svg,
             ByteString::formatted("{}/icons/ParentTheme/scalable", data_sys),
             ByteString::formatted("{}/icons/ParentTheme", data_sys),
             hicolor_16,
             ByteString::formatted("{}/icons/hicolor/16x16", data_sys),
             ByteString::formatted("{}/icons/hicolor", data_sys),
             ByteString::formatted("{}/icons", data_sys),
             pixmaps, apps_sys, data_home, data_sys, root,
         })
        ::rmdir(d.characters());

    if (g_failures > 0) {
        outln("==> XDG Icon Theme tests FAILED ({} failure(s))", g_failures);
        return 1;
    }
    outln("==> XDG Icon Theme tests PASSED");
    return 0;
}
