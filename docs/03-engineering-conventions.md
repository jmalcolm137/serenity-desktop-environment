# Serenity Desktop Environment: Engineering Conventions

**Status:** Living document · **Date:** 2026-10-09
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md)

These are binding conventions for all Serenity Desktop Environment (SDE) code in this
repository. They exist so the SDE reads as SerenityOS userland that happens to run on Linux,
and so it stays upstream-friendly.

## D1: Build on the SerenityOS libraries

Every SDE component links the SerenityOS userland libraries built by **`libgui-wayland`**
(AK, LibCore, LibGfx, LibGUI, LibVT, LibGPU, …). We obtain them from that checkout/build and
do **not** vendor, fork, or duplicate them.

- Use AK for types and algorithms (`ByteString`, `String`, `Vector`, `HashMap`, `Optional`,
  `NonnullOwnPtr`/`RefPtr`, `ErrorOr`, …).
- Do **not** use the C++ standard library where AK already provides the facility.
- LibGPU is backed by **Mesa/EGL** (`libeglgpu`, with a LibSoftGPU fallback); use it for
  hardware-accelerated drawing.
- **D-Bus uses the `dbus-1` reference library behind an AK value layer.** SDE services that
  speak D-Bus (the portals, the Wi-Fi applet) use AK types over `dbus-1`, never GLib/GIO
  (`GVariant`, `GDBus`, `GMainLoop`). The shared value/service layer for the portals is
  `Tools/portal/dbus.h` (`Portal::Value`, `Portal::MethodCall`, `Portal::Service`).

## D1a: One build tree, one entry point

The repository has a single top-level `CMakeLists.txt`, and `scripts/build-full-sde.sh`
(or `cmake -S . -B build && cmake --build build`) is the one command that builds
everything. The components' own `CMakeLists.txt` still stand alone for the rare case
of building one piece, but nothing needs them to: they are `add_subdirectory()`d here,
so there is one dependency graph, one `compile_commands.json`, and any target is one
`cmake --build build --target <name>` away.

- **Do not add a `build-*.sh` script that runs its own `cmake -S <dir> -B <dir>`.** Add
  the component to the top-level file (or, for a group, a custom target like
  `sde-tools`) and make the script a wrapper around `scripts/build-target.sh`. Twenty
  independent build directories held together by environment variables are how a
  component ended up depending on a generated header that only happened to exist
  (README, "Building from scratch").
- **The framework is not a subdirectory.** `libgui-wayland`'s Lagom build has its own
  entry point and its own state (its patch set, its configuration), so the SDE drives
  it as the `lagom` target instead of re-configuring it (see `Meta/CMake/SDELagom.cmake`).
  The files the SDE links are that target's byproducts, so a change to LibGUI is a
  dependency of the SDE targets that use it, not something to rebuild by hand.

## D1b: Repository layout

The tree is grouped the way SerenityOS groups its own, so a reader who knows one
knows the other:

| Directory | What goes there |
|---|---|
| `Services/` | Session services, each in its own directory: the compositor (`Services/WindowServer/`) and the services SDE builds or provides (`AudioServer`, `ImageDecoder`, `LaunchServer`, `NotificationServer`, `RequestServer`, `SQLServer`, `WebContent`). |
| `Applets/` | Taskbar applets. SDE's own (`Applets/Battery/`, `Applets/WiFi/`) are full projects; `Applets/CMakeLists.txt` builds SerenityOS's applets from upstream, with SDE's Linux replacements for individual files in `Applets/<Name>/`. |
| `Dialogs/` | The LibGUI dialogs the xdg-desktop-portal backend launches (`Access`, `AppChooser`, `ColorPicker`, `FileChooser`, `Print`, `ScreenCastChooser`, `ScreenshotOverlay`). |
| `Libraries/` | Code linked into other components: `LibSerenitySystem`, and `Compat/`, host stand-ins for Serenity-only headers. |
| `Base/` | Files installed as data rather than built: the display-manager session entry (`Base/session/`) and the portal configuration (`Base/xdg-desktop-portal/`). |
| `Meta/` | Build and release machinery: `Meta/CMake/` (the CMake modules), `Meta/CI/` (the CI image), `Meta/packaging/` (the AUR packages). |
| `Tools/` | Session utilities, the portal backends, and the C test clients the scripts drive. |
| `Tests/` | The C++ test programs (`scripts/build-tests.sh`). |
| `scripts/` | Build, session and test entry points (`run-*-test.sh`), and `scripts/lib/`. |
| `patches/` | SDE's changes to the SerenityOS tree (`patches/README.md`). |
| `docs/` | Design documents and conventions, numbered. |

