# Serenity Desktop Environment: Font parity decision (WS7)

**Status:** Decided: R4.7 landed the session font broker; **Katica stays the SDE default** (picking a scalable family such as SerenitySans is what enables size zoom) · **Date:** 2026-10-10
**Related:** [`02-compositor-decision.md`](02-compositor-decision.md) §11 · [`04-known-gaps.md`](04-known-gaps.md) · [`05-r3-plan.md`](05-r3-plan.md) §4 WS7 · [`09-r3-implementation-plan.md`](09-r3-implementation-plan.md)

## 1. Decision

**R3 keeps the bundled bitmap **Katica** family as the default UI font, and
size-based font zoom remains a no-op.** This is accepted parity with upstream
SerenityOS (which also defaults to Katica), not a regression, and it does **not**
block the milestone. Selecting and scaling system fonts is deferred to the
settings work (objective **G5**, milestone **R4**).

## 2. Why (evidence)

- **The default families are bitmap-only at fixed sizes.** `Base/res/fonts`
  ships Katica (and the fixed-width Csilla) only as `.font` bitmap faces at 10 and
  12 pt (`KaticaRegular10.font`, `KaticaRegular12.font`, …).
- **An inexact size does not resolve.** `Widget::set_font_size()` goes through
  `Gfx::FontDatabase::get(family, size, weight, width, slope,
  Font::AllowInexactSizeMatch::No)`. `Typeface::get_font()` returns a bitmap face
  only for an exact size and otherwise returns `nullptr`, so `set_font` keeps the
  widget's current font. The zoom actions therefore have nothing to switch to.
- **The settings path is inert.** DisplaySettings ▸ Fonts builds a font picker and
  calls `set_system_fonts(...)`; the port's shim accepts and ignores it (it must
  return a valid response so the synchronous caller does not block).
- **Confirmed by a test.** The SDE's `libwm-font-parity-test` (`test/`) asserts
  the behaviour directly (run it with `SERENITY_RES_ROOT` pointing at the
  resource root):

  ```
  FONT-PARITY: Katica 10 resolved=true
  FONT-PARITY: Katica 14 resolved=false (expected false: no such bitmap face)
  FONT-PARITY: SerenitySans 14 resolved=true (expected true: scalable)
  FONT-PARITY: zoom-no-op confirmed=true
  ```

- **A scalable face is already bundled.** `Base/res/fonts/SerenitySans-Regular.ttf`
  is a vector font, and `Typeface::get_font()` returns `scaled_font(size)` for it
  at any size; this is the path to working zoom, not new asset work.

## 3. What this means for R3

- The default UI font, the window-title font and the fixed-width font all render
  correctly at their intended sizes; the shell, apps and chrome look right.
- **TextEditor Zoom In/Out (`Ctrl+=` / `Ctrl+-`) does nothing** with the stock
  font, identical to upstream SerenityOS with the default theme.
- **DisplaySettings ▸ Fonts is inert**: the picker lists the available fonts, but
  Apply has no effect.
- Theme switching (font/colour roles) works and is unaffected by this decision.

## 4. The R4/G5 path (what the real fix needs)

Working size-based zoom and system-font selection require, in order:

1. **A scalable default or a font selector.** Either default the UI family to a
   scalable face (`SerenitySans-Regular.ttf`) or let the user pick one of the
   bundled TTFs. The bitmap families stay available at 10/12.
2. **A session font channel.** Wire `set_system_fonts` to real state applied to
   every client and the compositor, the same broker pattern as the theme
   (`$XDG_RUNTIME_DIR/sde-theme`), plus persistence (`WindowServer.ini`
   `[Fonts] Default/FixedWidth/WindowTitle`). Today the shim only accepts the
   call.
3. **Live relayout.** On a font change, notify running applications to re-resolve
   fonts and re-layout (LibGUI has no system-font-change broadcast today), and
   restyle compositor chrome.
4. **Re-baseline the visuals.** A scalable default changes text metrics, so the
   layout tests and any screenshot comparisons must be re-baselined deliberately.

## 5. Non-goals for R3

- No font picker wiring, no persistence, and no session font broker in R3.
- `libwm-font-parity-test` is the guard for this decision: when the R4 change
  lands it should be updated to assert that a scalable default resolves at
  arbitrary sizes (flipping the expectation intentionally).
