/*
 * Regression: "Clear to previous mark" after the scrollback ring has wrapped
 * crashes the Terminal (SIGILL out of AK::Vector::insert).
 *
 * VT's scrollback is a ring buffer: while it is full, Terminal::add_line_to_history
 * overwrites the oldest line at m_history_start and advances that index. The rest of
 * the history code, however, treats m_history as an ordered list. In particular
 * Terminal::clear_to_mark (reached from TerminalWidget::clear_to_previous_mark)
 * removes lines with m_history.remove() and never touches m_history_start.
 *
 * A mark whose line the ring has since overwritten stays in m_valid_marks, so
 * clear_to_previous_mark can hand clear_to_mark a mark that is neither in the
 * active buffer nor in the history. clear_to_mark then wipes the whole history,
 * leaving m_history_start pointing past the end of an empty vector. The next
 * linefeed scrolls a line into history, and add_line_to_history() inserts at
 * m_history_start - 1 -- out of bounds. AK::Vector::insert VERIFYs the index and
 * traps (Terminal.h:419).
 *
 * The exit status is the assertion: dying by signal is the bug.
 */
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <LibVT/Terminal.h>

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

constexpr size_t history_limit = 8;

}

int main()
{
    Client client;
    VT::Terminal terminal(client);
    terminal.set_size(20, 5);
    terminal.set_max_history_size(history_limit);

    // Mark the current line; scrolling it off-screen and then past the end of the
    // ring leaves the mark set holding a line that no longer exists.
    terminal.mark_cursor();

    // Fill the history. The scroll that fills it still appends, so the ring index
    // is still 0 here.
    for (size_t guard = 0; terminal.history_size() < terminal.max_history_size(); ++guard) {
        if (guard > history_limit + terminal.rows() + 8) {
            warnln("history never filled; the test needs a full ring");
            return 1;
        }
        terminal.on_input('\n');
    }

    // Two more scrolls now overwrite lines and advance the ring index to 2.
    terminal.on_input('\n');
    terminal.on_input('\n');

    auto marks = terminal.marks().values();
    if (marks.is_empty()) {
        warnln("no mark survived the history wrap; the test needs a stale mark");
        return 1;
    }

    // The "clear to previous mark" action, with a mark whose line is gone: this
    // wipes the history without resetting the ring index.
    terminal.clear_to_mark(marks.first());

    // Back at the bottom, a newline scrolls a line into history, where it used to
    // insert out of bounds.
    terminal.set_cursor(terminal.rows() - 1, 0);
    terminal.on_input('\n');

    if (terminal.history_size() != 1) {
        warnln("expected the scrolled line back in history, got {}", terminal.history_size());
        return 1;
    }
    outln("ok: history_size={} after clearing to a stale mark", terminal.history_size());
    return 0;
}
