# V866: independent synthetic output banks for native RIFE Normal

On the actual V865 MSFS run, source Release is already0.04ms, per-output CPU
compose waits are gone and the next worker starts in0.1ms. A clean3X segment
still takes36.2ms per stereo source cycle (82.65FPS), including7.9ms preparing
output images. The blocking span is acquisition of private synthetic images
still being read by the endpoint's preceding copy.

Normal private SteamVR RIFE now alternates two complete banks of generated
outputs:4distinct destination handles per source at3X and6at4X. The next batch
can prepare while the endpoint retains the preceding bank. Existing image and
descriptor leases continue to guard actual reuse. Each bank retains its own
released image identity behind queued camera metadata. The existing six-slot
storage and complete-bank layout used by generated panels are reused.

Fast keeps its exact previous destination allocation (2at3X,3at4X). RIFE2X,
Luke OSR/AFW, native VDXR, other generators and panel policy retain their existing
allocation. Buffer/Fast timing policy, endpoint FIFO capacity, presentation
clocks, GPU shaders, neural rendering, Python/model files and TensorRT engine
identity are unchanged. Changing Fast or multiplier drains the old output shape
before replacing it, through the existing live-control reconciliation.

At4396x4028RGBA8 with two independent eye resources,3X Normal needs about270MiB
of additional texture payload plus allocation overhead. This is output storage;
no new intentional presentation delay is added. The user's benchmark suite and
winner policy are unchanged.

Targeted validation covers allocation at2X/3X/4X, Normal/Fast, existing panels and
other runtimes/generators; a deliberately withheld actual endpoint image-copy
reader (same-bank acquire blocks, the other bank remains available, exact native
pixels and camera order preserved); actual RIFE2-3-4-3-2live switches with Normal/
Fast/Normal changes inside every phase in Framework and Director; existing3X/4X
retained-output and next-source capture checks. Test evidence accompanies the
local build. No broad game or headset benchmark is implied by these fixtures.

Actual MSFS pacing/FPS and headset visual acceptance remain for the owner to
measure. The7.9ms observed reuse wait is not a promise that the entire interval
will disappear after overlapping GPU work. Use matching V866 tray/game layer;
no engine recompilation is required by this native allocation-only change.
Control ABI86201 retained. Source base is V865; the separate V900 experiment
remains on its own branch.
