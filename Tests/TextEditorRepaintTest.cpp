/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: TextEditor repainted the whole widget on every selection change
// (and every drag-select mouse move), and called on_modified_change on every
// undo-stack update even when the document's modified state had not changed.
//
// Selection highlighting is drawn per physical line, so a selection change only
// needs to damage the lines covered by the old and the new selection. The test
// opens a real (headless) window, records the rects delivered to paint_event(),
// and checks that:
//
//   - selecting lines 5..9 damages those lines and nothing else,
//   - moving the selection to lines 20..22 damages the old band as well as the
//     new one (so the old highlight is removed), and
//   - clearing the selection damages only the previously selected band.
//
// It also checks that on_modified_change fires on the false -> true and
// true -> false transitions, not on every intermediate undo-stack update.
//
// The driver is scripts/run-texteditor-repaint-test.sh.
#include "LibWM.h"
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibGUI/Application.h>
#include <LibGUI/Event.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>

namespace {

struct PaintRecordingEditor : public GUI::TextEditor {
    C_OBJECT(PaintRecordingEditor)

    PaintRecordingEditor()
        : GUI::TextEditor(Type::MultiLine)
    {
    }

    Vector<Gfx::IntRect> paints;

    void clear_paints() { paints.clear(); }

    // keydown_event() is protected in Widget; the test only needs Backspace.
    void press_backspace()
    {
        GUI::KeyEvent event(GUI::Event::Type::KeyDown, KeyCode::Key_Backspace, 0, 0, 0, 0);
        keydown_event(event);
    }

