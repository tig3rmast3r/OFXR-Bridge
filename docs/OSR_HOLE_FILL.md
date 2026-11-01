# OSR band fill: OFXR's fill of the stereo band in AFW output

OSR (OFXR Stereo Reconstruction) is OFXR's own work on the reconstructed eye
of Luke Ross titles, written from the public provider ABI and OFXR's inputs
only. This first piece keeps AFW as the renderer and removes its one artefact
that no input can fix. It consumes exactly what AFW consumes (the source eye
colour, the converted depth, the composed motion vectors and the camera data),
so every title OFXR prepares for AFW works with it unchanged.

## The artefact

Cyberpunk 2077 F8 captures of 2026-10-29, strafing past a pole:

- AFW places the pole exactly halfway between the previous and next real
  frames of the generated eye, so the camera, depth and motion inputs are
  right.
- A streaked band 20-25 px wide hugs the pole on the side the player moves
  toward, only in the eye on that side. It is background the destination eye
  sees but the source eye hides behind the pole, and the same eye's previous
  frame is further behind still, so no provider input contains it. The
  provider fills it by stretching edge columns.
- The provider leaves that smear over the whole band whenever the eye leads,
  not only over the part its history could not have seen: with OSR filling
  the inner 62% of a run, dark pixels remained at positions 0.7-0.9 of it.

## What OSR does

Two compute passes on the AFW command list, `src/d3d12/osr_hole_fill.hlsl`,
driven by `src/d3d12/osr_hole_fill_pass.hpp` and the CPU rules of
`include/xrfg/osr_hole_fill.hpp`:

1. **Coverage.** Before the provider evaluates, every source depth texel is
   forward-splatted into the destination eye through `source clip -> view ->
   world -> destination view -> clip` with the provider's own matrices
   (reversed-depth variant included). A destination texel that nothing reaches
   is a stereo hole; the nearest depth is kept per texel.
2. **Fill.** After the provider returns, each hole pixel finds its run along
   the row. Runs touching the image border, narrower than 3 depth texels
   (splat cracks) or wider than 96 keep the provider's fill. The nearer
   bounding depth marks the occluder side. The part of the run next to the
   occluder that OSR owns is replaced by the background mirrored across the
   run's background edge, averaged 3x3 so no column repeats.

How much of each run OSR owns follows from the three viewpoints alone. With
the peer eye's previous camera at parameter `t` along the source-to-destination
axis, the fraction is `(1 - t) * gain`, clamped to the run: the trailing eye
(`t > 1`) and standing still (`t = 1`) fill nothing, a leading eye fills more
the faster it moves. The leading decision has hysteresis (`kOsrLeadStart`
0.90, `kOsrLeadStop` 0.98) so a still head's jitter cannot flip the fill on
and off at an object's edge. Alternate-eye primes fill the whole band, since
the provider has no history then.

The result replaces the copy into the generated eye; nothing else in the
provider path changes. The coverage texture returns to COMMON at the end of
every recording, so a recording the AFW path closes without executing (no
provider output) leaves no stale resource state for the next one. With the option off, the frame path is the existing
one.

## Settings

Luke Ross tab, **Reconstruction**: `AFW`, `AFW + OSR band fill` or `Full OSR`
(`luke_osr`; Full OSR is described in `docs/OSR_FULL_WARP.md`). **Advanced parameters** (`luke_advanced`, off by default)
reveals the OSR sliders and the provider controls (Framewarp mode, shading
rate, Debug framewarp, Ignore motion, Depth dilation). All apply live:

- **Band fill** (`luke_osr_fill`, gain 1-8, default 2): 1 fills only what no
  viewpoint saw, 2 fills the whole band once the eye clearly leads. The
  Cyberpunk verdict was that x2 alone removes the outline 99% of the time.
- **Band cover** (`luke_osr_cover`, 0-8 depth texels, default 0): covered
  texels beside the run on the background side treated as band, with the
  mirror taken from beyond them. It joins only when the whole run is filled.
- **Object trim** (`luke_osr_trim`, 0-2 depth texels, default 0): covered
  texels on the occluder side replaced as well. They hold the provider's
  smeared edge, which the 2x2 splat footprint counts as covered, but
  replacing them visibly thins the object.

The runtime ini carries the same keys in `[experimental]`. Depth dilation now
defaults to Off. The tray and headset mode labels read "AFW + OSR" (for
example "FSR + AFW + OSR + Neural") and the status line "AFW + OSR: Active"
while the option is selected.

## Diagnostics

- With Debug framewarp on, OSR tints filled pixels magenta, cover and trim
  pixels orange, and band pixels left to the provider cyan.
- Flight log `luke_afw_stage` 1300 per evaluation: result holds the history
  parameter (high word) and the fill fraction (low word) as float bits, b the
  publication, c the effective mode (bits 8+) and the history flag (bit 0).
  1301 reports a failed OSR preparation with its HRESULT.
- F8 captures add an `osr_coverage` image (R32 at depth resolution, zero =
  hole). Matching its publication to the 1300 record is how the band fill
  decisions were judged offline.

## Tests

`xrfg_osr_hole_fill` covers the history parameter, the fill fraction, the
hysteresis and the provider modes. `xrfg_settings_widgets_tests` ticks
Advanced parameters before reaching the provider controls.

## Limits

- Mirroring repeats background features; it removes streaks, it does not
  recover detail. The exact content exists one frame later and would need a
  frame of latency.
- With Object trim at 0, the first two or three colour pixels next to the
  occluder keep the provider's smeared edge.
- Dynamic objects are not considered when deciding what history saw.
