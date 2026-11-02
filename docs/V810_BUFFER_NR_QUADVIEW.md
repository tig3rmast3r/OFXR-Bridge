# V810: SteamVR Buffer, NR view regions and FSR foveated guides

Local experimental successor to V809 (1295bf075bc5740b7bc789b9d23a0f5863bd605d),
on the 800/RIFE lane. Public V759 remains the chosen 0.3.2 release candidate.
No Forza diagnostics or rejected OSR dilation. Shared RIFE worker/models unchanged.

## SteamVR Buffer

Framework performed its optional prediction renewal, metadata and tracing after
its final cadence wait. These now run in an early window, followed by the precise
final wait immediately before native EndFrame. Diagnostic work must not become
an alternating addition to the display interval. Director keeps V809's physical
clock and ownership policy; both modes share the conditional reserve extension.

The private reserve may grow from its former 0.75 physical-period limit to at most
1.75 periods: ONE extra period (11.111ms at 90Hz), learned only when required.
The 64-output observation window must reach at least 99% refresh and contain
both sufficient jitter and residual sub-period lateness. Growth remains <=0.5ms
per qualifying window. No allocation of a fourth FIFO credit, fixed extra pause,
quiet decay or sustained-low-FPS buffering. OFF, mode/epoch, refresh and session
resets retain V809 ownership/drain semantics. The Legacy VDXR call retains its
original default 94% gate and 0.75P cap; no Legacy scheduling source changed.

V809 MSFS source evidence: packets5423/6503 were accepted ~21ms before their
copy began, already GPU-ready at dequeue. The later CPU fence waits were16.947/
15.633ms. This identifies downstream delays, NOT game render durations or pure
GPU copy cost. GPU contention/preemption and CPU wakeup remain possible.

Diagnostic stage31 (flight5741) records intended buffered handoff QPC and ready
lead. Stage32 (5742), D3D12 only, records copy GPU start/end calibrated to QPC.
Two timestamp queries live on the EXISTING command list and read after its
EXISTING copy fence. No extra submission/wait. Comparing GPU start with stage13,
GPU end with stage14 distinguishes queue delay, GPU elapsed interval and CPU
completion observation. GPU elapsed time can include preemption. Logging OFF
records no queries; query/clock-calibration refusal never fails presentation.

## NR before SR: all four native view histories

Previously nonzero colour origins were refused and full-scale depth/MV origins
were also refused. Each native handle can now process its independent colour,
depth and motion ROI in a shared 2D allocation. Colour is staged only if needed;
depth/motion are matched into private zero-origin guides even at100%. The edit
returns only to that colour ROI. Guide origins are independent; vector units
are scaled exactly once. A region change resets only that view's histories.
Optional 2x NR and exact native submission retirement apply to the new resources.

Pre-SR input resources must have an unambiguous separate Texture2D contract.
Input arrays without a slice identity, invalid/MSAA regions remain refused.
An SR OUTPUT array (including four slices) is not the pre-SR colour and is not
modified by this path. This is not an implementation of arbitrary post-SR
array slicing. The owner's current trial uses NR BEFORE SR at100%.

NR_VIEW per-handle records distinguish accepted model/resolve recordings from
entry/retirement cadence. Successful records recur every128 resolves, so starting
logging after model creation still works. Skip records include input layouts.
The NR file allows shared reading while MSFS is alive.

The old game log does not identify all per-view origins or successful resolves;
therefore atlas refusal is a concrete corrected limitation, not a proven exact
mapping of the owner's affected peripheral view. Headset confirmation is needed.

## FSR foveated vectors

Restore the implementation and tests from V760 f70d2e0e4ea8c04ffa3c3859113ba1d5f83f5ce9:
camera-proven whole-texel periphery placement, exact motion/depth crop, placed XR
view FOV and motion array slice. No fractional/unknown/camera-less crop admission.
V760 identity and rejected V761 changes are excluded. The signed/provider bytes
stay the same. This restores FSR dispatch availability; it does NOT claim to fix
Hybrid or FSR visual artifacts with foveated rendering. Non-foveated behavior is
covered by retained renderer and layer tests.

## Validation and owner trial

See final-tests.xml and PACKAGE_V810.json beside this worktree's diagnostics for
actual passing test receipts. GPU fixtures execute the shipping host/codec and
resolve, with deterministic callbacks in place of the NVIDIA NR model. Four
independent views at100/50%, separate atlas origins, source without UAV, independent
history reset and untouched guard pixels are checked. Existing pre/post-SR2x
retirement/refusal, GPU output ownership and real FSR provider checks are retained.
These checks do not prove NVIDIA model quality or headset smoothness in MSFS.

Close the game and old tray, start V810 tray, then relaunch. First compare Buffer
OFF/ON in Framework (MSFS Director was unusable; use another game for Director).
Keep NR OFF for the initial pacing comparison so its GPU cost cannot confound it.
Then enable NR BEFORE SR at100% and inspect both centres and both peripheries.
Diagnostic logging records reserve, handoffs, GPU copies and per-view NR receipts.
Later, the owner will test Hybrid/FSR quality on the ground with Quad View.
