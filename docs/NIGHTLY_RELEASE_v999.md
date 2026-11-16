# V999 ? MSFS 2024 Neural Rendering with ReShade

Neural Rendering now uses the same underlying command-list identity as the native queue observer, including ReShade and nested ReShade/Streamline proxies. This addresses an NR initialization stall where every following DLSS call was skipped as still awaiting submission.

Exact submission fences remain required before reusing NR state. No pacing, frame generation or glass-cockpit rendering behavior is changed.

Restart the game with this build, retain ReShade and the Glass Cockpit Fix, and enable Neural Rendering. This build still requires the separately supplied NVIDIA NR runtime DLL. In-game MSFS acceptance is pending; the regression test covers four NR histories across repeated native submissions and malformed proxy chains.
