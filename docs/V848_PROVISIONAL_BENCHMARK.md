# V848: provisional benchmark matrix and owner-sheet strings

Based directly on Djules V84660278084 andV847fcfb10a4: retains the presenter
refused-output recovery and UEVR Alternating/AFR OSR support. Main/tag unchanged.

## Temporary sequence

The layout was read from the owner's live sheet on2026-11-03. Every selected
route uses its Normal/Fast pair on Legacy VDXR, or eight combinations on private
SteamVR: Framework Normal/Fast, Framework Buffer Normal/Fast, then Director with
the same four settings. Advanced mode uses these same eight, matching the sheet.
Luke Ross instead puts OSR normal/cheap into the Normal/Fast positions and leaves
Fast off. AFW is not tested. The normal user-selected AFW option remains available.

Order: Off reference (OSR normal/cheap references for Luke),0.2.1,Djules2X,0.3.0,
RIFE4.4 Medium18/High21/Very High25 at2X, conditional same3X tier, then
Hybrid/FSR/DLSS-G for Complete tests. Luke stops after RIFE; no vector trials.
No Djules3X or automatic4X trial is included. Manual4X options remain available
on their existing supported paths.

RIFE3X is skipped only when a valid preceding result sustains the actual headset
refresh: median>=displayHz and minimum>=displayHz-1. No fixed90/120 threshold.
All2X qualities finish first. Unknown/unsupported multiplier configurations are
not measured as2X under a3X label: they remainn.a. The entire winner function is
byte-identical toV847; automatic preset selection is temporary, until the final
single-RIFE suite where users select quality themselves.

Potential counts: Legacy optical19/complete25/Luke20; privateSteamVR optical73/
complete97/Luke74. With3X omitted, counts shrink by6onLegacy or24onprivate.
RIFE TensorRT optimization still completes before settling/measurement, including
live quality changes. Stop/error/disconnect restore original settings.

## Native Legacy VDXR3X

Enable the existing ordered RIFE synthesis worker for nativeVDXR Legacy D3D12,
including the isolated post-OSR Luke lane. Fast retains distinct A/B output slots.
Native4X remains unsupported; D3D11, split-loop and other unsupported native
configurations keep their2X capability and omit3X trials. Private3X/4X is unchanged.
Policy tests cover native capability, both Fast choices, the Luke worker, and
rejection of split/D11 native configurations. No live nativeVDXR headset/game
performance or temporal pacing result is claimed by these tests.

## Private paste exporter

The owner's internal Python exporter remains outside the distribution repository.
It validates live headers, uses real semantic trial names, and outputs one TSV
row plus local CSV/JSON evidence:

- LegacyVDXR: G:AH,26measurement cells + Tested Build + blankLog.
- SteamVR: AK:EF,98measurement cells + Tested Build + blankLog.

Manual game-only references are untouched. Unsupported/skipped tests stayn.a.
The OSR-cheap reference is blank for ordinary runs; vector columns aren.a. for
Luke. Build precedes Log. Historical schemas require matching offline snapshots;
new headings are never used to relabel old results silently. The live sheet is
read-only: no online values/headings are modified by the adapter.

Focused checks include actual controller-produced CSVs for all four suite types,
current live headings and exact paste-column mappings; settings/widgets/ranking,
optimization/cancellation, dynamicHz gates, importedUEVR math and native3X policy.
No full game matrix, installer test or new model/engine compilation is required.
ABI84801: restart the matching tray and game together.
