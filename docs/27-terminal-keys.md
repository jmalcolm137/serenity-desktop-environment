# Terminal key handling: backspace and control characters

Status: both diagnosed, fixed, and now measured in a headless harness
(`scripts/run-terminal-keys-test.sh`).

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

## Ctrl-C: made explicit, and measured

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

**Measured, at last.** The same headless harness the mouse test uses now works, and
it says Ctrl-C is correct. A byte recorder (`cat`, raw) as the Terminal's child
received, for `a`, Ctrl-C, Backspace:

```
61 03 7f      a   003   177
```

so the byte is right. In *cooked* mode the byte never reaches the child, because
the line discipline consumes it — and that is also correct: a child that installs a
SIGINT handler (resetting the `SIG_IGN` it inherited from being started as a
background job) reports `GOT signal 2`. So Ctrl-C produces SIGINT end to end.

The other modifier flags were probed the same way, because the shim derives them
from key events rather than from `wl_keyboard.modifiers`, and that derivation was
the one thing the byte test could not see: Alt+a emits `1b 61` (ESC,a — LibVT
prefixes on the *flag* alone) and Ctrl+/ emits `1f`. The derived Alt/Ctrl flags
work.

**So the Ctrl-C symptom is not reproducible here.** What `od -An -c` shows in a
*normal* shell is nothing at all for Ctrl-C (and nothing for Backspace either):
both are line-discipline characters (`intr = ^C`, `erase = ^?`), so the shell
consumes them before `od` runs. `003` is only visible in a raw tty:

```sh
stty raw -echo; od -An -c -N3; stty sane   # press Backspace, Ctrl-C, then a
```

expects `177 003 141`. `scripts/run-terminal-keys-test.sh` asserts all of this.

While probing, one real (if masked) bug surfaced: the conversion range here was
`'@' .. '_'`, which stops below `'c'` (0x63), so it never fired for *lowercase*
Ctrl+letters. LibVT converted those itself, which is why it went unnoticed; the
range now matches LibVT's (`'@' .. <DEL`).

## Getting the fixes

Both are framework changes (LibVT, and the LibWM shim), so they take effect when the
framework is rebuilt — and that rebuild is safe while the session runs. Relinking
*replaces* the library file (a new inode); a process that already has the old one
mapped keeps that mapping for its whole life and is never rewritten underneath. So
the only step after a rebuild is to open a **new** Terminal: an existing one still
has the old libraries mapped and will behave exactly as before. (An earlier version
of this file claimed the session had to be down for the rebuild — it does not, and
`scripts/rebuild-framework.sh` does the rebuild and says what to do next.)

Related, and worth noting because it may be the same machinery: `run-terminal-mouse-test.sh`
failed in the first CI run with "no SGR mouse event reached the app" — the virtual
input path, from the test side.
