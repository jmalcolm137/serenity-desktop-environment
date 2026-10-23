# WLCS (Wayland conformance suite)

WLCS is the closest thing to a portable Wayland compositor conformance suite.
It is an external project, not vendored here:

- upstream: <https://github.com/canonical/wlcs>
- pinned revision: `f26abf01de4611bf63da97f15c743e16abcb4bf5`
- checkout: `third_party/wlcs` (ignored by git)

`scripts/fetch-wlcs.sh` maintains the checkout, and `scripts/build-wlcs.sh`
builds it in `build/wlcs` together with SDE's integration module. WLCS needs
GoogleTest and the Boost headers; `scripts/ci-deps.sh` installs `gtest` and
`boost`. On systems that have only `boost-libs` installed, `build-wlcs.sh`
generates the few Boost headers WLCS actually uses in the build directory.

## Running

```sh
scripts/run-wlcs-test.sh
```

The default filter is the core/surface/keyboard subset that the harness fully
supports today. To run a protocol suite or the whole thing:

```sh
WLCS_FILTER='xdg_toplevel*' scripts/run-wlcs-test.sh
WLCS_FILTER='*' scripts/run-wlcs-test.sh
```

Every run creates a private runtime directory and a private headless
`WindowServer`; it never connects to or signals a live session. The test runner
is marked `ci-tier: full` and is picked up by `scripts/run-all-tests.sh --tier full`.

## The integration

`Tests/wlcs/sde_wlcs_integration.cpp` implements `WlcsDisplayServer` by:

1. starting `WindowServer` with the headless backend on a private socket;
2. primed virtual pointer, keyboard and touch devices before the test client
   connects, so the seat capabilities stay stable for the whole test;
3. giving WLCS a connected Wayland client socket;
4. injecting pointer/keyboard events through wlroots' virtual-device protocols
   and touch through the SDE-private `serenity-virtual-input` protocol;
5. using a test-only `serenity-wlcs-control` global (created only when
   `SDE_WLCS_CONTROL=1`) for `position_window_absolute`, because Wayland has no
   client request for compositor-owned placement.

The control global is not advertised in a normal session. The virtual-input and
pointer/keyboard protocols already existed for the headless harness.

## Known failures

These are upstream limitations rather than integration failures, and are kept
visible so a future wlroots/WLCS update can be checked against them:

- `ShmTest.truncated_shm_file_is_an_error` (and the second-server variant):
  wlroots' `wl_shm` accepts a buffer from a truncated pool instead of raising a
  protocol error.
- `WlPointerTest.set_cursor_on_a_surface_with_another_role_is_a_protocol_error`
  and the subsurface variant: wlroots posts the role error on the `wl_surface`
  resource rather than the `wl_pointer` resource the protocol specifies.
- `WlPointerTest.set_cursor_with_a_stale_serial_is_ignored`: wlroots assigns the
  cursor role before emitting `request_set_cursor`, so the compositor cannot
  reject a stale serial before the role has been applied.
- The parameterised `SurfaceBuilder` touch tests attach a buffer before the
  initial xdg configure has been acknowledged; the compositor correctly reports
  `xdg_surface has never been configured`, but WLCS does not wait for the
  configure in that builder.
- The same eager-buffer pattern appears in the interactive
  `XdgToplevelStableTest` move/resize tests, so they fail at the first commit
  with that protocol error before exercising the compositor's move path.
- `XdgToplevelStableTest.pointer_respects_window_geom_offset` reports the
  pointer at window-relative coordinates without the configured window
  geometry offset; this is a WLCS geometry/positioning expectation mismatch
  still under investigation.

These are tracked as conformance findings, not worked around by weakening the
compositor's protocol checks.
