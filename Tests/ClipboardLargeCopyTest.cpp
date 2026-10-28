/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: a Terminal lost its clipboard connection and exited after a large
// copy. The clipboard client did a synchronous fetch from inside
// Clipboard::clipboard_content_did_change(), re-entering the clipboard
// connection's message dispatch; the default ConnectionToServer::die() then
// called exit(0) when the connection was lost.
//
// The test seeds a real input serial (the host compositor only accepts
// set_selection from a recent input event), copies 1 MiB, and performs the same
// synchronous fetch. It only passes if the process survives and the data is
// intact; an early exit(0) leaves no success line, which the driver treats as a
// failure.
//
// The driver is scripts/run-clipboard-large-copy-test.sh.
#include <AK/StringBuilder.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibGUI/Application.h>
#include <LibGUI/Button.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/Event.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <stdio.h>
#include <unistd.h>

namespace {

constexpr size_t payload_size = 1024 * 1024;

struct FetchingClient : public GUI::Clipboard::ClipboardClient {
    int changes { 0 };
    bool fetched { false };
    bool data_ok { false };

    virtual void clipboard_content_did_change(ByteString const&) override
    {
        ++changes;
        auto [data, mime_type, _] = GUI::Clipboard::the().fetch_data_and_type();
        fetched = true;
        data_ok = mime_type.starts_with("text/"sv) && data.size() == payload_size;
    }
};

String make_payload()
{
    StringBuilder builder;
    for (size_t i = 0; i < payload_size; ++i)
        builder.append(static_cast<char>('a' + (i % 26)));
    return MUST(builder.to_string());
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    (void)app;

    bool clicked = false;

    auto window = GUI::Window::construct();
    window->set_title("Clipboard Large Copy Test");
    window->set_rect(100, 100, 300, 200);
    auto button = window->set_main_widget<GUI::Button>();
    button->set_text("Click"_string);
    button->on_click = [&](unsigned) { clicked = true; };
    window->show();

    // The driver waits for this line, then injects a click so the host
    // compositor gives us an input serial.
    outln("READY");
    fflush(stdout);

    Core::ElapsedTimer startup_timer;
    startup_timer.start();
    while (!clicked && startup_timer.elapsed() < 15000) {
        Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
        usleep(1000);
    }
    if (!clicked) {
        warnln("FAIL: the input seed click did not arrive");
        return 1;
    }

    auto payload = make_payload();
    FetchingClient client;

    GUI::Clipboard::the().set_plain_text(payload.bytes_as_string_view());

    // Let the host report the new selection before fetching it.
    Core::ElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1000) {
        Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
        usleep(1000);
    }

    // This is the synchronous fetch the Terminal used to do from inside the
    // change callback. The change event itself also fetches through the client
    // if the host reports the selection change.
    auto [data, mime_type, _] = GUI::Clipboard::the().fetch_data_and_type();
    bool data_ok = mime_type.starts_with("text/"sv) && data.size() == payload_size;

    if (!data_ok) {
        warnln("FAIL: the fetched clipboard data did not survive ({} bytes)", data.size());
        return 1;
    }
    if (client.fetched && !client.data_ok) {
        warnln("FAIL: the change callback fetched different clipboard data");
        return 1;
    }

    outln("ok: a large clipboard copy keeps the connection alive and the data intact");
    return 0;
}
