# V837: AppData-only RIFE runtime

RIFE defaults exclusively to `%LOCALAPPDATA%/ofxr-bridge/RIFE/runtime`.
The tray no longer injects an automatically discovered runtime path. The DLL
never walks package/ancestor directories, and the PowerShell Check helper
never substitutes a development or bundled runtime for a missing AppData copy.
This makes a missing managed installation visible to the normal install dialog.

A developer can manually set `rife_runtime_dir` in `[tray]` of the global
`ofxr-bridge.ini`. The tray persists that explicit value and passes it to the
runtime `[ofxr]` INI. Empty selects AppData; invalid/relative paths fail explicitly.
Check RIFE remains dedicated to AppData and does not consult this override.
Existing development runtimes are preserved; no migration or reinstall is
performed by starting the tray.

The build changes discovery/INI transport only. Python/CUDA/TensorRT pins,
model weights, the 17-file same-release overlay, image generation, pacing and NR
remain unchanged. Focused checks cover missing/incomplete managed runtime,
ignored ancestor/package copies, manual override/INI persistence, paths with
spaces and actual PowerShell missing-runtime detection inside a development tree.
