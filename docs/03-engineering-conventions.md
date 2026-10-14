# Serenity Desktop Environment — Engineering Conventions

**Status:** Living document · **Date:** 2026-10-09
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md)

These are binding conventions for all Serenity Desktop Environment (SDE) code in this
repository. They exist so the SDE reads as SerenityOS userland that happens to run on Linux,
and so it stays upstream-friendly.

## D1 — Build on the SerenityOS libraries

Every SDE component links the SerenityOS userland libraries built by **`libgui-wayland`**
(AK, LibCore, LibGfx, LibGUI, LibVT, LibGPU, …). We obtain them from that checkout/build and
do **not** vendor, fork, or duplicate them.

- Use AK for types and algorithms (`ByteString`, `String`, `Vector`, `HashMap`, `Optional`,
  `NonnullOwnPtr`/`RefPtr`, `ErrorOr`, …).
- Do **not** use the C++ standard library where AK already provides the facility.
- LibGPU is backed by **Mesa/EGL** (`libeglgpu`, with a LibSoftGPU fallback); use it for
  hardware-accelerated drawing.
- **D-Bus uses the `dbus-1` reference library behind an AK value layer.** SDE services that
  speak D-Bus (the portals, the Wi-Fi applet) use AK types over `dbus-1` — never GLib/GIO
  (`GVariant`, `GDBus`, `GMainLoop`). The shared value/service layer for the portals is
  `tools/portal/dbus.h` (`Portal::Value`, `Portal::MethodCall`, `Portal::Service`).

## D2 — SerenityOS-style `main` (always use LibCore/LibMain)

Every SDE executable **must** be a SerenityOS-style program: link **LibCore** and **LibMain**,
and define

```cpp
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    ...
}
```

not a raw `int main`. This is deliberate and non-negotiable, so that:

- we get LibCore's runtime (event loop, files, sockets, resource loading) and LibMain's
  uniform error reporting and exit codes;
- command-line parsing uses `Core::ArgsParser` (not `getopt`/`getopt_long`);
- fallible code returns `ErrorOr`/`Optional` and uses `TRY`/`MUST`;
- our entry points look and behave exactly like SerenityOS userland programs.

`liblagom-main.a` provides the real `main` that calls `serenity_main`.

## D3 — SerenityOS code style

