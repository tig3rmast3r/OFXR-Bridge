# V841: acquire RIFE native cycles after preparation

## Evidence

The V840 SteamVR Director + Buffer capture stopped at 22:39:06 on
2026-11-02 contains 603 content submissions and 754 native Wait calls.
151 native-cycle age renewals recur every four packet IDs. At 90 Hz,
`90 * 603 / 754 = 71.976`, matching the reported stable 72 FPS.
Predictions discarded by the age guard still lie 15.253–24.710 ms ahead.
The cycle was acquired before preparation and aged while its image was copied.

The subsequent Framework + Buffer capture stopped at 22:51:17 shows two
transient drops, 90 -> 62 -> 90 and 90 -> 60 -> 90. It does not establish a
fixed Framework plateau. Compact delivery time rises from about 12 to 19 ms,
and total pipeline cycle from about 63 to 83 ms. These are pipeline timings,
not pure neural inference costs. Detailed flight records for the drops were
overwritten by verbose rotation; the retained recovered tail has mean RIFE
inference 0.827 ms. A shared cause for every Framework drop is not proven.

Preserved local evidence: `rife-72fps-v840` and
`rife-framework-drops-v840` under the laboratory diagnostics directory.

## Change

Only enabled RIFE on the private native SteamVR transport uses the new policy.
Regular native Wait jobs start once the FIFO head's GPU image preparation is
complete. This applies to Framework and Director, with either Buffer capacity.
Director's qualified early Wait cannot bypass this RIFE gate.

Seed, shutdown and expired-cycle recovery still obtain their native cycles.
The Director age guard and genuinely expired prediction recovery remain in
place. FIFO image ownership, native Begin/End order, submission spacing,
Buffer reserve, swapchain counts and poses are unchanged. Returning to ordinary
OFXR restores its existing native clock policy. VDXR is outside this change.

Existing native timing receipts, age renewals and RIFE completed-work timings
also survive in the asynchronous compact benchmark log. No extra GPU query or
render-thread disk write is introduced. Logging remains controlled by F9.

## Focused validation

- Eight D3D11/D3D12 x Framework/Director x capacity 2/3 GPU cases deliberately
  delay image preparation 20 ms. Every exact image is delivered, all native
  Wait receipts follow preparation, and no age-only renewal occurs.
- The existing ordinary Director aged-cycle tests still require recovery
  after delayed preparation. Native contract, live controls and Framework
  reference policy checks remain passing.
- Shipping RIFE Luke 2X Framework/Director, Buffer off/on, 3X Director, 4X
  Framework and live 2X/3X/4X/Quality changes pass using the NVIDIA runtime.
- Logger rotation preserves inference and native-cycle evidence in the
  compact sidecar, without duplicating inference begin records there.

These checks reproduce and remove the avoidable pre-preparation cycle ageing.
They do not certify 90 FPS in the user's live game or explain every transient
Framework drop. Recheck the game with the matching V841 layer; compact timing
records now preserve a subsequent drop even if the verbose log rotates.
