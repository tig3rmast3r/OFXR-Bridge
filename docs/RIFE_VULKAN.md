# Experimental Radeon Vulkan RIFE

This optional backend targets Radeon on Windows x64. Local validation on NVIDIA
Vulkan does not establish Radeon, game or headset acceptance. NVIDIA's existing
CUDA/TensorRT worker remains its normal backend. Select **AMD Radeon - Vulkan
(experimental)** in the RIFE installation dialog when the package includes the
sealed `rife-vulkan` payload. The complete V957 ZIP includes it: **RIFE -> Reinstall
RIFE... -> AMD Radeon - Vulkan (experimental)** selects that installer; NVIDIA is
the other choice in the same dialog. The first model is **RIFE 4.4** only.

The worker is native C++/ncnn/Vulkan. Portable CPU-only PyTorch and pnnx are used
by an isolated offline compiler, not for live inference. The installer needs
2 GB free for staging; per-resolution caches need additional space. It validates
a staged runtime before activation and retains the previous runtime. AppData is
the automatic runtime location for the tray and games. Installation does not
require editing any INI or selecting a folder. No global Python, PATH, SDK or driver changes are made.

## Device-specific preparation

**RIFE -> Prepare Vulkan for found games...** uses the existing recorded game
resolutions and eye grouping. Vulkan working dimensions round up to multiples of
32 so its planar FP16 shared buffers have aligned channel strides. A new shape
can also be prepared on first use, before inference starts; this can take time.
Offline preparation while the game is closed is recommended.

From V952, a missing or outdated plan starts an owned background compiler. Until
it is ready, both inline and asynchronous submission paths transport the real
image, report zero newly generated outputs and retry without latching a worker
failure. Switching quality or 2X/3X/4X may need a different shape; abandoning an
unfinished preparation closes only its owned helper processes. Cached plans
still need an initial model/pipeline load, which can cause a short hitch; offline
preparation reduces cold work but is not a guarantee of stutter-free loading.

V953 requires its matching native worker and compiler. Install or reinstall through the matching tray dialog; replacing only the
tray/layer cannot update an older worker/compiler. V957 keeps the V953 worker
and compiler, plus the V955 pacing behavior tested on Nobody.

Preparation exports the same pinned 4.4 weights and flow/mask contract, fuses
convolution/activation patterns through pnnx and replaces six image warps with
FP32-coordinate fused Vulkan sampling. It tests FP32, FP16 storage, FP16
arithmetic and cooperative-matrix profiles when supported. The fastest measured
profile that passes numerical checks is retained. No matrix path is required:
ordinary Vulkan shaders remain a candidate on devices without it.

V953 adds three mixed-precision graph policies: reduced precision in all feature
convolutions (`features`), only the last two refinement blocks (`refine`), or only
the last block (`final`). Deconvolution heads, flow/mask arithmetic, interpolation,
warping, inputs/timestep and final output remain FP32. Explicit GPU Cast nodes
bridge storage boundaries; per-layer feature masks select the corresponding
shaders. Weight-bearing nodes retain their original order and share the pinned
weights. Each policy competes in storage16, FP16 arithmetic and matrix mode where
the GPU supports them, under the same numerical gates as the old profiles.
Every timed candidate receives at least 100 ms of GPU warm-up before its samples.

V953 also loads the selected model/pipelines in the background. Pending frames
keep the real-image fallback until the model is resident, including a cached
plan's first use; shader/model loading no longer holds its frame receipt.

`plan.json` records every candidate, accuracy/timing, the selected profile and
the mixed-precision map. F9 `rife_backend` receipts retain the low flag bits and
add the actual executing profile in bits8..15 (`(result >> 8) & 255`):1 FP32,
2 storage16,3 FP16,4 matrix;5/6/7 mixed-features storage16/FP16/matrix;
8/9/10 mixed-refine;11/12/13 mixed-final. This ties game FPS reports to actual
execution rather than merely a prepared cache. Radeon tunes locally; RTX4090
results do not establish RDNA gains or VR quality.

