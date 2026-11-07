# V907: current motion on trusted visible foreground edges

Owner V906 improves movement substantially, but quiver still flickers. ActualF8
Tsushima PID23708,20261107-153456-260-1,normal OSR+OFXR2x,depth dilation0.
Interior quiver lookup99%accepted,RGB replay99.8-100%matches output; position within
about one pixel. Thin edges still categorically excluded current motion in V906.

V907 removes only that blanket exclusion for TRUSTED VISIBLE FOREGROUND edges:
- Stereo surface agrees with nearest local depth; background side is excluded.
- Existing projected own-eye history has no nearer surface to preserve.
- ALL FOUR current depth taps under lookup agree with expected surface, AND
  ALL FOUR own-history depth taps agree with expected intervening depth.
- Existing current vector, native depth, temporal/identity, scene and bounds
  checks remain. Same3% correspondence tolerance, no widening.

The same V906 per-point current vector transform then samples previous OWN-eye
colour. No guide/colour interpolation, new sampler, motion threshold, dilation,
pose/pacing/FG/RIFE/NR/AFW/UI changes. No new resources/dispatches/waits/readbacks.
Interiors and genuine occluded/ambiguous/background edges retain V906 behavior.
All extra depth checks are confined to the fringe candidate path.

CPU replay trials of bilinear guides did not provide a consistent improvement,
so they were NOT imported. No previous/current vector delta is introduced.
Conservative edge trial improves selected high-contrast shaft-edge error against
current-other-eye geometric reference; some small patches do not improve. Such
cross-eye comparison is NOT temporal/headset proof. Actual baseline/modified GPU
replay and focused guards are retained locally. Native older jitter at F8 time
was absent from rolled flight data; inferred from78repetitions of same jitter
pair in same run, then checked against actual output. Window fraction constant
in629recorded events, FOV stable. See evidence for exact limitations.

Tests: stale-history moving foreground edge must use CURRENT vectors/PREVIOUS
colour; one contradictory depth neighbour and nearer own-eye geometry must retain
legacy edge behavior, both eyes and qualities. Earlier48current-field and history/
confidence/scene/screen invariants preserved. Local trial; headset acceptance pending.
