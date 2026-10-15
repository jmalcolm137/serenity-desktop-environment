# The Terminal scrollback-ring crash

Status: fixed — `libgui-wayland` patch `0064-libvt-history-ring-index.patch`, guarded
by `scripts/run-vt-history-test.sh` (test/VtHistoryTest.cpp).

## The crash

Terminal died with `SIGILL` during ordinary output. `coredumpctl` put the trap in
`AK::Vector::insert`'s `VERIFY(index <= size())`:

```
#1 try_insert<AK::NonnullOwnPtr<VT::Line>> AK/Vector.h:553
#2 insert<AK::NonnullOwnPtr<VT::Line>>     AK/Vector.h:282
#3 VT::Terminal::add_line_to_history       LibVT/Terminal.h:419
#5 VT::Terminal::scroll_up                 LibVT/Terminal.cpp:865
#7 VT::Terminal::linefeed                  LibVT/Terminal.cpp:820
```

`Terminal.h:419` is `m_history.insert(m_history_start - 1, move(line))`.

## Why

The scrollback is a ring buffer. `add_line_to_history()` treats `m_history` as a
plain vector while it still has room, and once it is full it overwrites the oldest
line at `m_history_start` and advances that index:

```cpp
if (m_history.size() < max_history_size()) { /* append, or insert at start - 1 */ }
m_history[m_history_start] = move(line);
m_history_start = (m_history_start + 1) % m_history.size();
```

Two other paths, however, mutate `m_history` as an *ordered list* and never touch
`m_history_start`:

* `Terminal::clear_to_mark()` — **Clear to previous mark** — `m_history.remove(...)`,
* `Terminal::set_size()` — the rewrap on resize — `extend` / `take_last` / `prepend`.

Once the ring has wrapped (`m_history_start != 0`), either can leave fewer lines
than the index. The next `add_line_to_history()` then computes `m_history_start - 1`,
past the end, and the `insert` VERIFY trips.

**Clear to previous mark reaches it readily.** A mark survives in `m_valid_marks`
after the ring has overwritten the line it was on (`m_valid_marks` is only pruned by
`clear_to_mark`, `mark_cursor` and clearing). So `clear_to_previous_mark` can hand
`clear_to_mark` a mark that is in neither the active buffer nor the history;
`clear_to_mark` then drops the *whole* history while `m_history_start` stays put, and
the **next** newline — plain output in a shell — is the crash. That is the shape of a
crash a user hits while working: use the terminal until the scrollback has wrapped,
clear to a mark, and the next line of output traps.

Those two paths were also reading a wrapped `m_history` in the wrong order, so the
mark search and the resize rewrap were already wrong. The crash is the loud symptom.

## The fix

`normalize_history()` linearises the ring — rotating `m_history` so the oldest line is
first and `m_history_start` becomes 0 — and is called wherever the ring is about to be
used as a list: at the top of `clear_to_mark()`, and before the rewrap in `set_size()`.
`add_line_to_history()` also clamps a stale index, so the insert cannot go out of bounds
even if a third path is missed.

## The regression test

`test/VtHistoryTest.cpp` fills the ring, wraps it, clears to a mark whose line the ring
has overwritten, and then scrolls one more line. Before the fix that last scroll is the
same `VERIFY(index <= size())` → `SIGILL`; the driver treats a signal as the assertion,
like `run-eventloop-teardown-test.sh`.

The ring is filled deterministically rather than by guessing a line count: feed until
`history_size() == max_history_size()` (the scroll that fills it still appends, so the
index is still 0 there), then two more scrolls put it at 2. `clear_to_mark` empties the
history; the scroll that follows inserts at 1 into an empty vector. `set_cursor` puts the
cursor back on the bottom row first, since `clear_to_mark` leaves it at the top.
