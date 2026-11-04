# V863 — recorded game resolutions and offline TensorRT preparation

The tray now groups three actions in **RIFE**:

- **Check RIFE…** retains the existing asynchronous health check/repair offer.
- **Reinstall RIFE…** confirms a clean installation, downloads the same pinned
  dependencies again and replaces the runtime with a newly validated stage.
  Successful reinstall clears the managed TensorRT engine cache. Game records
  and diagnostic logs remain. An active runtime blocks replacement; no game is
  terminated. A failed staged installation preserves the previous runtime.
- **Pre-compile TensorRT for found games…** opens a progress window with an
  advancing combination counter and Cancel/Close. Run it with the VR game closed.

## What is remembered

Every game using the layer records its last valid submitted primary projection
layout in `%LOCALAPPDATA%\ofxr-bridge\RIFE\games`. There is one small JSON file
per executable path, replaced on the first valid frame or a dimension/eye-layout
change. Unchanged frames do not write, hash files, query the GPU or read pixels.
Logging and RIFE need not be enabled. Empty frames do not erase the last record.
There is no image capture, desktop/process scan, frame log or expanding list of
old resolutions. No old logs or TensorRT entries are used to guess source sizes.

The record contains the actual per-eye viewport dimensions, the one/two-view
transaction layout and the rendering adapter LUID when available. Packed
side-by-side eyes use eye rectangles, not the enclosing texture size. Quad views
retain distinct peripheral/focus shapes in the same latest record. Overlay quad
panels are excluded. Games must be opened once with this build to populate it.

## Offline preparation

The offline helper reads only that folder and prepares all six Quality presets
for RIFE 4.4. It uses the same float32 size calculation, minimum extent, model,
compiler fingerprint, device identity and cache validation as the in-game worker.
It prepares only the recorded eye count. Identical shapes shared by games or
presets are deduplicated. 2X/3X/4X, vectors and Framework/Director do not require
separate engines. Existing valid engines are checked/reused rather than rebuilt.

An explicit offline attempt retries a failed in-game compilation despite its
one-hour negative-cache backoff. Each new engine retains the 300-second compiler
limit and is verified with a real small CUDA inference at its compiled shape.
Failure is logged and the remaining combinations are still attempted; the final
result reports failures. Cancel stops only this helper's builder and preserves
completed engines. Parent-death watchdogs prevent hidden orphan compilers.
The runtime lease excludes other offline runs, installers and active managed
RIFE workers. Neither models nor dependencies are downloaded by pre-compilation.

Normal in-game compilation and safe PyTorch fallback remain available for new
or changed resolutions. Errors retain V862's red meter status. This update makes
compilation reproducible outside the game; it does not change frame pacing or
claim to fix the remaining RIFE 3X FPS plateau. Product version remains 0.3.3.

## Focused validation

Tests cover independent/shared/packed/quad view records, latest-only replacement,
change-only writes, all six preset sizes, deduplication, invalid records, device
matching, failed-cache retry, cancellation, cached reuse and partial failures.
The actual Windows progress form is exercised for counter/bar completion and
cooperative cancellation. Installer fixtures cover clean replacement/cache reset,
game-record retention and preservation on failed validation. An isolated NVIDIA
run verifies a previously failed synthetic stereo engine can compile, infer and
be reused without recompilation. No broad game/pacing matrix is run.
