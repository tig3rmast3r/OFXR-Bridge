# V859: Hybrid's depth search keeps thin near surfaces

## Symptom

On a lateral move, a thin near object (a pole, a bar) swept across background
that both frames show went missing from Hybrid's generated frames.

## Cause

The vectors' solve from a generated pixel finds that background, whose samples
agree, and takes it. 0.2 fixed this with the game's depth (V505) and made the
search a midpoint splat (V555). This ports both, at 0.2's built-in values
(Nearer 1.5, agreement 0.25), without the tuning panel.

## Setting

**Depth search** under Hybrid, saved per game (`hybrid_depth_search`), applied
live, on by default (0.2's switch, V532, was off).

## How it works

- The guide's depth is bound at t8 (t7 is FSR's) and the splat at t9.
  `ClearDepthSplat`/`BuildDepthSplat` write, per vector texel, the surface at
  the generated instant (its displacement and log-depth rank), one slice per
  eye and synthetic output, up to 4X.
- Where that surface is nearer than the vectors' answer by 1.5x, one solve
  from where it is in the next frame, held to 0.25 agreement, replaces the
  answer and reports that agreement as its error, so the flow keeps off it.
- FidelityFX and NVIDIA hybrids share it. `XRFG_DISABLE_DEPTH_SPLAT=1` leaves
  it out.

## Flight log

`embedded_configuration` 867: a synthesizer's first splat for an eye and
whenever its grid or outputs change; a = eye << 48 | columns << 20 | rows,
b = the synthetic outputs splatted.

## Test

Headset: Dead Island 2. `xrfg_d3d12_history_tests hybrid` runs 0.2's bars
scene on FidelityFX and NVIDIA, along both axes, in every 2X/3X/4X output:
coverage 0 without depth, 1.0 with the search, 0 with the splat disabled or
the setting off. Autonomous control checks cover the default, the saved value
and the UI merge.
