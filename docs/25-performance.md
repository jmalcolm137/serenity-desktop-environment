# Performance & latency budget (R6, N3)

**Status:** landed — metered and enforced by `scripts/run-perf-test.sh`.
**Related:** [`01-objectives.md`](01-objectives.md) N3 ·
[`03-engineering-conventions.md`](03-engineering-conventions.md) D7 ·
[`08-compositor-gaps.md`](08-compositor-gaps.md) §5–7

N3 asks for **low, predictable input latency; smooth 60 Hz; and a software
compositor fallback**. This is the budget for that, how the compositor measures
itself, and the test that keeps it true.

## 1. The budget

| Metric | Budget | What it covers |
|---|---|---|
| **Frame time** p95 | **< 16.6 ms** (60 Hz) | The compositor's own work for one frame: scene build, render, commit (`output_frame()`). This is the cost we own — a client's own drawing happens before its commit. |
| **Frame time** max | < 16.6 ms | No frame may overrun the 60 Hz budget, or a client's commit would be delayed to the next vblank (a visible hitch). |
| **Input → present** p95 | **< 20 ms** headless | From the compositor handling an input event to the frame that presents it (`notify_activity()` → the next `output_frame()`). On a headless output there is no vblank, so this is the compositor + client share of the whole path; on real hardware add the refresh interval (≤ 16.6 ms) for input-to-photon. |
| **Idle frames** | **≤ 3 per second** | A compositor must not render when nothing changed. Rendering on damage only is what makes 60 Hz *affordable* rather than a busy loop. |

The budget is per output at 1280×720 with the **software (pixman) renderer** —
i.e. the fallback N3 asks for is the case we hold to the budget. GLES renders
less on the CPU and is bound by the GPU instead; it is metered by the same code.

## 2. Instrumentation

The compositor meters itself **always** (two `clock_gettime` calls per frame —
nothing measurable), and reports on demand:

| Variable | Effect |
|---|---|
| `SDE_PERF_STATS=<path>` | Rewrite a `key=value` summary of the meter to that file, once a second. |
| `SDE_PERF_LOG=1` | Also log a one-line summary each second, so a session can be watched live. |

Both are off by default, so a normal session pays nothing; neither changes the
code path being measured (no "profiling build").

Each metric keeps a ring of the last 600 samples (~10 s at 60 Hz — fixed memory,
so a long-running session cannot grow) plus cumulative totals. The summary is:

```
frames=179                 slow_frames=0
fps=41.0
samples=179
frame_ms_budget=16.6       frame_ms_p50=0.026   frame_ms_p95=0.046
frame_ms_max=0.106         frame_ms_over_budget=0
frame_interval_ms_p50=16.071   frame_interval_ms_p95=16.204
input_to_present_ms_budget=50  input_to_present_ms_p50=0.043
input_to_present_ms_p95=0.106  input_to_present_ms_max=0.157
input_to_present_ms_over_budget=0   input_to_present_samples=110
```

`frame_interval_ms_*` is the spacing between frames: under a 60 Hz input cadence
it settles at ~16.07 ms, which is how "smooth 60 Hz" is verified rather than
assumed.

### How the numbers are taken

- **Frame time** — around the compositor's own work in `output_frame()`. The
  wlroots scene renders only the **damage** for the frame (plus its buffer-age
  reuse), so this is the cost of what actually changed, not of the whole output.
- **Input → present** — `notify_activity()` (the hook that already resets the
  idle timer, i.e. "the compositor saw input") records the time; the next
  `output_frame()` reports the interval to its commit. The **most recent** input
  wins, because that is what the frame ends up showing. It approximates the
  user-visible path: it includes the client's response and the buffer hand-off,
  but not the display's vblank (there is none headless).

## 3. Reference numbers

Measured on the development machine (headless, 1280×720, `SDE_OUTPUT_SCALE=1`)
with `scripts/run-perf-test.sh`; times in milliseconds:

| Phase | Renderer | frame p50 | p95 | max | input→present p50 / p95 |
|---|---|---|---|---|---|
| Idle (2 s) | pixman | — | — | — | 0 frames rendered |
| Pointer motion at 60 Hz | pixman | 0.026 | 0.046 | 0.106 | 0.043 / 0.106 |
| Dragging a decorated window | pixman | 0.033 | 0.651 | 1.935 | — |
| Pointer motion at 60 Hz | auto (GLES/llvmpipe) | 0.026 | 0.045 | 0.065 | 0.042 / 0.102 |
| Dragging a decorated window | auto | 0.032 | 0.734 | 4.801 | — |

So the compositor's own per-frame cost is **three orders of magnitude** inside
the 60 Hz budget on this workload: dragging a decorated window — the heaviest
path we own, since every motion step moves the window and re-renders its frame —
peaks at ~2 ms with the software renderer. That headroom is what pays for the
per-output granularity of a commit and for slower machines.

These numbers are a trend record, not a promise: re-run the test after any change
to the render path and compare.

## 4. What keeps it inside the budget

- **Render on damage only.** `wlr_scene_output_commit()` is driven by damage;
  the compositor has no "tick" that repaints (the animation timers only run while
  an animation is in flight, and the 1 s heartbeat writes a file, not pixels).
  Phase (A) of the test guards exactly this.
- **Damage granularity.** Frames are rendered from the wlroots scene's damage and
  its buffer-age reuse, so a pointer move or a titlebar hover costs the changed
  region, not the output.
- **Compositor chrome is cached.** Window frames, the cursor highlight, the
  switcher's overlay and the stack-switch overlay are `ChromeNode`s: a scene
  buffer that is re-rendered only when it changes (a LibGfx `Painter` draw into a
  bitmap at the output scale), not per frame.
- **A per-frame early-out for the desktop background.** `update_desktop_background()`
  rebuilds the background bitmap only when the output size or the wallpaper
  changes; otherwise it returns immediately.
- **Bounded per-frame work.** Timers are short-lived (animation timers stop when
  the animation ends), the key-repeat timer is a single `Core::Timer`, and the
  session's watchdog heartbeat is a 1 s file write.

## 5. Running it

```sh
scripts/run-perf-test.sh                     # the software fallback (pixman)
SDE_PERF_RENDERER=auto scripts/run-perf-test.sh
```

The knobs (`SDE_PERF_FRAME_BUDGET_MS`, `SDE_PERF_LATENCY_BUDGET_MS`,
`SDE_PERF_IDLE_FRAME_LIMIT`, `SDE_PERF_FAST_MS`, `SDE_PERF_RENDERER`) exist so a
slower machine or a stricter CI gate can be expressed without editing the script.

To watch a live session:

```sh
SDE_PERF_STATS=/tmp/sde-perf SDE_PERF_LOG=1 loginctl terminate-session "$XDG_SESSION_ID"
watch -n1 cat /tmp/sde-perf
```

## 6. Not covered yet

- **Input-to-photon on real hardware.** The headless number excludes the vblank;
  a DRM measure would need a high-speed capture or a hardware timestamp path.
- **A per-hardware/GPU matrix.** The budget is enforced on one headless
  configuration (plus the renderer knob). R6's CI matrix would run it per renderer
  and per output scale, and a 4K software-render budget would come from that.
- **Client-side cost.** For Serenity apps the window contents are drawn on the
  CPU by LibGUI/LibGfx; the budget above deliberately measures the *compositor's*
  share, and a client that draws slowly shows up as a low `fps` rather than as
  frame time.
- **Frame-pacing policy.** The compositor presents when a frame is committed and
  the output signals a frame; there is no explicit frame-rate cap or
  missed-vblank accounting yet (`frame_interval_ms_p95` is the closest proxy).
