/*
 * Regression: hyperlinks are stored once in the Terminal, not once per cell.
 *
 * VT::Attribute used to hold a ByteString href and an Optional<ByteString>
 * href_id for every cell, even though almost every cell has no hyperlink. That
 * made VT::Line::Cell 56 bytes. The small-index change moves the strings into a
 * terminal-level table and leaves a 32-bit index in the attribute.
 *
 * This test also checks that OSC 8 still resolves the URL and its generated ID,
 * that the closing sequence clears the current attribute, and that two links get
 * distinct table entries.
 */
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <LibVT/Line.h>
#include <LibVT/Terminal.h>

static_assert(sizeof(VT::Attribute) <= 32, "VT::Attribute should not grow back to per-cell hyperlink strings");
static_assert(sizeof(VT::Line::Cell) <= 32, "VT::Line::Cell should not grow back to per-cell hyperlink strings");

namespace {

struct Client final : public VT::TerminalClient {
    virtual void beep() override { }
    virtual void set_window_title(StringView) override { }
    virtual void set_window_progress(VT::ProgressState, u8) override { }
    virtual void terminal_did_resize(u16, u16) override { }
    virtual void terminal_history_changed(int) override { }
    virtual void terminal_did_perform_possibly_partial_clear() override { }
    virtual void emit(u8 const*, size_t) override { }
    virtual void set_cursor_shape(VT::CursorShape) override { }
    virtual void set_cursor_blinking(bool) override { }
    virtual void set_cursor_hidden(bool) override { }
};

void feed(VT::Terminal& terminal, StringView bytes)
{
    for (char byte : bytes)
        terminal.on_input(static_cast<u8>(byte));
}

}

int main()
{
    Client client;
    VT::Terminal terminal(client);
    terminal.set_size(80, 24);

    feed(terminal, "\033]8;;https://example.com/first\033\\link\033]8;;\033\\"sv);

    auto first = terminal.attribute_at({ 0, 0 });
    auto* first_link = terminal.hyperlink(first.hyperlink);
    if (!first.has_hyperlink() || !first_link) {
        warnln("the first OSC 8 link did not produce a hyperlink index");
        return 1;
    }
    if (first_link->href != "https://example.com/first"sv) {
        warnln("wrong first href '{}'", first_link->href);
        return 1;
    }
    if (!first_link->id.has_value()) {
        warnln("the generated hyperlink id is missing");
        return 1;
    }
    for (int column = 0; column < 4; ++column) {
        if (terminal.attribute_at({ 0, column }).hyperlink != first.hyperlink) {
            warnln("column {} does not share the link index {}", column, first.hyperlink);
            return 1;
        }
    }
    if (terminal.attribute_at({ 0, 4 }).has_hyperlink()) {
        warnln("the closing OSC 8 did not clear the hyperlink");
        return 1;
    }

    feed(terminal, " \033]8;id=ignored;https://example.com/second\033\\link\033]8;;\033\\"sv);
    auto second = terminal.attribute_at({ 0, 5 });
    auto* second_link = terminal.hyperlink(second.hyperlink);
    if (!second.has_hyperlink() || !second_link || second.hyperlink == first.hyperlink) {
        warnln("the second link did not get its own table entry");
        return 1;
    }
    if (second_link->href != "https://example.com/second"sv) {
        warnln("wrong second href '{}'", second_link->href);
        return 1;
    }
    if (terminal.hyperlink(first.hyperlink) == nullptr || terminal.hyperlink(first.hyperlink)->href != "https://example.com/first"sv) {
        warnln("the first table entry was lost when the second link was added");
        return 1;
    }

    outln("ok: attribute={} cell={} first={} second={}", sizeof(VT::Attribute), sizeof(VT::Line::Cell), first.hyperlink, second.hyperlink);
    return 0;
}
