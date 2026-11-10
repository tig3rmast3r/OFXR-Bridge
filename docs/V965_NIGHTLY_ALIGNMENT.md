# V965: align Luke framegen trials and guide-publication recovery

Integrates local V963/V964 Luke AER provider selection, camera-based guide crop
and private NGX isolation with Djules's newer nightly V963 protected guide
publication recovery (82b077b3). That fix frees only completed registry-only old
records under snapshot pressure; external CPU/GPU readers and recent anchors
remain protected. Both V963 documents describe their respective development
branches; V965 is the combined build number.

The Luke/private-provider changes remain as in V964, while the shared publisher
and its pressure regression are taken unchanged from Djules. RIFE inference,
runtime overlay, pacing, model/compiler/cache and Radeon payload remain unchanged.
Shared-control ABI remains96201. No Luke configuration or eye swap is forced.

Validation covers actual private N1/N2 per-eye DLSS-G generation with zero OFXR
shared NGX init calls, ordinary core/capture/late-discovery, FSR/crop/token/history
and CPU/GPU snapshot ownership including pressure. Actual GTA stereo appearance,
mono-state recovery, repeated starts and framegen quality remain owner tests.
