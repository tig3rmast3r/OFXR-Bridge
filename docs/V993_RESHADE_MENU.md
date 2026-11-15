# V993: ReShade menu rendering

Menu draw and fence retirement compare the exact underlying device identity
through the existing ReShade/Streamline unwrapping helper. Rendering still uses
the original wrapped objects; foreign-device checks remain enforced. Menu
failure details persist in UI progress diagnostics. Permanent WARP alias tests
cover actual menu drawing and submission receipts. No Independent pacing,
metrics, IPC-layout or queue changes are included.

Ported from 173a2b5e and 885730e9 (Independent V1132).
