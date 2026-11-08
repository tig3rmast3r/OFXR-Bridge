# V922: visible flow size and reduced benchmark matrix

Basic mode exposes optical flow size for FidelityFX/OFXR, including Hybrid's
flow engine. NVIDIA flow offers Slow and Medium. Fast flow quality and backward
calculation remain Advanced-only; Basic mode resets them to Medium and Off,
including inactive generator preferences loaded from older game configuration.
Visible flow sizes and Slow/Medium selections are retained. Other hidden
controls still use their existing enforced defaults. Advanced remains global.

The separate delivery Fast mode checkbox and Increase buffer remain available,
including their manual combination. The ordinary OFXR benchmark omits Fast plus
Buffer for all three optical routes in both Framework and Director. RIFE and
vector routes retain their combinations. Luke OSR cheap is a quality variant,
not the Fast flag, so its Buffer combinations remain.

RIFE benchmarks now use one Quality: the user's selection when the suite starts.
The same selection is used throughout every pacing combination, the conditional
3X tier and CSV exports. No quality sweep occurs. The existing optimization wait,
dynamic headset-refresh threshold and V921 winner policy remain unchanged.

RIFE adds Godlike at 36%, after Ultra; the default remains Medium at 18%.
Persistence, both UIs, inference scaling and offline TensorRT preparation support
it. Labels show names rather than percentages. Existing compiled shapes remain
valid; no runtime overlay or model change is needed.

Maximum trial counts including reference and possible 3X: SteamVR without vectors
35, complete59, Luke42; Legacy VDXR11/17/12 respectively. Conditional pruning or
session capability can reduce actual counts. Existing CSV summary metrics remain
unchanged; columns reflect the selected quality and reduced matrix.

Validation targets settings normalization/profile round trips, desktop/headset
widgets, benchmark matrix/controller/CSV and CPU-only precompile planning. No
new game, GPU performance or headset validation is implied.
