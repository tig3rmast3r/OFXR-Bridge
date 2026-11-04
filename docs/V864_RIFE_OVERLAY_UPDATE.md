# V864 — align installed RIFE source files with the current tray

V863's reported six failures happened before compilation: the installed source
overlay still contained the 180-second compiler deadline. Its preflight also
compared raw release bytes with the installer's normalized LF bytes. An engine
prepared from a Windows checkout could therefore have a different cache identity
from the identical installed network, preventing in-game reuse.

## Startup and maintenance

Every normal tray launch now checks the installed managed RIFE source overlay
before auto-arming. It aligns the complete same-release 17-file manifest when
needed, without downloading or replacing Python, PyTorch, TensorRT or weights.
Already identical files are left alone. No GPU inference runs during this startup
step. A missing runtime is skipped quietly; a dependency-pin mismatch requires
the existing explicit repair/install path. In-use stale code is not overwritten.

Check RIFE also aligns the overlay before its health check. Pre-compilation does
the same as a single global preflight, before counting individual engine jobs.
An environment/source error is no longer counted as six failed compilations.
The precompiler uses the installed `runtime/app` files for its model/compiler
fingerprint, matching the worker in the game. All installed overlay files use
canonical LF endings, independent of the release checkout's line endings.
An isolated-mode CLI regression test also covers loading the sibling update
helper. A stored Windows adapter LUID may change after reboot; a single available
CUDA adapter is selected unambiguously, while ambiguous multi-GPU records require
the game to be observed again.

## Atomic code-only updates

The updater validates dependency pins and every release-file hash, stages only
the source overlay and atomically exchanges its directory under the runtime's
exclusive lease. App files and receipt roll back together on failure. A bounded
recovery journal handles interrupted updates; no game/process is terminated.
Libraries, model weights, recorded game resolutions and existing engines remain.

TensorRT libraries are independent of the source overlay. Compiled engines depend
on the cache's network/weights/warp/compiler hashes, shape, batch layout, device,
backend versions and adapter contract. A changed dependency produces a new key;
unrelated helper/telemetry changes do not invalidate compatible engine entries.
The compiler-file hash is conservative and may rebuild after non-mathematical
compiler changes. The updater does not empty the engine cache at tray startup.

Focused tests cover old/aligned/in-use overlays, mixed line endings, corrupt
release files, dependency mismatch, missing runtime, failed promotion/receipt
updates, interrupted recovery, the tray's startup-only helper mode and exact
precompiler use of installed files. This update does not change frame pacing.