All SDE code follows upstream **`Documentation/CodingStyle.md`**
(<https://github.com/SerenityOS/serenity/blob/master/Documentation/CodingStyle.md>) and the
project `.clang-format` (use clang-format 20 or later). In particular:

- Names: `CamelCase` for types, `snake_case` for variables/functions, `SCREAMING_CASE` for
  constants; `m_`/`s_`/`g_` prefixes for members/statics/globals.
- **East const** (`Foo const&`, not `const Foo&`); pointers and references written with no
  space before `*`/`&`.
- AK types, `ErrorOr`/`TRY`, `dbgln` (AK) for logging; `#pragma once`; no C-style casts.
- Prefer `class` over `struct` for types with methods; for structs, all members public and no
  `m_` prefix.
- No `std::` where AK suffices; no third-party abstraction where AK provides the facility.

## D4 — Portable libraries stay portable; host differences live in the SDE

Serenity-vs-Linux differences (process/`/proc` semantics, utmp, session/`/bin` layout, …) live
in the SDE (`src/LibSerenitySystem`, shims, services) or in the `libgui-wayland` host-compat /
build layer — **never** as non-portable code inside LibGUI/LibGfx/LibCore that would break
SerenityOS. SerenityOS source changes are kept minimal, are SDE-owned patches, and are written
to be upstreamable.

## D5 — wlroots is a C dependency behind a C++ platform layer

`WindowServer` uses **wlroots** for the display/input plumbing. wlroots is a C library,
so:

- wrap wlroots includes in `extern "C" { … }` (its headers lack the guards);
- keep the small, inert header normalization (C99 `[static N]` array parameters, the
  `namespace` member) in `scripts/fetch-wlroots.sh`;
- keep compositor policy in Serenity-style C++/AK, and encapsulate the C API so the rest of
  the compositor does not see raw wlroots types more than necessary;
- draw Serenity chrome with LibGfx and present it through `wlr_buffer`s.

Louvre (a C++ compositor library) was evaluated and rejected in favour of wlroots
(licensing/maturity/protocol coverage); see [`02-compositor-decision.md`](02-compositor-decision.md) §15.

## D6 — SerenityOS idioms and patterns

In addition to the style guide, SDE code follows upstream **`Documentation/Patterns.md`**
(<https://github.com/SerenityOS/serenity/blob/master/Documentation/Patterns.md>). The patterns
we commit to:

- **`TRY(...)` for error propagation.** Use `MUST(...)` only where failure is genuinely
  impossible or the program must crash. Where error propagation is not possible *yet*, use
  `release_value_but_fixme_should_propagate_errors()` — never `MUST(...)` — so the gap is
  greppable and FIXME-able.
- **Fallible constructors.** A class whose construction can fail defines a static fallible
  factory, usually `create(...)`, returning `ErrorOr<T>`/`ErrorOr<NonnullOwnPtr<T>>`; the real
  constructors are private and non-fallible.
- **`serenity_main(Main::Arguments)`** entry points linked with `LibMain` (see D2).
- **Singletons via `the()`** (e.g. `Gfx::FontDatabase::the()`), per the style guide.
- **String-view literals** `"..."sv` (no runtime length computation).
- **Contiguous collections**: `Vector` for dynamic lists, `Array` for compile-time inline
  storage, `FixedArray` for runtime-fixed size, `Span` for non-owning views. C-style arrays are
  discouraged outside these types' implementations.
- **Intrusive lists** (`IntrusiveList<&T::m_node>`, list type exposed via a public alias) where
  OOM-free insertion matters.
- **`AK::AssertSize<T, N>()`** inside `static_assert` instead of comparing `sizeof`, so
  mismatches print the real sizes.
- **`AK::SourceLocation`** as a defaulted argument (`= SourceLocation::current()`) for debug
  instrumentation, instead of preprocessor macros.
- **WindowServer IPC methods are append-only.** `WindowServer.ipc`'s `MessageID` is
  *positional* (an enum generated from the interface order), so a new method — e.g. SDE's
  `set_output_scale` (patch `0042`) — must be added at the **end** of the interface.
  Inserting one mid-file shifts every later id and breaks already-built apps ("Failed to
  parse IPC message") until the whole userland is rebuilt. `scripts/run-apps-smoke-test.sh`
  guards this.

## D7 — Verify UI by screenshot (the gold standard), not by bespoke instrumentation

To verify rendering and UI behaviour, **take a screenshot of the composited output**
(from a nested or native session) and look at it; for scripted checks, compare
images. This is the gold standard and the default way we test the UI.

- When something looks wrong, screenshot it and **fix the rendering** — do not grow
  a pile of debug output in its place.
- Prefer image comparisons over log scraping wherever the behaviour is visual
  (e.g. `scripts/run-hidpi-fidelity-test.sh` renders an app at 1× and 2× and asserts
  the 2× is the 1× pixel-doubled).
- Do **not** add ad-hoc, test-only hooks (`WINDOWSERVER_DUMP_*` / `SDE_*` one-offs,
  per-run debug branches) to the compositor mid-debug. If a capture facility is
  genuinely needed for headless CI, it is **one** small, general, permanent way to
  capture an output — not a bespoke env var invented to chase one bug.
- Make the shot reproducible with scripted input
  (`tools/virtual-input-test` can snap/move/focus) rather than manual steps.

## D8 — Scripts clean up after themselves

Every script that starts background processes, allocates a temp dir, or binds a
resource **must** undo all of it in an `EXIT` trap, so the cleanup also runs when
the script fails part-way:

- `kill` every background process it started (compositor, services, clients);
- `rm -rf` its temp dir;
- **unlink any Wayland/compositor socket it created.** Wayland does not remove a
  compositor's socket on shutdown, and a compositor that is `kill`ed (SIGTERM)
  never runs its own cleanup, so a socket left in `$XDG_RUNTIME_DIR` survives the
  run forever. A script that starts `WindowServer -n "$SOCK"` must include, in its
  trap, something like
  `rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$${,.lock}` (the run's own
  `$$`-suffixed sockets and their lock files).

Leaked sockets accumulate silently — dozens of dead `sde-*` sockets from the test
harness were once found in `$XDG_RUNTIME_DIR`. Sockets inside the script's own
temp dir (when the script points `XDG_RUNTIME_DIR` at it) are covered by
`rm -rf "$RUNTIME"`; when the compositor uses the real runtime dir, remove them
explicitly.

