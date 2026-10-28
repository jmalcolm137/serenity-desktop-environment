/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression: pasting a large document inserted one code point at a time and
// notified the editor after every character. Each notification walked the whole
// document (update_content_size) and inserted one line, so a 1 MiB paste was
// quadratic and froze the UI.
//
// This measures one bulk insertion of a 1 MiB, many-line document. The old path
// takes far longer than the timeout; the batched path completes quickly and the
// content must be intact.
//
// The driver is scripts/run-texteditor-large-paste-test.sh.
#include <AK/StringBuilder.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGUI/Application.h>
#include <LibGUI/TextEditor.h>
#include <LibMain/Main.h>

namespace {

constexpr size_t payload_size = 1024 * 1024;

struct ProbeEditor : public GUI::TextEditor {
    C_OBJECT(ProbeEditor)

    ProbeEditor()
        : GUI::TextEditor(Type::MultiLine)
    {
    }
};

String make_text()
{
    StringBuilder builder;
    for (size_t i = 0; i < payload_size; ++i) {
        if (i % 80 == 79)
            builder.append('\n');
        else
            builder.append(static_cast<char>('a' + (i % 26)));
    }
    return MUST(builder.to_string());
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));
    (void)app;

    auto editor = ProbeEditor::construct();
    editor->resize(800, 600);
    editor->set_text(""sv);

    auto payload = make_text();
    Core::ElapsedTimer timer;
    timer.start();
    editor->insert_at_cursor_or_replace_selection(payload.bytes_as_string_view());
    auto elapsed = timer.elapsed();

    auto payload_length = payload.bytes_as_string_view().length();
    if (editor->text().length() != payload_length) {
        warnln("FAIL: inserted {} bytes, expected {}", editor->text().length(), payload_length);
        return 1;
    }
    if (elapsed > 2000) {
        warnln("FAIL: inserting {} bytes took {} ms", payload_size, elapsed);
        return 1;
    }

    outln("ok: 1 MiB text inserted in {} ms", elapsed);
    return 0;
}
