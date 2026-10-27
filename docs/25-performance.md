# Performance & latency budget (R6, N3)

**Status:** landed, metered and enforced by `scripts/run-perf-test.sh`, which now
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
| **Frame time** p99 | **< 1 refresh interval** (4.167 ms at 240 Hz) | The compositor's own work for one frame: scene build, render, commit (`output_frame()`). A client's own drawing happens before its commit. |
| **Frame time** max | < 1 refresh interval | No frame may overrun it, or a client's commit slips to the next vblank (a visible hitch). |
| **Frame interval** | ≈ the refresh interval under sustained damage | The compositor must actually keep the cadence, not merely be fast enough per frame. |
| **Input → present** p95 | < 20 ms | From the compositor handling an input event to the frame that presents it (`notify_activity()` → the next `output_frame()`). Damage renders immediately on a headless output (see §2), so this is the compositor + client share; on real hardware add up to one refresh interval for the vblank. |
| **Idle frames** | **≤ 3 per second** | A compositor must not render when nothing changed; rendering on damage is what makes high refresh *affordable* rather than a busy loop. |

The budget is held on the **software (pixman) renderer**: the fallback N3 asks
for is the case we hold to it. GLES renders less on the CPU (bound by the GPU
instead) and is metered by the same code.

## 2. Instrumentation

The compositor meters itself **always** (two `clock_gettime` calls per frame,
nothing measurable) and reports on demand:

