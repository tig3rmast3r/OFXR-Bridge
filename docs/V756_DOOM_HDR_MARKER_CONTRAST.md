# V756 DOOM HDR marker contrast

Current owner F8: ofxr-capture-v23286-58496-20261029-224719-462-1.
Native color RGBA16F3344x3344, XR color BGRA83344x3064. The expected256x8 marker
at native(2,282) appears in the final eye at(2,142), exactly the140-row center crop.
The band survives geometrically but becomes nearly uniform: BGRA B=3, G=2..3,
R=0..3 on eye0. No token decodes anywhere in either full XR image, not merely in
the live ROI. Native HDR scene is exposed/graded into a much darker SDR image.
The current1/16 stamp loses its usable contrast. This is the observed current
block; it does not establish whether every historical regression had that cause.
Native F8 witnesses are before the marker write and are not after-mark evidence.

For DOOMTheDarkAges.exe only, use the existing full-white float pattern1.0 instead
of1/16. CRC/identity, band256x8, coordinates, readback ROI and Vulkan marker copy
rectangle stay unchanged. Reuses already-created bright upload patterns: no new
band/texture/copy region, waits, pacing, hook, AFW math or decoder relaxation.
Other titles retain their prior patterns (including existing Uncharted bright copy).

A WARP D3D12 GPU regression reads actual written FP16 cells: old1/16 fails the
new test;1.0 white/0 black passes, with exact16,384-byte marker/crop280/CRC identity.
Existing color-origin and GTA/Outlaws protected tests remain. This does NOT emulate
DOOM's actual tonemapping shader or prove AFW in the headset; owner must confirm.

Based on V755 recognition compatibility, V748 lineage; no experimental751-754
capture hooks or optional749 phase fix. No RIFE/game/mod/profile/registry changes
or publication. Local candidate, not declared game-verified.