- **Pick the group by what a component is, not by who wrote it.** A service that
  wraps upstream sources and one written for SDE both live in `Services/`.
- **Build outputs do not follow the source.** The top-level `CMakeLists.txt` gives
  each component an explicit binary directory, so a component's output stays at
  `build/<Name>/` (`build/WindowServer/WindowServer`, `build/WifiApplet/WifiApplet`,
  `build/tools/`, `build/test/`) wherever its source moves. Scripts refer to those
  paths; keep them stable.
- **Paths into the repository start from `$SDE_ROOT`** (scripts) or
  `${CMAKE_SOURCE_DIR}` / `${SDE_ROOT}` (CMake), never from a relative `../`, so a
  component can move without its references breaking.

## D2: SerenityOS-style `main` (always use LibCore/LibMain)

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

## D3: SerenityOS code style

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

## D4: Portable libraries stay portable; host differences live in the SDE

Serenity-vs-Linux differences (process/`/proc` semantics, utmp, session/`/bin` layout, …) live
in the SDE (`src/LibSerenitySystem`, shims, services) or in the `libgui-wayland` host-compat /
build layer, **never** as non-portable code inside LibGUI/LibGfx/LibCore that would break
SerenityOS. SerenityOS source changes are kept minimal, are SDE-owned patches, and are written
to be upstreamable.

## D5: wlroots is a C dependency behind a C++ platform layer

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

## D6: SerenityOS idioms and patterns

In addition to the style guide, SDE code follows upstream **`Documentation/Patterns.md`**
(<https://github.com/SerenityOS/serenity/blob/master/Documentation/Patterns.md>). The patterns
we commit to:

- **`TRY(...)` for error propagation.** Use `MUST(...)` only where failure is genuinely
  impossible or the program must crash. Where error propagation is not possible *yet*, use
  `release_value_but_fixme_should_propagate_errors()`, never `MUST(...)`, so the gap is
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
  *positional* (an enum generated from the interface order), so a new method, e.g. SDE's
  `set_output_scale` (patch `0042`), must be added at the **end** of the interface.
  Inserting one mid-file shifts every later id and breaks already-built apps ("Failed to
  parse IPC message") until the whole userland is rebuilt. `scripts/run-apps-smoke-test.sh`
  guards this.

## D7: Verify UI by screenshot (the gold standard), not by bespoke instrumentation

To verify rendering and UI behaviour, **take a screenshot of the composited output**
(from a nested or native session) and look at it; for scripted checks, compare
images. This is the gold standard and the default way we test the UI.

- When something looks wrong, screenshot it and **fix the rendering**; do not grow
  a pile of debug output in its place.
- Prefer image comparisons over log scraping wherever the behaviour is visual
  (e.g. `scripts/run-hidpi-fidelity-test.sh` renders an app at 1× and 2× and asserts
  the 2× is the 1× pixel-doubled).
- Do **not** add ad-hoc, test-only hooks (`WINDOWSERVER_DUMP_*` / `SDE_*` one-offs,
  per-run debug branches) to the compositor mid-debug. If a capture facility is
  genuinely needed for headless CI, it is **one** small, general, permanent way to
  capture an output, not a bespoke env var invented to chase one bug.
- Make the shot reproducible with scripted input
  (`Tools/virtual-input-test` can snap/move/focus) rather than manual steps.

## D8: Scripts clean up after themselves

Every script that starts background processes, allocates a temp dir, or binds a
resource **must** undo all of it in an `EXIT` trap, so the cleanup also runs when
the script fails part-way:

- `kill` every background process it started (compositor, services, clients);
- `rm -rf` its temp dir;
- **unlink any Wayland/compositor socket it created.** The compositor removes its
  socket when it shuts down cleanly (SIGTERM, SIGINT or Alt+Escape run its
  teardown), but one that is killed (SIGKILL, a timeout's kill, a crash) never
  does, so a socket left in `$XDG_RUNTIME_DIR` survives the run forever. A script that starts `WindowServer -n "$SOCK"` must include, in its
  trap, something like
  `rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$${,.lock}` (the run's own
  `$$`-suffixed sockets and their lock files).

Leaked sockets accumulate silently: dozens of dead `sde-*` sockets from the test
harness were once found in `$XDG_RUNTIME_DIR`. Sockets inside the script's own
temp dir (when the script points `XDG_RUNTIME_DIR` at it) are covered by
`rm -rf "$RUNTIME"`; when the compositor uses the real runtime dir, remove them
explicitly.