| Variable | Effect |
|---|---|
| `SDE_PERF_STATS=<path>` | Rewrite a `key=value` summary of the meter to that file, once a second. |
| `SDE_PERF_LOG=1` | Also log a one-line summary each second, so a session can be watched live. |
| `SDE_PERF_BUDGET_HZ=<hz>` | Override the reference rate the frame budget is derived from. |
| `SDE_OUTPUT_REFRESH=<hz>` | Run the output at that refresh (a *virtual* output's modes carry none; the counterpart of `SDE_OUTPUT_SCALE`). |

All are off by default, so a normal session pays nothing, and none changes the
code path being measured (there is no "profiling build").

Each metric keeps a ring of the last 600 samples (~10 s at 60 Hz; fixed memory,
so a long session cannot grow) plus cumulative totals. The summary carries
`output_refresh_hz`, the derived `frame_ms_budget`, p50/p95/p99/max and the
over-budget count for frame time, the frame interval at p50/p95/p99, the
input → present percentiles with a sample count, `scanout_samples` and
`direct_scanouts` for client content, and **resident memory** (`rss_kb`, the
high-water mark, and the change since the last report); see §4.

### How the frames are paced

The meter's numbers are only meaningful with the pacing model in mind:

- **Damage renders immediately.** wlroots' scene requests a frame when a buffer
  is committed with damage (a 0-delay frame timer on the headless backend), so the
  first damage after an idle period is presented with sub-millisecond latency,
  which is why `input → present` is well under a millisecond here.
- **The output's refresh then paces sustained frames.** While damage keeps
  arriving, frames settle at one per refresh interval (measured below: 4.06 ms at
  a 240 Hz output, 16.1 ms at 60 Hz, for the *same* 4 ms damage stream).
- **A headless output has no vblank**, so the interval equals the damage cadence
  clipped by the refresh: it measures the compositor's pacing, not the display's.
- **Input → present takes the most recent input**, because that is what the frame
  ends up showing.

## 3. Reference numbers

Measured on the development machine (headless, 1280×720, `SDE_OUTPUT_SCALE=1`,
pixman unless stated); times in milliseconds, from `scripts/run-perf-test.sh`:

| Phase | frame p50 | p95 | p99 | max | over budget | interval p50 |
|---|---|---|---|---|---|---|
| Idle (2 s) | n/a | n/a | n/a | n/a | n/a | 0 frames rendered |
| Pointer motion (60 Hz output) | 0.03 | 0.06 | 0.11 | 0.25 | 0 | 16.1 (60 Hz) |
| Drag a decorated window (60 Hz output) | 0.04 | 0.60 | 0.80 | 2.07 | 0 | 16.1 |
| Drag a decorated window (GLES forced) | 0.03 | 0.73 | 1.02 | 4.80 | 1 | 16.1 |
| **240 Hz output, motion at 4 ms** | **0.024** | **0.048** | **0.067** | **0.124** | **0** | **4.06** |
| **240 Hz output, drag at 4 ms** | **0.52** | **0.72** | **0.94** | **2.70** | **0** | **4.59** |
| 240 Hz output, drag at 4 ms (GLES/llvmpipe) | 0.52 | 0.73 | 1.02 | 5.20 | **1** | 4.59 |

Read across the 240 Hz rows: the compositor **holds a ~4.1–4.6 ms cadence under a
4 ms damage stream**, and the work per frame is 3–60× inside the 4.167 ms
interval at p99. The heaviest path we own (dragging a decorated window, where
every motion step moves the window and re-renders its frame) costs ~0.5 ms at
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
| `SDE_PERF_STATS`: `rss_kb` / `rss_high_kb` / `rss_delta_kb` | A live RSS trace of the compositor, with the high-water mark and per-second growth: the cheap shape of a leak. Phase (E) of the test enforces it. |
| **Profiler** (`SerenitySystem::Profiling`, `scripts/run-profiler-test.sh`) | `perf_event_open` sampling into the perfcore JSON the SerenityOS Profiler app reads: CPU samples with the process's **mappings** (`mmap`/`mmap2` records plus a mapping snapshot), so `/proc` and library contributions are visible alongside time. |
| **SystemMonitor** | Process list and CPU/memory graphs, and a **memory-map window per process** (reads `/proc/<pid>/smaps`), the same view a user has. |

### Reference numbers (headless, 1280×720)

| Configuration | Idle RSS | After 50 window open/close cycles |
|---|---|---|
| pixman (the fallback) | **35.9 MB** | 35.9 MB (no growth) |
| `SDE_RENDERER=gles2` (forced GL, i.e. llvmpipe here) | 119.7 MB | n/a |

### Per-client (session) memory

The meter reports every Wayland client, and then the rest of the session's
processes: the services LibWM spawns on demand (the AudioServer, the
NotificationServer) that share the compositor's process group but never connect
as clients. The summary carries `clients`, `client_pss_kb`, `session_pss_kb`
(compositor + clients + services) and a `client_memory=` breakdown of the twelve
largest by PSS, labelled by the process name (the shell's clients are panels and
applets: layer surfaces with no toplevel to name them by). That makes the
compositor's own stats file the session's "who is using memory" report, and
`scripts/run-session-memory-test.sh` asserts it on the session's real startup
list (Taskbar + Desktop + applets + BatteryApplet).

Measured headless (1280×720), PSS in kB:

| Session process | PSS | RSS |
|---|---|---|
| WindowServer | 17 975 | 44 000 |
| FileManager (Desktop) | 8 165 | 30 800 |
| Taskbar | 4 101 | 23 500 |
| **Audio applet** | **2 429** | 5 600 |
| ClipboardHistory | 2 421 | 21 900 |
| ResourceGraph | 2 299 | 21 200 |
| Network | 2 277 | 21 500 |
| Keymap | 2 207 | 21 000 |
| WorkspacePicker | 2 188 | 21 000 |
| BatteryApplet | 2 150 | 20 900 |
| **AudioServer** (service) | **~301** | ~3 000 |
| **session total** | **46 609** | **258 608** |

Read the two columns together: **RSS says the session uses 253 MB; PSS says
45 MB.** Ten clients each "use" the whole shared Lagom stack, so RSS multiplies
it. Every applet now costs ~2.2 MB.

### What that measurement found, and the three fixes

The Audio applet used to be the outlier at 11.0 MB PSS. Neither of the two causes
was the one we expected:

1. **An audio backend it never used** (~1.5 MB). The applet links
   `liblagom-audio` for `Audio::ConnectionToManagerServer` (the AudioServer
   *protocol* client) and that library does
   `target_link_libraries(LibAudio PRIVATE pulse)`, so it DT_NEEDs libpulse
   (+ libpulsecommon + libsndfile), mapped by this process alone and made
   resident by the dynamic linker at startup. The desktop's audio path is
   PipeWire (the AudioServer service), so SDE now builds **`SDEAudioClient`**
   (`Applets/CMakeLists.txt`) from the one LibAudio translation unit the applets
   need, and the applet links that. libgui-wayland's LibAudio keeps its
   PulseAudio backend, as it should.
2. **A screen-sized placeholder for an unsized window** (~7 MB, the real cost).
   The applet is the only one that `show()`s a window without setting a size
   (Keymap calls `resize(16, 16)` first), and the shim substituted *the screen
   size* for an empty size, so the applet allocated three 1280×720 buffers
   (`1280×720×4` of memfd, exactly three 3.5 MB mappings) and threw them away
   when the compositor reconfigured it to 16×16. The placeholder now depends on
   the window type: a panel or applet (which the compositor lays out from its
   applet area) gets a 16×16 placeholder, and only a window that will fill the
   output (fullscreen, or the Desktop, anchored to every edge) assumes the screen
   size.
3. **The service's own backend** (~9 MB, 0.3 MB now). The SDE's **AudioServer**
   also linked `liblagom-audio` → libpulse + libpulsecommon + libsndfile, for
   three header-only helpers (`Resampler.h`, `Queue.h`, `Sample.h`). It is
   PipeWire-only by design, so it links no audio library at all now: the service
   costs ~0.3 MB.

Session PSS fell from **53.9 MB to 46.6 MB** with no behaviour change.

One test note: `run-audio-test.sh` now runs with a private `SDE_SESSION_DIR`.
LibWM *defers* to a service already listening on the session's portal path, so
without it the test was answered by a live session's AudioServer instead of
spawning its own (its spawn assertions failed, and it would have replaced the
live session's sockets).

### Where the rest goes (the other applets)

The natural follow-up is "and the other applets?", so here is the same session
split by what the pages *are* (one representative headless 1280×720 run, PSS in
kB; "pixel surfaces" is `memfd` plus screen-sized anonymous mappings, "shared
libraries" the process's share of the Lagom/shim/libc stack):

| Process | total | pixel surfaces | shared libraries | heap | binary, fonts, anon, stack |
|---|---|---|---|---|---|
| WindowServer | 17 729 | 10 930 | 3 436 | 1 784 | 1 579 |
| FileManager (Desktop) | 8 207 | 3 644 | 2 694 | 884 | 985 |
| Taskbar | 4 065 | 148 | 1 378 | 1 812 | 727 |
| each applet (×7) | ~2 150 | ~10 | ~1 200 | ~390 | ~550 |

Read the sum, not the rows: **the sum of PSS across the session is the session's
*unique* resident memory**: a page in a shared library counts once however many
clients map it. So there are only two levers: private memory (a heap, an
anonymous region, a pixel surface) and dropping a mapping that *every* client
maps.

The applets have nothing left to trim. All seven are uniform at 2.1–2.4 MB
(~1.2 MB of shared stack, ~0.9 MB private), and every one of their 32 DT_NEEDED
libraries comes from LibGUI's link closure, not from the applet (the applet's
own binary is ~200 kB of that; `Audio`'s old libpulse/libgstreamer-style outlier
is gone, above). Dropping a library from *some* clients would save nothing at
all under the rule above.

Ranked by size × how much of it is actually addressable:

1. **Pixel surfaces: 14.8 MB, 32 % of the session**, and the only term that
   grows with resolution (one 4K output surface is 33 MB). The compositor holds
   two wlroots output buffers (shm on headless, a scanout/GBM buffer on DRM)
   plus a cached full-screen desktop background bitmap; the Desktop holds the one
   backing store a full-screen client needs. Reducible parts: render the
   background per frame instead of caching it, and note that overlays allocate a
   fresh full-screen `Gfx::Bitmap` on every render (`ChromeNode::render`), cheap
   while the tile preview and window switcher are small, not once they are
   output-sized.
2. **The framework's per-client private cost: ~0.85 MB × 9 clients ≈ 7.5 MB.**
   ~390 kB heap, ~240 kB of *uniform* anonymous pages (every applet, however
   small, has the same ~132 kB region plus a set of small ones), ~200 kB binary
   text. That uniform part is startup cost every LibGUI client pays; the
   structural lever is fewer client processes (bundling the seven applets into
   one would save ~0.85 MB each), which is a deliberate divergence from
   SerenityOS's one-process-per-applet model.
3. **The shared LibGUI stack: ~16 MB, 34 %**, but it is clean file-backed
   memory the kernel reclaims under pressure, and it counts once for the whole
   session, so trimming a DT_NEEDED only pays when *no* client uses it.
4. **The Taskbar's 1.8 MB heap: 4× an applet's, at startup, with no windows
   open.** Measured, and not a leak: it is *live* (glibc's
   `MALLOC_TRIM_THRESHOLD_=0` does not return it), it is small-allocation `brk`
   heap (profiling its startup records no anonymous `mmap` at all, so no
   screen-sized buffer hides in it, unlike the Audio applet's old three), and it
   stays at exactly 1 812 kB while thirty clients start and exit.

   ~0.6 MB of it is the system menu, at **8.7 kB per installed app**, linear: a
   resource root with 373 app files (300 duplicates) takes the heap to 4 196 kB.
   That is upstream's eager menu construction: an `AppFile`, a `Gfx::Icon` bitmap
   and a `GUI::Action` per app, built at startup; the SDE only made it visible by
   shipping 73 app files instead of ~40. A lazy menu would trade that memory for a
   first-open hitch, so it is recorded, not changed.

   The rest is startup state any LibGUI app pays (the framework baseline is
   ~390 kB) plus the Taskbar's own chrome. Splitting the 8.7 kB further needs an
   allocation profiler, and that is worth writing down: our `Profiler` is a
   CPU-and-mapping sampler, and the SerenityOS profiling interface's
   malloc/free events (`Profile::Event::MallocData`) have **no producer on Linux**:
   on SerenityOS the kernel owns malloc, here glibc does. A small `LD_PRELOAD`
   interposer emitting those same events would close the gap for every process,
   not just the Taskbar.

   Chasing it did find a real bug, though: a pinned quick-launch entry whose `.af`
   is missing or invalid aborted the whole shell (`AppFile::name()` VERIFYs on an
   empty name, and the quick-launch path never checked `is_valid()`; the system
   menu path does). Fixed by `patches/0009-taskbar-quicklaunch-missing-app.patch`;
   `scripts/run-taskbar-quicklaunch-test.sh` runs the shell against a resource root
   with no app files at all and asserts it still comes up.

### The first fruit: `auto` no longer loads software GL for nothing

`auto` used to hand the headless backend to wlroots' GL path, which on a machine
with no DRM device can only be **software GL (llvmpipe)**: **119.5 MB** idle,
against 35.6 MB for pixman, for a *frame time that is the same or slightly
worse* (p95 0.73 ms vs 0.60 ms while dragging). Almost all of that is Mesa's
gallium/LLVM footprint ([heap] alone was 13.3 MB, plus libgallium and libLLVM)
that pixman never touches.

`auto` now checks `wlr_backend_get_drm_fd()`: **with no DRM device it takes
pixman directly**, so the GL stack is never initialised: **84 MB, ~70 %, saved**
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
- **Server-side frames keep only their decoration.** A window frame is painted
  once, then split into titlebar/border strip buffers; the content area is a
  solid scene rect. A maximised 1280×800 window no longer keeps a 16 MiB ARGB
  frame bitmap alive at scale 2.
- **Menubars are small subsurfaces, not full-window compositions.** A client
  with a menubar used to redraw the menubar into a second window-sized buffer on
  every present (about 11 MiB for a large Terminal, and a full-window copy per
  repaint). The client now presents the strip from a menubar-sized subsurface at
  `y = -inset` and tells the compositor to offset the content tree with
  `serenity_toplevel.set_menubar_inset`; the old full-window composition remains
  as a fallback for a compositor without `wl_subcompositor` or a version-2
  `serenity_toplevel`. Screenshots are pixel-identical at scale 1 and 2.
- **The wallpaper is on demand.** The compositor does not push the image when a
  client binds `serenity_window_manager`; `get_wallpaper()` reads the persisted
  raw state when a client actually needs it. Only the desktop and Display
  Settings pay for the image, instead of every applet and terminal carrying a
  private copy (3 MiB each for the default 1024×768 wallpaper, 16 MiB each for
  2560×1600).
- **The desktop background is scene-rendered.** `update_desktop_background()`
  keeps a solid colour rect plus a wallpaper scene buffer, and only rebuilds
  those nodes when the output size, wallpaper, mode or colour changes. It no
  longer allocates a full-output CPU-composited bitmap.
- **Terminal cells are compact.** Hyperlinks are stored once in a terminal-level
  table and cells hold a 32-bit index. `VT::Line::Cell` is 28 bytes instead of
  56, halving scrollback memory (about 4.6 MiB saved for a 1024-line,
  160-column terminal).
- **The polkit agent is native.** `sde-polkit-agent` is a small AK/LibCore
  service over the same `dbus-1` transport as the portals (no GLib/GIO, no
  PyGObject). It registers on the system bus, serves
  `org.freedesktop.PolicyKit1.AuthenticationAgent`, and talks to the trusted
  polkit helper over `/run/polkit/agent-helper.socket` (or the setuid helper's
  pipes). Its idle PSS is ~1 MiB, against ~19 MiB for the Python agent it
  replaces.
- **One applet supervisor.** The session starts every applet through one
  `sde-supervise-applet.sh` instead of one Bash wrapper per applet. Each applet's
  backoff is its own short-lived timer process, so one applet waiting to restart
  does not delay another. In a controlled eight-applet run the supervisor's PSS
  fell from ~3.6 MiB (eight Bash wrappers) to ~0.9 MiB (one host). The applets
  themselves remain one process each (they are unmodified SerenityOS clients).
- **Bounded per-frame work.** Timers are short-lived (animation timers stop when
  the animation ends), the key-repeat timer is a single `Core::Timer`, and the
  session's watchdog heartbeat is a 1 s file write.

## 6. Running it

```sh
scripts/run-perf-test.sh                     # the software fallback (pixman)
SDE_PERF_RENDERER=auto scripts/run-perf-test.sh
```

Five phases: **(A)** idle pacing: nothing changes, so nothing may render;
**(B)** 60 Hz pointer motion: frame time and input → present inside the budget,
with the p99 held to the 240 Hz interval; **(C)** dragging a decorated window,
the heaviest path we own; **(D)** a **240 Hz output** (the compositor is
restarted with `SDE_OUTPUT_REFRESH=240`) with a 4 ms damage stream, where the
cadence itself is asserted (~4.17 ms) along with the work fitting inside it;
**(E)** memory: the idle footprint and that 50 window cycles do not grow it.

The knobs (`SDE_PERF_FRAME_BUDGET_MS`, `SDE_PERF_LATENCY_BUDGET_MS`,
`SDE_PERF_IDLE_FRAME_LIMIT`, `SDE_PERF_FAST_MS`, `SDE_PERF_AGGRESSIVE_MS`,
`SDE_PERF_RENDERER`) let a slower machine or a stricter CI gate be expressed
without editing the script.

The session's memory budget is enforced separately, on the session's real startup
list (Taskbar + Desktop + applets + BatteryApplet):

```sh
scripts/run-session-memory-test.sh
```

The per-client wallpaper copy is guarded by:

```sh
scripts/run-wallpaper-on-demand-test.sh
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
- **A direct-scanout positive check.** Start a hardened session with
  `SDE_PERF_STATS=/tmp/sde-perf` and map a fullscreen client
  (`build/tools/native-app-test --fullscreen --linger 5000`). `direct_scanouts`
  must grow and the log must say `direct scanout active`; a client with
  compositor chrome/overlays may legitimately stay on the render path. The
  headless budget test asserts the opposite (samples > 0, direct = 0).
- **Hardware capability report.** `scripts/report-rendering-capabilities.sh`
  prints the GPU, connectors, EDID, `max bpc`, colour-space, HDR metadata and
  `vrr_capable` properties. Run it on the target before assigning hardware work.
- **The drag tail above**: one ~5 ms frame on the GLES path, which is what
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
