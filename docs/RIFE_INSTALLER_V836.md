# V836: pinned RIFE setup and tray check

Based on sealed V835. This is a local test build, not a public release update.
It adds desktop tray **Check RIFE...** and one shared per-user NVIDIA runtime
at `%LOCALAPPDATA%/ofxr-bridge/RIFE/runtime`. AMD Radeon (RDNA) is displayed but
disabled; no AMD/Intel implementation is included.

The check is asynchronous and uses the package's PowerShell entry point without
console interaction. It checks receipt/pins, adapter files, CRT/model hashes,
Python/Torch/CUDA imports and a small actual RIFE 4.4 CUDA inference. Ready,
missing, repair, unsupported and busy are distinct results. Optional TensorRT
without compiled engines is normal. Busy/OOM/unsupported/invalid JSON/timeouts
cannot offer installation. Only an explicit confirmation after missing/repair
starts the visible unattended console. Existing shared developer runtimes and
the owned temporary INI locator remain supported; managed AppData takes priority.

Python 3.12.10, Torch 2.9.1+cu128, CUDA bindings 12.9.10, TensorRT 10.14.1.48,
weights and all transitive download URLs/byte counts/SHA256 remain pinned. A
new lock entry identifies the exact HolyWu/vs-rife base source archive. Setup
verifies that archive, maps the required model files, then overwrites them with
all 17 OFXR adapter/model/lock files shipped in the same package. A canonical
LF manifest checks the final complete overlay; the upstream VapourSynth API
initializer is replaced by the existing empty OFXR model initializer.

The package includes the ten pinned app-local VC143 CRT files and the original
Microsoft license from the matching 14.44.35112 redistributable. No user-supplied
CRT folder, global Python/pip, PATH changes, driver installation or elevation
is required. Around 8 GB is installed; 14 GB free is required for staging and
downloads. Engine caches remain separate and can grow. Successful setup removes
its own default download cache; explicit developer caches are retained.

Native workers/checks hold shared Windows file leases; installation requires the
same runtime exclusively. New files are verified in staging before promotion.
A persistent journal recovers interrupted promotion, and failures retain the
previous runtime. Existing games are never terminated. Logs and structured
UTF-8 JSON results remain available. Root overrides support isolated tests;
normal AppData activation was not performed on the development machine.

## Focused verification

Five CTests passed: actual tray dialog/process/typed-result handling; managed,
legacy and explicit INI discovery; actual Windows shared/exclusive leases;
19 installer/backend scenarios; and eight existing setup scenarios, using the
pinned Python 3.12.10 interpreter. Cases cover bad hashes/overlay/path traversal,
network/health/activation failures, interrupted journal states, busy/unsupported,
space preflight, Unicode paths, strict JSON and unattended command arguments.

One clean isolated AppData installation completed from verified pinned archives,
including one new TensorRT download. After final check/recovery refinements,
the final same-release overlay and receipt were refreshed in that isolated
installation and all 17 files were verified again. Final real CUDA inference
and TensorRT version detection succeeded on RTX 4090; the exclusive gate also
returned the expected busy status. No full game/headset/pacing matrix was run.
The existing shared runtime/models/dependencies were preserved.

Version/control 836/83601: restart the old game/tray before trying this package.
The V835 released fixes and other generation/pacing/NR behaviour remain intact;
only runtime discovery and a worker-lifetime read lease change the RIFE launcher.
New local binaries require separate signing.
