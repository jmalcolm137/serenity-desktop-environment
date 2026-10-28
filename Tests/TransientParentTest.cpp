/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: a transient child outlived its parent. Destroying the parent left
// the child's `transient_parent` pointing at freed memory; the next pointer
// motion hit-tested the child, walked the modal chain, and crashed the
// compositor.
//
// This opens a parent window and a blocking "Unsaved Changes" dialog, closes
// the parent, and keeps the dialog alive. The driver moves the pointer over the
// dialog; the compositor must survive and the client must still be running.
//
// The driver is scripts/run-transient-parent-test.sh.
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGUI/Application.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <stdio.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto parent = GUI::Window::construct();
    parent->set_title("Transient Parent Test");
    parent->set_rect(100, 100, 400, 300);
    auto widget = parent->set_main_widget<GUI::Widget>();
    widget->set_fill_with_background_color(true);
    parent->show();

    auto dialog = TRY(GUI::MessageBox::create(
        parent.ptr(), "Close without saving?"sv, "Unsaved Changes"sv,
        GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNoCancel));
    dialog->set_screen_position(GUI::Dialog::ScreenPosition::DoNotPosition);
    dialog->GUI::Window::show();

    // A second transient child is the blocker that makes the hit-test walk the
    // first child's parent chain.
    auto second_dialog = TRY(GUI::MessageBox::create(
        parent.ptr(), "Another unsaved change"sv, "Unsaved Changes 2"sv,
        GUI::MessageBox::Type::Warning, GUI::MessageBox::InputType::YesNoCancel));
    second_dialog->set_screen_position(GUI::Dialog::ScreenPosition::DoNotPosition);
    second_dialog->set_rect(300, 300, 340, 92);
    second_dialog->GUI::Window::show();

    // Let both windows map, then destroy the parent while the transient dialog
    // stays alive. The compositor must clear the dialog's parent pointer.
    Core::ElapsedTimer startup_timer;
    startup_timer.start();
    while (startup_timer.elapsed() < 500) {
        Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
        usleep(1000);
    }
    parent->close();

    startup_timer.start();
    while (startup_timer.elapsed() < 500) {
        Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
        usleep(1000);
    }

    outln("READY");
    fflush(stdout);

    auto finish_timer = Core::Timer::create_single_shot(4000, [&] {
        outln("SURVIVED");
        fflush(stdout);
        app->quit(0);
    });
    finish_timer->start();

    return app->exec();
}
