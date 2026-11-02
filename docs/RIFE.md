# Experimental RIFE route (v736)

The Frame generation menu includes RIFE between OFXR and Hybrid. Model choices
are 4.6, 4.4 and 4.3. The initial trial selects 4.6 with 25% motion
inference when entering its new profile. Existing OFXR defaults are unchanged.

This is a NVIDIA CUDA / PyTorch experiment, not TensorRT and not an AMD backend.
No DLSS motion vectors or depth are needed. RIFE is 2X only. Benchmarks continue
to test the existing accepted routes; this experiment is selected manually.

## Native-resolution colour

Each eye's two temporal images are first projected into the synthetic target
camera and sampled at the chosen motion-inference resolution. RIFE predicts
the forward/backward sampling displacements and blending mask at that size.
The D3D12 output shader applies those fields to the original full-resolution
colour images, including their alpha. It does **not** enlarge a low-resolution
RGB generated frame. The real frame is copied unchanged. 20% is also available (per axis); full-resolution output is retained. Lower inference
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
adapter LUID. Both eyes execute serially with the same weights and independent
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
