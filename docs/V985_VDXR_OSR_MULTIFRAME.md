# Local V985 trial: Native VDXR OSR/AFW 3X/4X

Historical local acceptance record. This extension is consolidated with the
other accepted native multiframe fixes in [nightly V984](V984_NATIVE_MULTIFRAME.md).
The V985 references below identify local test builds, not a public version.

Builds on the accepted V984 ordinary VDXR change. The OSR extension is a
separate change; its accepted V984 r1 test package took no test-only version.

## Change

Native VDXR can admit Luke Ross OSR/AFW D3D11 sessions into the existing
ordered multi-output worker when the application frame loop is unsplit, the
D3D11 device supports multithreaded use, and the isolated Luke synthesis queue
is available. Ordinary D3D11 games, split Luke loops, single-threaded D3D11
devices and AFW-only sessions retain their restrictions.

Opening the gate alone was insufficient. The GPU negative control produced
the right output counts but only the first synthetic image was copied from
D3D12 into the native D3D11 output. S2/S3 were black or held a prior batch.
The worker now publishes every generated D3D11 image and retains its exact
publication receipt. It waits those receipts before releasing outputs and
supplies the corresponding completed-image proof to a private endpoint when
applicable. Shared D3D11 multi-output workers benefit from the same correction.
The 2X path has no extra publication; the V984 native RIFE 2X overlap rule is
retained. AER framegen remains a separate 2X-only mode that bypasses OSR/AFW.
FSR and 0.2.13 retain their existing multiplier choices. DLSS-G 4X still needs
GPU multi-frame generation support and valid guides.

## Local verification

The extended GPU fixture feeds completed reconstructed stereo packets into the
real post-OSR worker and D3D11 publication path, poisons source metadata/images,
and reads back both eyes. It checks eight real packets and every generated
output, including the expected 1/3 and 2/3 fractions or 1/4, 1/2 and 3/4
fractions with NVIDIA optical flow. Native 2X is a control. FidelityFX 3X/4X
checks freshness of its scene-cut fallback on the fixture's changing histogram;
those cases do not claim distinct interpolated fractions. Capability checks retain the unsplit/device guards and native
RIFE 2X overlap admission. The fixture injects completed reconstruction
packets: it does not validate the OSR reconstruction algorithm or game guides.

The final focused run passed 48/48 in 74.26 seconds. Results and source hashes
are in BUILD_PROVENANCE.json. The earlier
native/SteamVR Framework matrix, ordinary private 3X/4X tests and existing
post-AFW 2X controls are included. On 2026-11-12 the owner confirmed that the
V984 OSR r1 package fixed the reported OSR + frame generation issue. This is
user acceptance, not independently measured scanout. No full-suite pass is
claimed; previous baseline
limitations in V984_VDXR_MULTIFRAME.md still apply. The unthrottled native
switch fixture also reproduced a missing/black real packet on the accepted
pre-OSR build; its isolated rerun is reported separately rather than counted
as an OSR regression.

## Accepted test configuration

Use this package's matching tray and runtime DLLs. Keep native VDXR/Auto and
OSR enabled in the same Cyberpunk scene. Leave AER framegen off. Start at 2X,
then test 3X and 4X for about 20 seconds each, and return to 2X. Check both eyes
for stale images, flicker, camera jumps and stalls. Repeat the available frame
generation engines without changing unrelated game or driver settings.

The accepted test logged to RuntimeLayer/v984; V985 logs to RuntimeLayer/v985; identify the run by PID and timestamp.
F9 windows and the flight log allow requested/actual multipliers and submission
rates to be compared. Native accepted submissions do not prove physical
headset scanout. No layer installation, arming, registry or game-settings
changes are made by this build process.
