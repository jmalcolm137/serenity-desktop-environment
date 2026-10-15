# Terminal key handling: backspace and control characters

Status: backspace diagnosed and fixed; Ctrl-C fix landed but its *symptom* is
unmeasured (see "What I could not establish").

## Backspace: our convention was wrong for the host

**What the host expects.** A shell running under our Terminal has
`erase = ^?` in its termios — measured directly from the pty of a live shell:

```
$ stty -a -F /dev/pts/0
intr = ^C; quit = ^\; erase = ^?; kill = ^U; eof = ^D; ...
```

So a program reading *bytes* (a raw-mode TUI like `yay`, `htop`, `vim`) expects
**0x7f (DEL)** for backspace.

**What we send.** LibVT's key emitter falls through for a plain Backspace and emits
the key's code point, and xkb's `BackSpace` keysym is UTF-32 **0x08** (BS). So the
terminal sent 0x08 where the host wanted 0x7f, and every raw-mode program ignored
it — "backspace does not work" in `yay`, exactly.

**Why it was easy to miss:** 0x08 is correct inside SerenityOS (its own line editor
accepts `'\b'`), so nothing in-tree complained. It only breaks against host
programs, which is what this Terminal mostly runs.

**Fix:** `patches/0056-libvt-backspace-sends-del.patch` — a plain Backspace emits
0x7f. Safe in-tree: `LibLine/Editor.cpp` registers both `0x7f` and `'\b'`
(`register_key_input_callback((char)127, ... erase_character_backwards)`), so
Serenity's own shell and line editor are unaffected. Ctrl+Backspace is untouched.

## Ctrl-C: made explicit, symptom unmeasured

SerenityOS's WindowServer converted Ctrl+letter into the control character itself,
so a client sees Ctrl-C as code point **3**, not `'c'` plus a ctrl flag — and
LibVT relies on that (its `code_point &= 0x1f` only runs when it is handed the
*letter*). On Wayland the keysym is the plain letter, and whether xkbcommon applies
the control transformation is a property of the keymap rather than something to
assume.

`src/WaylandClient.cpp` now does the conversion where the Serenity keyboard model
is presented, leaving already-control characters alone so it cannot double-convert.
That is a consistency fix: it makes our behaviour match the WindowServer's
regardless of the keymap.

**What I could not establish, plainly:** I did not measure the Ctrl-C symptom. My
probe (a headless session running the Terminal with a byte recorder as its child,
driven by the virtual-input tool) failed twice for its own reasons — the recorder
exited before reading anything, and the input tool never registered its devices —
so it produced no evidence. The code path also *looks* correct for Ctrl-C, which is
why I am not claiming the shim change explains the symptom. It may, or the cause may
be elsewhere.

**A ten-second measurement would settle it,** in your Terminal:

```sh
od -An -c
```

then press **Backspace**, then **Ctrl-C**, then Ctrl-D. Expected on a normal
terminal: `177` (backspace), `003` (Ctrl-C). If you see `010` the backspace
diagnosis is confirmed; what you see for Ctrl-C tells us whether this fix is the
right one.

## Getting the fixes

Both are framework changes (LibVT, and the LibWM shim), so they take effect when the
framework is rebuilt — **and that rebuild must not happen while your session is
running**: it relinks `libvt`/`libwindowserver`, which every running client has
mapped, and rewriting a mapped library under a live process is how you get SIGBUS.
CI's image gets them on the next dispatch; your machine needs a `build-libgui.sh`
with the session stopped (from a VT, or before logging in). I can do that for you if
you would rather not, but it needs the session down first.

Related, and worth noting because it may be the same machinery: `run-terminal-mouse-test.sh`
failed in the first CI run with "no SGR mouse event reached the app" — the virtual
input path, from the test side.