    virtual void paint_event(GUI::PaintEvent& event) override
    {
        paints.append(event.rect());
        GUI::TextEditor::paint_event(event);
    }
};

int g_failures = 0;

void fail(char const* what)
{
    if (g_failures < 20)
        warnln("FAIL: {}", what);
    ++g_failures;
}

String make_text(size_t line_count)
{
    StringBuilder builder;
    for (size_t i = 0; i < line_count; ++i)
        builder.appendff("line {}\n", i);
    return MUST(builder.to_string());
}

struct Band {
    int top;
    int bottom;
};

bool intersects(Band const& band, Gfx::IntRect const& rect)
{
    return rect.top() < band.bottom && rect.bottom() > band.top;
}

bool contained_in(Band const& band, Gfx::IntRect const& rect)
{
    return rect.top() >= band.top && rect.bottom() <= band.bottom;
}

// Repaint only happens if the window is visible and has a window id; flush the
// pending damage synchronously so the recorded rects are exactly what the last
// selection change asked for.
void flush(GUI::Window& window, PaintRecordingEditor& editor)
{
    window.flush_pending_paints_immediately();
    editor.clear_paints();
}

void expect_paints_only_in(PaintRecordingEditor const& editor, Vector<Band> const& allowed, char const* what)
{
    if (editor.paints.is_empty()) {
        fail(what);
        return;
    }
    for (auto const& rect : editor.paints) {
        bool ok = false;
        for (auto const& band : allowed) {
            if (contained_in(band, rect)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            warnln("FAIL: {}: paint rect {} is outside the expected line bands", what, rect);
            ++g_failures;
            return;
        }
    }
}

bool band_was_painted(PaintRecordingEditor const& editor, Band const& band)
{
    for (auto const& rect : editor.paints) {
        if (intersects(band, rect))
            return true;
    }
    return false;
}

void test_selection_damage(GUI::Application& app)
{
    (void)app;

    auto window = GUI::Window::construct();
    window->set_title("TextEditor Repaint Test");
    window->set_rect(80, 80, 480, 400);

    auto editor = window->set_main_widget<PaintRecordingEditor>();
    editor->set_wrapping_mode(GUI::TextEditor::WrappingMode::NoWrap);
    editor->set_text(make_text(60));
    window->show();

    // The first paint creates the backing store and therefore repaints
    // everything; discard it before measuring selection damage.
    window->flush_pending_paints_immediately();
    editor->clear_paints();

    auto line_height = editor->line_height();
    auto frame_thickness = editor->frame_thickness();
    if (line_height <= 0 || editor->line_count() < 60) {
        warnln("FAIL: unexpected layout: line_height={} line_count={}", line_height, editor->line_count());
        ++g_failures;
        return;
    }

    auto band_for = [&](size_t first, size_t last) {
        return Band { frame_thickness + static_cast<int>(first) * line_height,
            frame_thickness + static_cast<int>(last + 1) * line_height };
    };

    auto first_band = band_for(5, 9);
    auto second_band = band_for(20, 22);

    // Select lines 5..9. Only those lines can have changed.
    editor->selection().set({ 5, 0 }, { 9, 0 });
    editor->did_update_selection();
    window->flush_pending_paints_immediately();

    expect_paints_only_in(*editor, { first_band }, "selecting lines 5..9 damaged other lines");
    if (!band_was_painted(*editor, first_band))
        fail("selecting lines 5..9 did not damage them");
    editor->clear_paints();

    // Move the selection to 20..22. The old band must be damaged as well, or the
    // old highlight would stay on screen.
    editor->selection().set({ 20, 0 }, { 22, 0 });
    editor->did_update_selection();
    window->flush_pending_paints_immediately();

    expect_paints_only_in(*editor, { first_band, second_band }, "moving the selection damaged unrelated lines");
    if (!band_was_painted(*editor, first_band))
        fail("moving the selection did not damage the old selection band");
    if (!band_was_painted(*editor, second_band))
        fail("moving the selection did not damage the new selection band");
    editor->clear_paints();

    // Clearing it must damage the band that was highlighted.
    editor->selection().clear();
    editor->did_update_selection();
    window->flush_pending_paints_immediately();

    expect_paints_only_in(*editor, { second_band }, "clearing the selection damaged unrelated lines");
    if (!band_was_painted(*editor, second_band))
        fail("clearing the selection did not damage the previous selection band");
    editor->clear_paints();

    // A public set_selection() does a full repaint. The next incremental
    // selection change must remember that band as the one on screen; otherwise
    // the old highlight would survive.
    auto third_band = band_for(18, 19);
    auto fourth_band = band_for(20, 21);
    editor->set_selection({ { 18, 0 }, { 19, 0 } });
    window->flush_pending_paints_immediately();
    editor->clear_paints();

    editor->selection().set({ 20, 0 }, { 21, 0 });
    editor->did_update_selection();
    window->flush_pending_paints_immediately();

    expect_paints_only_in(*editor, { third_band, fourth_band }, "a selection change after set_selection() damaged unrelated lines");
    if (!band_was_painted(*editor, third_band))
        fail("a selection change after set_selection() did not damage the old selection band");
    if (!band_was_painted(*editor, fourth_band))
        fail("a selection change after set_selection() did not damage the new selection band");

    outln("ok: selection changes repaint only the affected lines");
}

void test_modified_callback()
{
    auto editor = PaintRecordingEditor::construct();
    editor->set_text("hello"sv);

    Vector<bool> notifications;
    editor->on_modified_change = [&](bool modified) {
        notifications.append(modified);
    };

    // Each edit is an undo-stack update, but the modified state only changes on
    // the first one. The two inserts merge; the backspace is a different
    // command and therefore another push that does not change the state.
    editor->insert_at_cursor_or_replace_selection("a"sv);
    editor->insert_at_cursor_or_replace_selection("b"sv);
    editor->press_backspace();

    if (notifications.size() != 1 || !notifications[0]) {
        warnln("FAIL: expected one modified=true notification, got {} (first={})", notifications.size(), notifications.is_empty() ? "none" : notifications[0] ? "true" : "false");
        ++g_failures;
    }

    // Undo until the document is unmodified. Undoing an edit that leaves the
    // document modified must not notify again; the transition back to the
    // unmodified state must notify exactly once, with false.
    bool saw_unmodified = false;
    for (int i = 0; i < 4 && !saw_unmodified; ++i) {
        editor->undo();
        if (!editor->document().is_modified()) {
            saw_unmodified = true;
            if (notifications.size() != 2 || notifications[1]) {
                warnln("FAIL: returning to the unmodified state should notify once with false, got {} notifications", notifications.size());
                ++g_failures;
            }
        } else if (notifications.size() != 1) {
            warnln("FAIL: undo while still modified notified again ({} notifications)", notifications.size());
            ++g_failures;
            break;
        }
    }
    if (!saw_unmodified) {
        fail("undoing the edits never returned the document to unmodified");
    }

    outln("ok: on_modified_change fires only on modified-state transitions");
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    LibWM::initialize();

    auto app = TRY(GUI::Application::create(arguments));

    test_selection_damage(*app);
    test_modified_callback();

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: text editor repaints only what changed");
    return 0;
}
