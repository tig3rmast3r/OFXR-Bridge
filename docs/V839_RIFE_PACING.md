# V839: RIFE post-OSR execution and model selection

## Observed report

The retained V837 GTA V Enhanced benchmark has two RIFE Framework/Buffer
segments at 75.28 and 74.48 output FPS; subsequent OFXR Framework/Buffer
segments reach 90.12 and 89.83. These are separate observations, not a controlled
scene replay. The circular detailed tail retains RIFE only. Its inference phase
averages 0.851 ms; pack completion waits reach 18.976 ms (95th percentile 16.700).
The reported low GPU utilization does not establish absence of fence/CPU waits.
The telemetry generation stage includes pipeline delay, not only inference.

## Change

Luke's isolated D3D11 post-reconstruction path now uses the existing asynchronous
RIFE execution machinery on distinct per-eye D3D12 queues. Enqueueing RIFE no
longer CPU-waits for pack/inference/compose while holding the session GPU mutex.
Both eyes can be admitted before the stereo completion owner waits. The capture
and OSR ingress helper remains on the original capture lane. Exact history and
output-reuse dependencies are retained: interop reuse protection is signalled
on its lane and captured into the async job's input fence snapshot.

Framework RIFE 2X now admits the existing post-AFW synthesis worker, as 3X/4X
already did. It retains Framework delivery policy. Ordinary OFXR Framework and
Director timing policy, shaders, OSR pose/scene detection, NR and RIFE inference
code/weights are unchanged. No extra queue latency or priority boost is added.
Startup/inline priming waits its exact completion outside the producer GPU mutex
and uses publish_completed; it never announces output before the async producer.
The normal stereo completion path still waits both eyes and their D3D11 copies
before releasing either output to the runtime.

## Models

Basic mode always selects 4.4, including loading old settings without opening the
UI, and hides the model dropdown. Advanced mode shows only installed compatible
versions found in the selected runtime's models/flownet_v*.pkl files, refreshed
when opening the menu. IDs remain the reviewed worker protocol: 4.6, 4.4 and 4.3.
Unknown networks and retired Lite networks cannot be run by this worker and are
not offered. No arbitrary downloaded Python is imported. The sole automatic
runtime remains managed AppData; only the existing explicit development INI
override changes the model catalog root. No filesystem search is added.

## Validation limits

Focused GPU fixtures exercise real RIFE with both Luke Framework/Director and
2X/3X/4X, exact output ordering and fence ownership. A deliberately blocked
synthesis lane verifies that application End returns and the producer GPU mutex
remains available while the GPU dependency is withheld. A passing fixture is
not a claim that the reported GTA scene now holds 90 FPS: a game retest with
this build is required for that measurement.
