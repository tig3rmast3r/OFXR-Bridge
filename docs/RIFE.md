# Experimental RIFE route (v744)

The Frame generation menu includes RIFE between OFXR and Hybrid. Model choices
are 4.6, 4.4 and 4.3. New RIFE profiles select 4.4 with15% motion
inference. The inference slider ranges from10% to40% and applies on release. Explicit settings in existing profiles are preserved. Existing OFXR defaults are unchanged.

This is a NVIDIA CUDA experiment with an optional TensorRT backend for4.4 and
a portable PyTorch fallback. It is not an AMD backend.
No DLSS motion vectors or depth are needed. Default remains 2X. Experimental 3X/4X
are available on private native D3D12 and on Luke Ross's completed post-AFW
D3D11 route with its isolated synthesis queue (Framework or Director). Legacy,
pipelined and other interop routes retain2X; unsupported multi-output requests
are refused instead of silently duplicating outputs. Benchmarks continue
to test the existing accepted routes; this experiment is selected manually.

## Native-resolution colour

Each eye's two temporal images are first projected into the synthetic target
camera and sampled at the chosen motion-inference resolution. RIFE predicts
the forward/backward sampling displacements and blending mask at that size.
The D3D12 output shader applies those fields to the original full-resolution
colour images, including their alpha. It does **not** enlarge a low-resolution
RGB generated frame. The real frame is copied unchanged. The inference slider controls a percentage per axis,10..40%, default15%.
Full-resolution output is retained. Lower inference
resolution still reduces motion/occlusion precision, especially on fine edges.

