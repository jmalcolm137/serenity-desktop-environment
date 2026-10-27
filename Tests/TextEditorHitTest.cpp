/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: TextEditor::text_position_at_content_position() scanned every line
// (twice) and called Document::line_is_visible() for each of them -- itself a
// scan over the folding regions. It runs for every mouse move and selection drag
// and four times per paint_event, so a large document made the editor crawl.
//
// It now binary-searches the visual-rect tops that recompute_all_visual_lines()
// lays out in order. This checks the new lookup against the definition the old
// scan implemented -- "the visible line whose visual rect contains y" -- for
// every y, including the boundaries:
//
//   - uniform NoWrap lines (a plain O(lines) index),
//   - folded regions (folded lines have zero-height rects and are skipped),
//   - wrapped lines (variable-height rects, several rows per line),
//   - y < 0 (first line) and y past the end of the content (last visible line).
//
// The driver is scripts/run-texteditor-hittest-test.sh.
#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibGUI/Application.h>
#include <LibGUI/TextEditor.h>
#include <LibMain/Main.h>

namespace {

// A TextEditor that exposes the protected layout queries the assertions need.
struct ProbeEditor : public GUI::TextEditor {
    ProbeEditor()
        : GUI::TextEditor(Type::MultiLine)
    {
    }

    Gfx::IntRect content_rect_for(GUI::TextPosition const& position) const { return content_rect_for_position(position); }
    void set_folding_regions(Vector<GUI::TextDocumentFoldingRegion> regions) { highlighter_did_set_folding_regions(move(regions)); }
};

int g_failures = 0;

void check_line(ProbeEditor const& editor, char const* what, int y, size_t expected)
{
    auto actual = editor.text_position_at_content_position({ 4, y }).line();
    if (actual == expected)
        return;
    if (g_failures < 20)
        warnln("FAIL: {}: y={} expected line {} got {}", what, y, expected, actual);
    ++g_failures;
}

String make_text(size_t line_count)
{
    StringBuilder builder;
    // Lines are short enough never to wrap, and end in '\n', so the document
    // has one more (empty) line than the number of formatted lines.
    for (size_t i = 0; i < line_count; ++i)
        builder.appendff("line {}\n", i);
    return MUST(builder.to_string());
}

void test_uniform_lines(ProbeEditor& editor)
{
    editor.set_wrapping_mode(GUI::TextEditor::WrappingMode::NoWrap);
    editor.resize(600, 320);
    editor.set_text(make_text(200));

    auto line_height = editor.line_height();
    auto line_count = editor.line_count();
    if (line_height <= 0 || line_count < 200) {
        warnln("FAIL: unexpected layout: line_height={} line_count={}", line_height, line_count);
        ++g_failures;
        return;
    }

    // y/h is the line index for uniform rects; past the content the last line
    // gets the click, and a negative y clamps to the first line.
    for (int y = -4; y < static_cast<int>(line_count) * line_height + 3 * line_height; ++y) {
        size_t expected = y < 0 ? 0 : min(static_cast<size_t>(y) / static_cast<size_t>(line_height), line_count - 1);
        check_line(editor, "uniform", y, expected);
    }
}

void test_folded_lines(ProbeEditor& editor)
{
    editor.set_wrapping_mode(GUI::TextEditor::WrappingMode::NoWrap);
    editor.resize(600, 320);
    editor.set_text(make_text(40));

    // Fold lines 11..19 (Document hides the lines strictly between start and
    // end, keeping both ends visible). The folded lines keep zero-height rects,
    // so the lines after the fold move up by nine line heights.
    editor.set_folding_regions({ { { GUI::TextPosition { 10, 0 }, GUI::TextPosition { 19, 0 } }, true, nullptr } });

    auto line_height = editor.line_height();
    auto line_count = editor.line_count();
    Vector<size_t> visible_lines;
    for (size_t i = 0; i < line_count; ++i) {
        if (editor.document().line_is_visible(i))
            visible_lines.append(i);
    }
    if (visible_lines.size() != line_count - 8 || line_height <= 0) {
        warnln("FAIL: fold not applied: {} of {} lines visible", visible_lines.size(), line_count);
        ++g_failures;
        return;
    }

    auto total_height = static_cast<int>(visible_lines.size()) * line_height;
    for (int y = -4; y < total_height + 3 * line_height; ++y) {
        size_t expected = y < 0 ? visible_lines.first() : visible_lines[min(static_cast<size_t>(y) / static_cast<size_t>(line_height), visible_lines.size() - 1)];
        check_line(editor, "folded", y, expected);
    }
}

void test_wrapped_lines(ProbeEditor& editor)
{
    editor.set_folding_regions({});
    editor.set_wrapping_mode(GUI::TextEditor::WrappingMode::WrapAnywhere);
    editor.resize(200, 320);

    // Long enough that every line wraps over several visual rows, so the rects
    // the binary search walks are variable-height.
    StringBuilder builder;
    for (size_t i = 0; i < 12; ++i) {
        builder.appendff("wrapped line {} ", i);
        for (size_t j = 0; j < 120; ++j)
            builder.append('x');
        builder.append('x');
        builder.append('\n');
    }
    editor.set_text(MUST(builder.to_string()));

    auto content_height = editor.content_height();
    if (content_height <= 0) {
        warnln("FAIL: wrapped text produced no content height");
        ++g_failures;
        return;
    }

    // Every visible line must own the top row of its visual block, and every row
    // inside that block must map back to the same line.
    for (size_t i = 0; i < editor.line_count(); ++i) {
        if (!editor.document().line_is_visible(i))
            continue;
        auto rect = editor.content_rect_for(GUI::TextPosition { i, 0 });
        if (rect.is_empty()) {
            warnln("FAIL: wrapped line {} has no content rect", i);
            ++g_failures;
            continue;
        }
        check_line(editor, "wrapped-top", rect.top(), i);
        check_line(editor, "wrapped-bottom", rect.bottom() - 1, i);
    }

    // The mapping is monotonic and covers the content; past the end it is the
    // last visible line.
    size_t previous = 0;
    for (int y = 0; y < content_height; ++y) {
        auto line = editor.text_position_at_content_position({ 4, y }).line();
        if (line < previous) {
            warnln("FAIL: wrapped mapping went backwards at y={}: {} after {}", y, line, previous);
            ++g_failures;
            break;
        }
        previous = line;
    }
    size_t last_visible = editor.line_count() - 1;
    while (last_visible > 0 && !editor.document().line_is_visible(last_visible))
        --last_visible;
    check_line(editor, "wrapped-past-end", content_height + 50, last_visible);
}

void test_degenerate_documents(ProbeEditor& editor)
{
    editor.set_folding_regions({});
    editor.set_wrapping_mode(GUI::TextEditor::WrappingMode::NoWrap);
    editor.resize(600, 320);

    editor.set_text(""sv);
    check_line(editor, "empty", -5, 0);
    check_line(editor, "empty", 0, 0);
    check_line(editor, "empty", 10000, 0);

    editor.set_text("only one line"sv);
    check_line(editor, "one-line", 0, 0);
    check_line(editor, "one-line", 10000, 0);
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    (void)app;

    auto editor = adopt_ref(*new ProbeEditor);

    test_uniform_lines(*editor);
    test_folded_lines(*editor);
    test_wrapped_lines(*editor);
    test_degenerate_documents(*editor);

    if (g_failures != 0) {
        warnln("{} check(s) failed", g_failures);
        return 1;
    }
    outln("ok: text editor hit-testing matches the visual line layout");
    return 0;
}