Plans include GPU UUID, vendor/device, driver and pipeline-cache identity;
model/network/warp/compiler/worker hashes; resolution and eye count. Source or
device changes require matching preparation. Pipeline caches reduce shader
compilation work; measured profile selection and fused operations affect steady
inference. This is not a TensorRT engine or a claim of equal performance.

The exported ncnn CPU graph is also checked against the original model. Vulkan
FP32 must agree within 0.1 pixels maximum flow error and 0.005 maximum mask error;
floating-point convolution and fused pixel-coordinate sampling can differ from
Torch's operation order. Candidate limits are mean/max flow 0.03/0.5 pixels
and mean/max mask 0.003/0.03, with finite outputs and a bounded mask. Preparation
checks fractions 0.25, 0.5 and 0.75. These fixtures do not replace visual review.

## Runtime contract and retained optimizations

The existing D3D12 pack/native-colour composition, banked fractional outputs,
per-fraction cameras and asynchronous caller scheduling are reused. The native
worker imports the exact D3D12 adapter's buffers by LUID, checks import support
and memory type compatibility, and records explicit external ownership barriers.
All eyes/fractions in a request share one Vulkan submission/completion receipt.
No CPU image upload/download or disk image exchange occurs in live inference.
Intermediate allocations use reusable GPU pools. Timestep filling and fused
warping stay on the GPU. In-place ncnn layers use private GPU input copies.

CUDA Graph replay and PyTorch channels-last are specific to the NVIDIA route;
this backend uses Vulkan command recording, ncnn packing/fusions and local
profile selection instead. It currently records a fresh command buffer per
transaction and dispatches independent eyes within that submission, rather than
claiming CUDA Graph replay or a single batched convolution. Private VRAM telemetry
is unavailable rather than fabricated. Unsupported graph operations fail instead
of silently copying image tensors to CPU.

## Headset-free Radeon test

Extract the complete OFXR ZIP and choose **RIFE -> Reinstall RIFE... ->
AMD Radeon - Vulkan (experimental) -> Install**. The same choice is offered
when **Check RIFE...** finds a missing runtime. Installation, checks, game
inference and preparation all use `%LOCALAPPDATA%/ofxr-bridge/RIFE/runtime`.
No separate ZIP, switch script or folder override is required. The Vulkan
installer retains an existing runtime automatically.

After installation, close GPU applications and run:

```powershell
.\tools\Test-RifeVulkan.ps1 -ResultJson "$env:USERPROFILE\Desktop\Radeon-RIFE.json"
```

This prepares a small graph and exercises real D3D12 shared buffers, both eyes
with reversed input pairs, and three distinct fractional output banks. The report
includes GPU/driver identity, selected profile and numerical errors. Optional
`-Width 512 -Height 288` checks another working shape. Send the resulting report
for review. A subsequent game/headset trial is still required for VR acceptance;
one Radeon result covers that tested GPU/driver combination.

## Build the optional payload

```powershell
python tools/prepare_rife_vulkan_deps.py --deps D:/ofxr-deps/vulkan-rife
cmake -S native/rife_vulkan -B build-vulkan -A x64 -DOFXR_VULKAN_DEPS=D:/ofxr-deps/vulkan-rife
cmake --build build-vulkan --config Release --target ofxr_rife_vulkan
python tools/package_rife_vulkan.py --build build-vulkan --deps D:/ofxr-deps/vulkan-rife --output rife-vulkan
```

Dependencies, CPU Torch, pnnx and the small ncnn Win32 external-memory patch are
pinned. The payload helper refuses overwrites. From V951, `Package-RifeBuild.ps1`
requires the sealed payload and verifies its files, source hashes and dependency
pins. Backend changes require rebuilding/resealing before a new distribution can
be packaged. The default output is one complete OFXR ZIP with the integrated tray installer,
licenses and compiler tools, without model weights, Python/Torch runtimes or
GPU caches. The optional -StandaloneInstaller packaging switch also creates an
auxiliary installer from that same sealed stage when explicitly requested.