The RIFE model implementations are derived from
[HolyWu/vs-rife](https://github.com/HolyWu/vs-rife), pinned in runtime-lock.json.
The local adapter returns the final flow and sigmoid blending mask before the
last low-resolution colour warp. Original weights are unchanged. The colour
merge follows the model's two-warp/mask rule. Model code retains its MIT license.

## One shared portable runtime

In the laboratory the runtime lives at `ofxr-development/runtime/rife`:

```
runtime/rife/python/       # CPython + pinned Torch/CUDA dependencies
runtime/rife/models/       # three original state_dict weight files
runtime/rife/app/          # persistent worker and model code
runtime/rife/runtime.json # download provenance
```

The tray resolves `runtime/rife` from its original package and passes that
absolute path through its owned generated runtime INI. The staged AppData DLL
uses that locator (v734); direct laboratory harnesses can also search their
parent directories. Keep the tray's test build under the laboratory tree.
The build ZIP does not duplicate the interpreter, Torch or weights. A system
Python or installed CUDA toolkit is not required; a compatible NVIDIA driver
is required. The process uses an isolated Python environment and a restricted
DLL search path, with no game-local Python/CUDA libraries.

For a fresh laboratory, run `tools/Setup-RifeRuntime.ps1 -Root <lab>/runtime/rife`
once. It bootstraps the embedded interpreter and verifies every pinned download
before extraction; no global Python/pip/CUDA installation is modified. To create
a new portable folder from scratch, also provide `-CrtSource` pointing at the
pinned x64 Microsoft.VC143.CRT redistributable folder; the prepared laboratory
runtime already contains these app-local DLLs and their cache. Copying the
complete prepared runtime to another PC does not require Visual Studio or an
installed VC runtime. Do not replace shared worker files during a RIFE test.

One persistent worker/model is shared by all RIFE synthesizers on the matching
adapter LUID. In the earlier path both eyes execute serially with the same weights and independent
same-eye temporal inputs. CUDA Graphs avoid repeated small Torch launches.
CPU pipes carry commands and completion receipts, never image pixels. Planar
FP16 inputs and five-plane flow/mask outputs use shared D3D12 buffers imported
by CUDA. Exact input/output receipts protect ownership. Reply IPC is event
driven; native A/B packing uses one submission. The route adds no XR swapchains
and no pacing policy; the existing selected framework/director owns delivery.

First use/model or shape changes warm up and capture a CUDA graph and can pause
generation briefly. A failed worker stops the transaction instead of reporting
an unproduced frame or queuing a GPU wait on an unsignalled CUDA semaphore.

## Measurements and limits

The laboratory test records D3D12 timestamps from input preparation through
CUDA inference and native-colour composition. It includes IPC/CPU scheduling
gaps inside that generation transaction, and excludes OpenXR presentation.
The GPU test also verifies stereo isolation, unchanged real frames, actual
temporal interpolation, and native one-pixel texture detail beyond the motion
grid. No game/headset quality or performance result is implied by these tests.

## Asynchronous native D3D12 submission

On a private native D3D12 endpoint with a separate synthesis queue, the bridge
freezes input resources, camera metadata and producer dependencies into a bounded
FIFO. The caller receives a pending ticket immediately; a CPU worker runs input
preparation, serial CUDA inference and native composition. The ordinary GPU
completion fence still guards delivery, source retirement and output reuse.
A full buffer applies backpressure. Shared-queue, legacy and D3D11/Vulkan interop
paths retain synchronous submission. No framework/director pacing is changed.
On healthy-device inference failure, the pending transaction transports a valid
real image and a current-image reprojection with a non-generated receipt; it does
not signal completion for work that never ran. Shutdown drains queued jobs first.

## VRAM accounting

The percentage and benchmark memory include game-process DXGI local usage plus
the shared Python worker's private DXGI local usage on the same adapter. Worker
usage includes CUDA context/model/cache overhead, not just PyTorch tensors.
The worker measures the budget charge of imported D3D12 buffers at configuration
and subtracts that charge because those allocations are already in the game.
PyTorch reserved memory is a subset shown in the tooltip, never added twice.
One cached worker sample serves all generators on that adapter, at most once per
second while UI/log/benchmark telemetry requests it. Unknown worker usage is N/A,
not a misleading game-only total. This is committed dedicated usage against the
DXGI budget, not an end-to-end GPU residency or compositor memory measurement.


## Faster model trial

The two newer Lite choices are retired from this build's menu. Persisted IDs2,
3 and4 mean4.6,4.4 and4.3; default remains4.6 at25%. Old Lite preferences map
to4.6. The shared worker retains legacy IDs0/1 for earlier sealed builds.
The two original new weight files and upstream architectures are pinned in
runtime-lock.json from HolyWu/vs-rife commit3488617283db7c428a83ba4a19382285da698b6a.
Only the same final flow/mask adapter used by4.6 is applied. Weights are unchanged.

Initial RTX4090 CUDA-event test, two eyes sequentially,3700x3400 native with
925x850 inference (25%):4.6 median5.66ms,4.4 4.59ms,4.3 4.65ms over120samples
per model in three rotated rounds. At20% (740x680):4.6 4.28ms,4.4 3.83ms,
4.3 3.86ms. This measures inference/graph/input-output tensor copies only;
D3D12 preparation, full-resolution composition, IPC, game load and headset
presentation are excluded. Not an in-game FPS or visual-quality guarantee.
New models need in-game quality comparison, especially fine occlusion edges.

Complete native-colour D3D12 transaction at3700x3400 per eye (five warmed samples,
median including pack/inference/composition/IPC, excluding game/OpenXR):

| Model | 25% | 20% |
| --- | ---: | ---: |
| 4.6 | 7.21ms | 6.22ms |
| 4.4 | 5.92ms | 5.46ms |
| 4.3 | 6.42ms | 5.63ms |

Background GPU/scheduler variation is present (one4.6/20% sample16.13ms).
These are small laboratory comparisons, not a guaranteed headset FPS gain.
All six configurations retain native one-pixel colour detail in the GPU fixture;
finer motion and occlusion accuracy still depend on model and inference grid.


## Experimental multi-frame generation (v737)

The RIFE multiplier is independent of the old Djules3X preference.3X computes
fractions1/3 and2/3;4X computes1/4,1/2 and3/4. A single persistent model and its
buffers are reused serially across fractions and eyes. Each output has its own
native full-resolution image and target camera metadata. No repeated midpoint
is counted as a new intermediate. The final native GPU completion receipt and
per-output content mask guard the whole batch, including the real B copy.

The private worker delivers S1,S2,[S3],B in order to the same framework/director
endpoint; physical pacing and its buffer2/3 preference are unchanged. Source
admission applies backpressure when the logical output queue is full.3X needs
two distinct S destinations and4X three, with both real-image slots retained
also in Fast mode. The4X logical descriptor buffer holds A/S1/S2/S3/B; it is
not an additional compositor-pacing queue or a duplicate model allocation.
Switching multiplier uses the existing drain/reconfigure/output-retirement
transaction. V738 explicitly promotes3X/4X to the ordered generation worker
once the initial real prime has closed its XR cycle. It does not wait for the
inline2X pair heuristic, whose multi-output safety guard refuses that pair.
This applies both at startup and after a live multiplier change. Disabled FG and AFW-only bypass still ignore a saved multiplier.

Whole-batch GPU timing starts before the first input pack and ends after the
last native composition. On a healthy-device inference error all outputs fall
back to current-colour reprojection, with zero generated bits, and the real
frame remains valid. The2X colour/model/pacing contract is unchanged.

Tests cover3X/4X on all three models in synchronous and asynchronous submission,
exact1/6 grids, one1/3 case, temporal order/distinct pixels, real copies,
repeated captures, invalid destination aliasing, receipts and reconfigure.
3700x3400per-eye3X/4X with4.4/1/6 also passes. Actual layer CPU tests verify
multiplier clocks, Fast on/off pool sizes, per-output delivery indices and
isolation from ordinary OFXR; the logical queue verifies ordered4X batches.
No in-game FPS or latency improvement is guaranteed by these laboratory tests.


V739 also renews expired native OpenXR cycles in Framework bypass. A slow GPU
preparation or sparse input can outlive the Wait already in flight. The same
owned image is retained while Wait/Begin obtains a live cycle; Framework does
not acquire the director's phase, hold, or delivery pacing. The directed path
and RIFE model/interpolation code are unchanged. This is a native-cycle
correctness fix for private Framework delivery, including ordinary engines;
legacy native delivery is outside this endpoint.


V740 is a diagnostic trial for a separate upstream worker stop. With F9
logging enabled, the first terminal synthesis-worker refusal or exception
writes `ofxr-worker-failure-pid*.txt` beside the runtime log, plus a frozen
`.flight.log` copy. Stage, source pair, prepare reason/HRESULT, native result,
output handles and buffer capacity survive later log wrapping. Normal output,
models and pacing are unchanged. No failure journal or snapshot is written
with diagnostics off. V739 native-cycle renewal is retained; the real4X
worker failure is not declared fixed by this diagnostic build.


V741 fixes the RIFE3X/4X model timestep selection. Irregular application XR
predictions previously changed the intermediate fractions. Near a34ms source
gap at90Hz,4X could request a first fraction below0.05; the generic sanitizer
replaced it with0.5, crossing the second fraction and causing E_INVALIDARG
before GPU work. RIFE multi-output now always samples1/3,2/3 or1/4,1/2,3/4.
Provider validity checks remain intact. This does not change the selected
inference resolution, including1/6, full-resolution colour, RIFE2X, other
engines, physical pacing or buffer counts. V740 first-error capture is retained.


V742 releases a completed RIFE3X/4X real B without waiting for another
application source in Framework mode. RIFE already owns the previous source
in its synthesizer history; retaining the output descriptor is redundant.
The detached source-sequence anchor preserves the next interpolation pair
while the ready S1/S2/[S3]/B batch retires in order. The existing admission
wait still prevents overlapping writes to owned output images.

No extra private images, XR swapchains, model copies, GPU work or pacing
queues are added. Director already uses this release policy, so its timing
is unchanged. RIFE2X, ordinary engines, legacy/interop and AFW are unchanged;
spatial inference1/6 and the fixed V741 temporal fractions remain intact.

The actual-layer GPU test pauses new source submission midstream and verifies
that every ready output, including real B, reaches native EndFrame without a
drain/control command. Resuming verifies temporal order; live2->3->4->3->2
transitions remain covered. Repeated3736x3424-per-eye4.4/1/6 comparisons with
a mocked90Hz native consumer reduce source-End-to-native-End median latency
by about13ms in3X and19ms in4X, with equal output counts and private VRAM.
Those are component test results; game/headset confirmation remains pending.


## V744 changes

Luke3X/4X interpolate the already completed post-AFW color pair on the isolated
synthesis lane. AFW still evaluates once per fresh source, not per generated
intermediate. Each S output has a separate destination and ownership lease;
3X/4X/2X switches drain and rebuild the D3D11 output shape before reuse. Legacy,
shared-queue, single-threaded D3D11 and split-loop safety gates remain intact.
The worker's model timesteps and the native-resolution color composition are
unchanged. Tests exercise Framework/Director x Normal/Fast and2->3->4->3->2,
including live10/15/23/40% model shapes. The AFW ingress GPU fixture uses a
completed pass-through AFW packet; actual in-game warp quality needs testing.

The RIFE slider has its own persisted `rife_resolution` field, independent of
the old optical-flow scale enum. It defaults to15%, clamps to10..40 and commits
when released, so dragging does not repeatedly rebuild history/model shapes.
The raw internal API retains its old exact-fraction fallback for earlier GPU
fixtures; it is not the current UI. Native output dimensions never change.

Increase buffer remains2->3 deliveries. With Director and a valid physical
clock, it also filters small observer phase jitter instead of reanchoring to
every noisy sample. The correction is bounded to25us per new physical counter;
real refresh/large-phase changes reanchor normally. No extra pair, service-time
reserve, fixed latency wait or fourth delivery is added. Framework bypass and
native VDXR Legacy pacing remain unchanged. Actual variable game/GPU time is
still measured; this does not force the latency meter to display a fixed value.
