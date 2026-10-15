# Terminal key handling: backspace and control characters

Status: both fixed and verified. Backspace was an encoding bug; Ctrl-C turned out
to be a *signal-disposition* bug: the byte was always right. Guarded by
`scripts/run-terminal-keys-test.sh` and `scripts/run-terminal-signals-test.sh`.

## Backspace: our convention was wrong for the host

**What the host expects.** A shell running under our Terminal has
`erase = ^?` in its termios (measured directly from the pty of a live shell):

```
$ stty -a -F /dev/pts/0
intr = ^C; quit = ^\; erase = ^?; kill = ^U; eof = ^D; ...
```

So a program reading *bytes* (a raw-mode TUI like `yay`, `htop`, `vim`) expects
**0x7f (DEL)** for backspace.

**What we send.** LibVT's key emitter falls through for a plain Backspace and emits
the key's code point, and xkb's `BackSpace` keysym is UTF-32 **0x08** (BS). So the
terminal sent 0x08 where the host wanted 0x7f, and every raw-mode program ignored
it: "backspace does not work" in `yay`, exactly.

**Why it was easy to miss:** 0x08 is correct inside SerenityOS (its own line editor
accepts `'\b'`), so nothing in-tree complained. It only breaks against host
programs, which is what this Terminal mostly runs.

**Fix:** `patches/0056-libvt-backspace-sends-del.patch`. A plain Backspace emits
0x7f. Safe in-tree: `LibLine/Editor.cpp` registers both `0x7f` and `'\b'`
(`register_key_input_callback((char)127, ... erase_character_backwards)`), so
Serenity's own shell and line editor are unaffected. Ctrl+Backspace is untouched.

## Ctrl-C: the byte was right; the *signal* was ignored

SerenityOS's WindowServer converted Ctrl+letter into the control character itself,
so a client sees Ctrl-C as code point **3**, not `'c'` plus a ctrl flag, and
LibVT relies on that (its `code_point &= 0x1f` only runs when it is handed the
*letter*). On Wayland the keysym is the plain letter, and whether xkbcommon applies
the control transformation is a property of the keymap rather than something to
assume.

`src/WaylandClient.cpp` now does the conversion where the Serenity keyboard model
is presented, leaving already-control characters alone so it cannot double-convert.
That is a consistency fix: it makes our behaviour match the WindowServer's
regardless of the keymap.

**The byte is right (measured).** A byte recorder (`cat`, raw) as the Terminal's
child received, for `a`, Ctrl-C, Backspace:

```
61 03 7f      a   003   177
```

Alt-a emits `1b 61` (ESC,a; LibVT prefixes on the *flag* alone) and Ctrl-/ emits
`1f`, so the shim's derived modifier flags reach the client too. The encoding is
correct, and `scripts/run-terminal-keys-test.sh` asserts it.

**But that is not what Ctrl-C does in a shell.** In a *cooked* shell the line
discipline consumes `0x03` and sends **SIGINT** to the foreground process group; the
program never sees the byte. And the session starts its applications as background
jobs of one non-interactive shell:

```
sh -c "Taskbar & FileManager -d & <applets> & ..."
```

POSIX requires a non-interactive shell to set **SIGINT and SIGQUIT to `SIG_IGN`** in
each background job, and a signal that was ignored on entry cannot be un-ignored by a
shell, or by anything it runs. Measured in the live session:

```
Terminal  SigIgn = 0x6   (SIGINT|SIGQUIT)
Taskbar   SigIgn = 0x6
yay       SigIgn = 0x2   (SIGINT)
```

So Ctrl-C *was* delivered and `yay` was configured to ignore it. The `^C` the user
sees is the line discipline's echo, which appears whether or not the signal does
anything, which is exactly why this looked like a broken terminal.

**`od -An -c` could never have shown it:** in a cooked shell both `^C` and the erase
character are consumed by the line discipline before `od` runs, so no amount of
testing there distinguishes a working Ctrl-C from a broken one.

**Fix, in three parts.**

* `patches/0058-libcore-process-reset-signals.patch`: `Core::Process::spawn` resets
  SIGINT/SIGQUIT in the child. A launcher must hand an application default
  dispositions; this covers every application the desktop launches, through either
  LaunchServer.
* `spawn_startup_command` (the compositor) runs each `&`-separated command as the
  *foreground* job of its own shell, and resets the signals in the forked child. A
  single `sh -c "A & B"` cannot be repaired from outside: the shell applies the
  ignore after we fork, so resetting the shell does not help and neither does job
  control (`sh -mc` behaves the same).
* `patches/0059-terminal-reset-signals-for-shell.patch`: the Terminal resets
  SIGINT/SIGQUIT before exec'ing the shell. A terminal starts a fresh session, so the
  shell should not inherit the terminal's own dispositions. This is also what makes
  the fix effective in a *running* session: the LaunchServer is a long-lived service
  holding the old library, so a new Terminal is what picks it up.

Verified in a headless harness: a spawned application reports `SIG_DFL` even when the
process that spawned it ignores SIGINT (and the Terminal is deliberately started as a
background job there, so `scripts/run-terminal-signals-test.sh` fails without the
reset).

**A latent bug surfaced while probing:** the shim's Ctrl+letter conversion range was
`'@' .. '_'`, which stops below `'c'` (0x63), so it never fired for *lowercase*
Ctrl+letters. LibVT converted those itself, which is why it went unnoticed; the range
now matches LibVT's (`'@' .. <DEL`).

## Getting the fixes

These are framework changes (LibVT, the LibWM shim, LibCore, the Terminal), so they
take effect when the framework is rebuilt, and that rebuild is safe while the
session runs. Relinking *replaces* the library file (a new inode); a process that
already has the old one mapped keeps that mapping for its whole life and is never
rewritten underneath. So the step after a rebuild is to open a **new** Terminal: an
existing one still has the old libraries mapped and will behave exactly as before.
(An earlier version of this file claimed the session had to be down for the rebuild;
it does not, and `scripts/rebuild-framework.sh` does the rebuild and says what to do
next.)

For this particular fix, what picks it up depends on the process:

* the **Terminal** change needs only a new Terminal (the binary is a symlink into the
  build, and the reset happens in the Terminal itself), so Ctrl-C works in a running
  session without logging out;
* the **compositor** and **`Process::spawn`** changes need the *compositor* and the
  **LaunchServer** restarted, because the startup commands and later launches are
  spawned by those long-lived processes. Until then a new Terminal is already enough
  for the reported symptom.

Related, and worth noting because it may be the same machinery: `run-terminal-mouse-test.sh`
failed in the first CI run with "no SGR mouse event reached the app" (the virtual
input path, from the test side).

A separate failure turned up while verifying this, and has been fixed: the
`/bin/X` → session-app-dir mapping built its candidate paths from a dangling
`StringView` (`LexicalPath::basename()`/`dirname()` return views *into* the
`LexicalPath`, and several call sites stored one taken from a temporary). The path
was garbage, so `run-launch-exec-test.sh` fell through to the `TextEditor` handler
instead of running the executable, and `Process::resolve_executable_path` missed its
`SDE_SESSION_BIN_DIR`/`SDE_APP_BIN_DIR` lookups. See
`patches/0060-libcore-lexicalpath-basename-lifetime.patch`.
