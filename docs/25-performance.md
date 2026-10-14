# Performance & latency budget (R6, N3)

**Status:** landed — metered and enforced by `scripts/run-perf-test.sh`, which now
holds the compositor to a **240 Hz** frame interval (4.167 ms).
**Related:** [`01-objectives.md`](01-objectives.md) N3 ·
[`03-engineering-conventions.md`](03-engineering-conventions.md) D7 ·
[`08-compositor-gaps.md`](08-compositor-gaps.md) §5–7

N3 asks for **low, predictable input latency; smooth 60 Hz; and a software
compositor fallback**. This is the budget for that, how the compositor measures
itself, and the test that keeps it true. The budget is expressed in **Hz** and
derived from the output being rendered on, so a high-refresh panel is held to its
own cadence rather than a hardcoded 60 Hz.

## 1. The budget

The frame budget is **one refresh interval of the output we are rendering on**:

| Output | Frame budget (one interval) |
|---|---|
| 60 Hz | 16.667 ms |
| 120 Hz | 8.333 ms |
| 144 Hz | 6.944 ms |
| **240 Hz** | **4.167 ms** |

`wlr_output.refresh` supplies it (and `SDE_PERF_BUDGET_HZ` can override the
reference rate, for a harness or to hold a session to a display it is not on).

| Metric | Budget | What it covers |
|---|---|---|
| **Frame time** p99 | **< 1 refresh interval** — 4.167 ms at 240 Hz | The compositor's own work for one frame: scene build, render, commit (`output_frame()`). A client's own drawing happens before its commit. |
| **Frame time** max | < 1 refresh interval | No frame may overrun it, or a client's commit slips to the next vblank (a visible hitch). |
| **Frame interval** | ≈ the refresh interval under sustained damage | The compositor must actually keep the cadence, not merely be fast enough per frame. |
| **Input → present** p95 | < 20 ms | From the compositor handling an input event to the frame that presents it (`notify_activity()` → the next `output_frame()`). Damage renders immediately on a headless output (see §2), so this is the compositor + client share; on real hardware add up to one refresh interval for the vblank. |
| **Idle frames** | **≤ 3 per second** | A compositor must not render when nothing changed — rendering on damage is what makes high refresh *affordable* rather than a busy loop. |

The budget is held on the **software (pixman) renderer** — the fallback N3 asks
for is the case we hold to it. GLES renders less on the CPU (bound by the GPU
instead) and is metered by the same code.

## 2. Instrumentation

The compositor meters itself **always** (two `clock_gettime` calls per frame —
nothing measurable) and reports on demand:

| Variable | Effect |
|---|---|
| `SDE_PERF_STATS=<path>` | Rewrite a `key=value` summary of the meter to that file, once a second. |
| `SDE_PERF_LOG=1` | Also log a one-line summary each second, so a session can be watched live. |
| `SDE_PERF_BUDGET_HZ=<hz>` | Override the reference rate the frame budget is derived from. |
| `SDE_OUTPUT_REFRESH=<hz>` | Run the output at that refresh (a *virtual* output's modes carry none; the counterpart of `SDE_OUTPUT_SCALE`). |

All are off by default, so a normal session pays nothing, and none changes the
code path being measured (there is no "profiling build").

Each metric keeps a ring of the last 600 samples (~10 s at 60 Hz — fixed memory,
so a long session cannot grow) plus cumulative totals. The summary carries
`output_refresh_hz`, the derived `frame_ms_budget`, p50/p95/p99/max and the
over-budget count for frame time, the frame interval at p50/p95/p99, the
input → present percentiles with a sample count, and **resident memory**
(`rss_kb`, the high-water mark, and the change since the last report) — see §4.

### How the frames are paced

The meter's numbers are only meaningful with the pacing model in mind:

- **Damage renders immediately.** wlroots' scene requests a frame when a buffer
  is committed with damage (a 0-delay frame timer on the headless backend), so the
  first damage after an idle period is presented with sub-millisecond latency —
  which is why `input → present` is well under a millisecond here.
- **The output's refresh then paces sustained frames.** While damage keeps
  arriving, frames settle at one per refresh interval (measured below: 4.06 ms at
  a 240 Hz output, 16.1 ms at 60 Hz — for the *same* 4 ms damage stream).
- **A headless output has no vblank**, so the interval equals the damage cadence
  clipped by the refresh: it measures the compositor's pacing, not the display's.
- **Input → present takes the most recent input**, because that is what the frame
  ends up showing.

## 3. Reference numbers

Measured on the development machine (headless, 1280×720, `SDE_OUTPUT_SCALE=1`,
pixman unless stated); times in milliseconds, from `scripts/run-perf-test.sh`:

| Phase | frame p50 | p95 | p99 | max | over budget | interval p50 |
|---|---|---|---|---|---|---|
| Idle (2 s) | — | — | — | — | — | 0 frames rendered |
| Pointer motion (60 Hz output) | 0.03 | 0.06 | 0.11 | 0.25 | 0 | 16.1 (60 Hz) |
| Drag a decorated window (60 Hz output) | 0.04 | 0.60 | 0.80 | 2.07 | 0 | 16.1 |
| Drag a decorated window (GLES forced) | 0.03 | 0.73 | 1.02 | 4.80 | 1 | 16.1 |
| **240 Hz output, motion at 4 ms** | **0.024** | **0.048** | **0.067** | **0.124** | **0** | **4.06** |
| **240 Hz output, drag at 4 ms** | **0.52** | **0.72** | **0.94** | **2.70** | **0** | **4.59** |
| 240 Hz output, drag at 4 ms (GLES/llvmpipe) | 0.52 | 0.73 | 1.02 | 5.20 | **1** | 4.59 |

Read across the 240 Hz rows: the compositor **holds a ~4.1–4.6 ms cadence under a
4 ms damage stream**, and the work per frame is 3–60× inside the 4.167 ms
interval at p99. The heaviest path we own — dragging a decorated window, where
every motion step moves the window and re-renders its frame — costs ~0.5 ms at
p50 on both renderers.

**The one caveat**, and the next target: a single frame of the drag on the
software-GL path reached 5.2 ms (1 frame in ~244), which would slip one 240 Hz
interval. It is a one-off at the start of the drag (buffer import / first frame
after the grab); the p99 is 1.02 ms. Until it is fixed, a 240 Hz panel is met
sustained and at p99, but not unconditionally on the GLES fallback.

## 4. Memory

The same meter reports resident memory (see §2), and the SDE already carries the
tools to go deeper:

| Tool | What it gives |
|---|---|
| `SDE_PERF_STATS` — `rss_kb` / `rss_high_kb` / `rss_delta_kb` | A live RSS trace of the compositor, with the high-water mark and per-second growth: the cheap shape of a leak. Phase (E) of the test enforces it. |
| **Profiler** (`SerenitySystem::Profiling`, `scripts/run-profiler-test.sh`) | `perf_event_open` sampling into the perfcore JSON the SerenityOS Profiler app reads — CPU samples with the process's **mappings** (`mmap`/`mmap2` records plus a mapping snapshot), so `/proc` and library contributions are visible alongside time. |
| **SystemMonitor** | Process list and CPU/memory graphs, and a **memory-map window per process** (reads `/proc/<pid>/smaps`) — the same view a user has. |

### Reference numbers (headless, 1280×720)

| Configuration | Idle RSS | After 50 window open/close cycles |
|---|---|---|
| pixman (the fallback) | **35.9 MB** | 35.9 MB (no growth) |
| `SDE_RENDERER=gles2` (forced GL, i.e. llvmpipe here) | 119.7 MB | — |

### Per-client (session) memory

The meter also reports every Wayland client: `clients`, `client_pss_kb`,
`session_pss_kb` (compositor + clients) and a `client_memory=` breakdown of the
eight largest by PSS, labelled by the process name (the shell's clients are
panels and applets — layer surfaces with no toplevel to name them by). That makes
the compositor's own stats file the session's "who is using memory" report, and
`scripts/run-session-memory-test.sh` asserts it on the session's real startup
list (Taskbar + Desktop + applets + BatteryApplet).

Measured headless (1280×720), PSS in kB:

| Client | PSS | RSS |
|---|---|---|
| WindowServer | 18 096 | 43 964 |
| **Audio** applet | **11 043** | 32 184 |
| FileManager (Desktop) | 8 159 | 34 124 |
| Taskbar | 4 040 | 24 028 |
| ClipboardHistory | 2 291 | 21 876 |
| ResourceGraph | 2 141 | 21 248 |
| Network | 2 131 | 21 496 |
| BatteryApplet | 2 093 | 20 868 |
| WorkspacePicker | 2 088 | 21 004 |
| **session total** | **54 154** | **262 140** |

Read the two columns together: **RSS says the session uses 256 MB; PSS says
53 MB.** Ten clients each "use" the whole shared Lagom stack, so RSS multiplies
it. Every ordinary applet costs ~2 MB — the 21 MB RSS figure is shared library
pages counted ten times.

### A known item: the Audio applet pays ~9 MB for an audio backend it never uses

The Audio applet is the one outlier (11.0 MB PSS against ~2.1 MB for the others).
It links `liblagom-audio` — for `Audio::ConnectionToManagerServer`, the
AudioServer client protocol — and that library has
`target_link_libraries(LibAudio PRIVATE pulse)`, so it **DT_NEEDs libpulse**
(+ libpulsecommon + libsndfile). Those are mapped only by this process, so their
resident pages are private to it, and the dynamic linker's relocation/constructor
work at startup makes them resident: ~9 MB for a dependency the applet never
calls (it talks to the SDE AudioServer over IPC).

Two fixes, both in `libgui-wayland` (the framework repo) and *not* done yet:

1. **Split the backend out**: move `PlaybackStreamPulseAudio.cpp` +
   `PulseAudioWrappers.{h,cpp}` into their own Lagom library, so
   `liblagom-audio` no longer DT_NEEDs libpulse. `PlaybackStream::create()` has to
   move with it (it names the platform implementation), and LibWeb's
   `AudioCodecPluginAgnostic` — the only in-tree user of a direct stream — links
   both. The cleanest split, but it changes an upstream file layout.
2. **Load libpulse lazily**: the 499-line `PulseAudioWrappers` already wrap the
   API, so resolving its functions through `dlopen` removes the DT_NEEDED and
   keeps every caller unchanged. More code churn, no API/structure change.

Until one lands, the client PSS budget (16 MB) records the outlier rather than
hiding it.

### The first fruit: `auto` no longer loads software GL for nothing

`auto` used to hand the headless backend to wlroots' GL path, which on a machine
with no DRM device can only be **software GL (llvmpipe)**: **119.5 MB** idle,
against 35.6 MB for pixman, for a *frame time that is the same or slightly
worse* (p95 0.73 ms vs 0.60 ms while dragging). Almost all of that is Mesa's
gallium/LLVM footprint ([heap] alone was 13.3 MB, plus libgallium and libLLVM)
that pixman never touches.

`auto` now checks `wlr_backend_get_drm_fd()`: **with no DRM device it takes
pixman directly**, so the GL stack is never initialised — **84 MB, ~70 %, saved**
on a headless session with no behaviour change. An explicit `WLR_RENDERER` in the
environment still wins (the fallback path stays exercised), and
`SDE_RENDERER=gles2` still forces the GL path for a job that wants to test it.

### The guard

Phase (E) of `scripts/run-perf-test.sh` asserts the idle footprint is inside
`SDE_PERF_RSS_LIMIT_KB` (default 80 MB) and that **50 window open/close cycles do
not grow it** beyond `SDE_PERF_RSS_GROWTH_KB` (default 4 MB). Measured growth: 0.

## 5. What keeps it inside the budget

- **Render on damage only.** `wlr_scene_output_commit()` is driven by damage; the
  compositor has no repaint tick (the animation timers only run while an animation
  is in flight, and the 1 s heartbeat writes a file, not pixels). Phase (A) guards
  exactly this.
- **Damage granularity.** Frames render from the wlroots scene's damage and its
  buffer-age reuse, so a pointer move or a titlebar hover costs the changed
  region, not the output.
- **Compositor chrome is cached.** Window frames, the cursor highlight, the
  switcher's overlay and the stack-switch overlay are `ChromeNode`s: a scene
  buffer re-rendered only when it changes, not per frame.
- **A per-frame early-out for the desktop background.** `update_desktop_background()`
  rebuilds the background bitmap only when the output size or the wallpaper
  changes.
- **Bounded per-frame work.** Timers are short-lived (animation timers stop when
  the animation ends), the key-repeat timer is a single `Core::Timer`, and the
  session's watchdog heartbeat is a 1 s file write.

## 6. Running it

```sh
scripts/run-perf-test.sh                     # the software fallback (pixman)
SDE_PERF_RENDERER=auto scripts/run-perf-test.sh
```

Five phases: **(A)** idle pacing — nothing changes, so nothing may render;
**(B)** 60 Hz pointer motion — frame time and input → present inside the budget,
with the p99 held to the 240 Hz interval; **(C)** dragging a decorated window,
the heaviest path we own; **(D)** a **240 Hz output** (the compositor is
restarted with `SDE_OUTPUT_REFRESH=240`) with a 4 ms damage stream, where the
cadence itself is asserted (~4.17 ms) along with the work fitting inside it;
**(E)** memory — the idle footprint and that 50 window cycles do not grow it.

The knobs (`SDE_PERF_FRAME_BUDGET_MS`, `SDE_PERF_LATENCY_BUDGET_MS`,
`SDE_PERF_IDLE_FRAME_LIMIT`, `SDE_PERF_FAST_MS`, `SDE_PERF_AGGRESSIVE_MS`,
`SDE_PERF_RENDERER`) let a slower machine or a stricter CI gate be expressed
without editing the script.

The session's memory budget is enforced separately, on the session's real startup
list (Taskbar + Desktop + applets + BatteryApplet):

```sh
scripts/run-session-memory-test.sh
```

It asserts the client count, the compositor's PSS, the session's PSS, each
client's PSS, and that an idle session does not grow
(`SDE_SESSION_PSS_LIMIT_KB`, `SDE_COMPOSITOR_PSS_LIMIT_KB`,
`SDE_CLIENT_PSS_LIMIT_KB`, `SDE_SESSION_PSS_GROWTH_KB`).

To watch a live session:

```sh
SDE_PERF_STATS=/tmp/sde-perf SDE_PERF_LOG=1 loginctl terminate-session "$XDG_SESSION_ID"
watch -n1 cat /tmp/sde-perf
```

## 7. Not covered yet

- **A Monitor-tab refresh control.** `SDE_OUTPUT_REFRESH` exists (and a saved
  layout keeps the output's current refresh); choosing a refresh rate from
  Display Settings is the natural follow-up.
- **Input-to-photon on real hardware.** The headless number excludes the vblank;
  a DRM measure would need a high-speed capture or a hardware timestamp path.
- **The drag tail above** — one ~5 ms frame on the GLES path, which is what
  stands between the current state and an unconditional 240 Hz claim on that
  renderer.
- **A per-hardware/GPU matrix.** The budget is enforced on one headless
  configuration plus the renderer knob; R6's CI matrix would run it per renderer,
  per output scale and per refresh.
- **Device memory.** The RSS meter covers the host heap; GPU/VRAM and dma-buf
  usage on the GL path (and a per-output memory budget) would need the driver's
  own counters.
- **Client-side render cost.** For Serenity apps the window contents are drawn on
  the CPU by LibGUI/LibGfx; the frame budget measures the *compositor's* share, so
  a slow client shows up as a low frame rate rather than as frame time. Client
  *memory* is metered (above), but a client's own CPU time is not.
