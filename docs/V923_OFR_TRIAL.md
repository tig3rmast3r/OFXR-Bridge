# V923: local OFR reconstruction trial

Base: sealed V922 (7eadf4d2314a84a5aa7ca545c25cb8869d4b5f7f). Local only,
unsigned experimental build; no automatic installation or profile changes.
The retired Valhalla depth/vector lane is not restored.

## Try it

Restart the matching V923 tray and game (control ABI 92301). Select
Reconstruction > OFR. This declares that the game uses AER. Start with
FidelityFX, Size 50%, Swap phase off. OFXR selects the existing NVIDIA optical
flow engine instead. Size is per axis for motion estimation; colour stays at
full output resolution. No depth or game motion vectors are required.

OFR owns the temporal pipeline in this first trial. Ordinary framegen settings
are retained and restored when OFR is deselected; stacking an additional RIFE,
DLSS-G or other generator after OFR is not implemented. OSR/AFW are excluded
while OFR is selected, including their native guide capture and reconstruction
allocations. Neural Rendering retains its own independent setting.

If Reconstruction-only transport was active, restart the game after selecting
OFR. For the special Horizon fresh-source route, select OFR before game startup
and restart before switching back. Generic live transport changes on that route
have not been validated.

## Delivery contract

For paired AER, raw A=(L0,R1), B=(L2,R3) produces ordinary midpoint S=(L1,R2).
OFR presents (L1,R1), then (L2,R2), reusing that same generator and OpenXR
presenter. Two per-eye texture-region copies select the endpoints. Raw A is
retained until the complete output fence; S is not published at its earlier
ordinary synthesis fence. Swap reverses the assumed temporal phase; it never
exchanges physical left/right resources, array slices or view rectangles.

For separately released alternating eyes, one source repeats. The ordinary
midpoint is already coherent and is published once; the unused B output is
not submitted. The application clock uses one physical display period for
alternating releases and two for paired releases. Extra presentations with
unchanged source identities do not increment the temporal phase. Capture gaps,
resource-history changes and topology changes re-prime instead of guessing.

The first trial assumes regular half-phase AER. Delivery is inferred from
per-eye XR release/history identities. These are **not pixel freshness proofs**:
a mod that recopies an old eye into a new XR release can look like paired
updates. A shared stereo release also cannot prove which eye was rendered
last; Swap is the manual phase choice. No generic AER/content detector,
scene-cut detector or irregular-phase timestamp recovery is claimed.

## Validation boundary

CPU checks cover saved controls, effective engine selection, physical-period
choice, endpoint camera metadata, both phase choices, 2,400 alternating/paired
steps with duplicate presentations, capture gaps and history resets. GPU checks
compare endpoint selection against the actual existing FidelityFX/NVIDIA
midpoints, with both Swap choices and paired/single-eye masks; exact receipts
must cover all output writes. See local V923 evidence for the recorded results.
These fixtures do not validate headset cadence, image quality, comfort,
latency or game performance. Valhalla in-game evaluation is still required.
