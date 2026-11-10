# V964: isolate Luke frame generation from shared NGX initialization

V963 owner testing reported mono output that persisted after disabling framegen
and required a rendering-mode switch in Luke. The earlier performance comparison
used different game resolutions; no RIFE/pacing change belongs to this fix.

Luke capability detection previously called process/device NGX initialization
under OFXR's project identity, even before DLSS-G was selected. Native feature
creation then used the shared core. This is a possible source of interference
with the mod's NGX/SR state; the exact cause of the mono state is not established.

For Luke, capability detection now uses the existing private signed FG snippet.
AER DLSS-G uses that same private route for create/evaluate/release. The snippet's
parameter maps implement the SDK's published virtual interface and own only OFXR
values, so no shared NGX initialization, capability map or parameter allocation
is required. The ordinary non-Luke core route and existing late-discovery recovery
are retained. Isolated and shared capability caches have separate keys.

The V963 camera-based crop, full-grid N1 composition, exact colour/guide identity
and selected-generator corrections remain. No forced eye swap or modification of
Luke settings is made. RIFE, model/overlay, pacing, FSR and Hybrid are unchanged.

CPU parameter type/ownership checks and real private DLSS-G N1/N2 per-eye generation
verify actual generated output with zero OFXR calls to shared NGX initialization.
That is not game/headset acceptance. Owner should test from a fresh game process,
first FSR/Hybrid, then DLSS-G with AER, F9 and an F8 while moving. Check that stereo
survives enable/disable and method changes without changing Luke's rendering mode.
