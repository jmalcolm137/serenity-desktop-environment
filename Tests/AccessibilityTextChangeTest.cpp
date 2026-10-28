/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: every TextEditor change rebuilt the AT-SPI snapshot (including
// the whole document text) and sent that text in the TextChanged signal. A large
// paste therefore froze the UI for an extended period. The event is now a
// notification; snapshot path lookup does not build text, and the AT-SPI client
// queries the Text interface when it needs the contents.
//
// The driver is scripts/run-accessibility-text-change-test.sh.
#include <AK/StringBuilder.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibGUI/AccessibilityServer.h>
#include <LibGUI/Application.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <unistd.h>

namespace {

constexpr size_t document_size = 4 * 1024 * 1024;
constexpr int notifications = 50;

String make_text()
{
    StringBuilder builder;
    for (size_t i = 0; i < document_size; ++i)
        builder.append(static_cast<char>('a' + (i % 26)));
    return MUST(builder.to_string());
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    (void)app;

    auto window = GUI::Window::construct();
    window->set_title("Accessibility Text Change Test");
    window->set_rect(100, 100, 400, 300);
    auto editor = window->set_main_widget<GUI::TextEditor>();
    window->show();

    auto text = make_text();
    editor->set_text(text.bytes_as_string_view());

    Core::ElapsedTimer timer;
    timer.start();
    for (int i = 0; i < notifications; ++i)
        GUI::AccessibilityServer::notify_text_changed(*editor);
    auto elapsed = timer.elapsed();

    if (elapsed > 1000) {
        warnln("FAIL: {} text-change notifications took {} ms", notifications, elapsed);
        return 1;
    }

    outln("ok: accessibility text notifications do not copy the document ({} notifications in {} ms)", notifications, elapsed);
    return 0;
}
