# V854: a SteamVR swapchain refusal no longer freezes the headset view

This nightly update changes what the three-slot route does when SteamVR
refuses a private copy of an auxiliary layer. Nothing else changes.

## Symptom

Returnal under UEVR froze the headset view about 7 s after injection, while
the desktop kept running.

## Cause

Once UEVR submitted its 3840 x 2160 UI quads, SteamVR refused the private copy
of the first one (`composition_publish result=-2 b=1 c=-10`,
`XR_ERROR_LIMIT_REACHED`): generated panels (transition 761) had already
taken the session's native swapchain handles, since each panel source keeps
outputs of its own. The three-slot handoff returned that refusal from every
xrEndFrame (2055 of 2055 failed). The only fallback for this refusal
(`luke_aux_budget_inline`) covers AFW on D3D11, not the D3D12 three-slot
route.

## Change

On that refusal the three-slot handoff drains the presenter and, once per
session, hands back every panel source's generation outputs and history,
then retries the copy. Panels stay on their real images, as in Fast mode;
projection generation continues. If no panel held outputs, or the runtime
refuses again, the session's generation budget is released and frames go out
real instead of failing.

## Flight log

`presenter_transition` 763: a=1 panels retired and the copy retried, a=2
generation budget released.

## Test

Returnal, UEVR, SteamVR: one refusal at 6.47 s, 763 a=1 at 6.53 s, no failed
xrEndFrame afterwards, headset view live.
