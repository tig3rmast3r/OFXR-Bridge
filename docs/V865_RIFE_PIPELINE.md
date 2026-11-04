# V865: RIFE GPU handoff and native source overlap

V864 could spend about11ms inside each source-image Release while its separate
synthesis worker finished. In the owner's MSFS comparison, RIFE3X TensorRT was
valid throughout but produced79.5FPS, versus89.3FPS for Djules3X without F10.
The clean RIFE worker cycle took33.1ms plus4.6ms before the next job. The native
SteamVR EndFrame call itself returned quickly. F10 explains Djules' final81FPS
segment; that segment is excluded from the comparison.

## Changes

Async RIFE composition now stays ordered on its own D3D12 queue instead of
blocking its CPU thread after every generated output. Separate pack/compose
command allocators and lists allow the next input preparation to proceed safely.
Pack descriptors0..11 and compose descriptors12..17 remain disjoint. The next
pack fence retires the preceding compose before its allocator, descriptors or
CUDA output buffer can be reused. Inputs/outputs are retained across deferred
work; dimension changes and destruction drain the exact pending transaction.
The containing synthesizer publishes its completion fence after final compose,
post-processing and copies. Synchronous callers retain their original wait.
A failed transaction cannot reset or reuse a possibly pending allocator.

Ordinary native D3D12 RIFE source capture now has a separate ownership handoff.
Once ALL sources/cameras have been snapshotted and jobs have acquired their
history leases and completion tickets, each source swapchain may capture ONE
next image while generation completes. Three history slots and their existing
consumer fences still protect reuse. The synthesis-busy flag remains set;
EndFrame cannot admit a second generation job before the previous one finishes.
A monotonic per-job credit prevents repeated early captures from overfilling
history. Bridges, Luke OSR/AFW, composition-shared images and other generators
retain the existing full-completion handoff. Both private Framework and Director
can use the native RIFE ownership handoff. No pacing clock, queue capacity,
Buffer/Fast policy, neural rendering, shader, model, runtime or TensorRT cache
identity changes are made.

## Validation

- Native4396x4028, two independent single-eye queues, RIFE4.4 Potato with the
  existing440x403 TensorRT engine: exact pixels for2X/3X/4X comparing synchronous
  and queue-ordered completion, including final output publication fences.
-32 interleaved ABBA3X samples in one process: initial mean7.47ms synchronous vs
  6.80ms queue-ordered; final idle repeat7.07vs6.37ms. Concurrent-build samples
  were noisy (7.20vs7.33ms), so these are not game-FPS gain measurements. This compiled-only benchmark
  skips if its440x403 TensorRT engine is still being prepared.
- Source handoff contract: captures block until sources are owned, both eyes
  obtain one credit while generation is busy, additional captures/strict paths
  wait, a new job invalidates the old readiness flag, worker failure wakes waiters.
- Real shipping Release/capture with the EXACT private RIFE eye queue deliberately
  blocked on a fixture-owned GPU fence, Framework and Director: the old ownership
  signal withheld reproduces Release blocking; restoring ownership lets the next
  source return while that GPU fence is still unsignalled. Existing completed
  frame identity/pixel checks continue to pass. A read-only submission-queue
  diagnostic identifies the correct eye queue without changing production policy.
- Existing native private Framework/MSFS and Director/MSFS tests,3X application
  overlap, Director retained-output overlap, independent eye queues, real CUDA
  async FIFO/shutdown tests.
- Actual worker rejection after a deferred output, and pending-output destruction.

Headset/game performance and visual acceptance remain pending the owner's run.
Use matching V865 tray and game layer after restarting both. No TensorRT
recompilation is required by this native-only change. Control ABI86201 retained.
