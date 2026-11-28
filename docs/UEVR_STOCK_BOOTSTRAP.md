# Stock UEVR eye-history bootstrap

OSR and guided AER need two independently advancing eye histories. Stock
UEVR's ghosting fix stops requesting construction after its global known-state
count reaches two, even when those states belong to an old scene. OFXR requests
two bounded game-thread construction frames through the existing UEVR function.
It keeps UEVR's AFR, Ghosting Fix, local-player, constructor and view-extension
checks. Extra construction cameras do not replace the rendered camera receipt.
Distinct advancing native guide streams remain mandatory before admission.

Official nightly 01143 retains the verified SDK metadata, file SHA256,
whole-function digest and exact instruction-byte validation path.
Cross-build discovery is an additive fallback for compatible images that do
not match the known profile. It identifies the function through its
diagnostic-string reference and x64 unwind chains. Zydis validates the count
comparison, common return-one failure target, all four later readiness checks,
return-one/two calculation and matching epilogues. The two hooks are placed at decoded instruction
boundaries. Member offsets and addresses are learned from code; OFXR never
dereferences private UEVR objects. The fallback requires no fixed hash, commit
or RVA; retaining the known profile does not exclude additional builds.

SDK 2.34 and later retain the required API prefix; other major versions are
rejected. The compiled `GhostingFixBootstrapViewStates` option marker leaves
the fork's implementation in charge, including when the setting is false.
Initialization never enters UEVR's live mod registry. Unknown instruction
shapes, ambiguous matches, entry detours and malformed metadata
leave the hook disabled. This supports equivalent builds, not arbitrary future
changes to UEVR's rendering logic. Stage 1462 records installation/refusal;
stage 1466 records function, count-branch and observation RVAs (result 1 for
the original known profile, 0 for generic discovery).

Discovery uses temporary CPU memory once during initialization. There are no
new GPU resources, frame queues, GPU passes or steady-state presentation waits.
The existing bootstrap attempt limits, cooldown, scene resets and fork path
are unchanged. No UEVR binary or game setting is modified on disk.

Installation remains synchronous during session construction, preserving the
existing callback registration and guide-admission lifecycle. Generic discovery
broadens compatibility without introducing a background installer or additional
callback synchronization.

Validation includes mapped binaries from stable 1.05, SDK 2.36/2.37 builds,
nightlies 01127 and 01143, and the installed Callisto/latest builds. Both hooks
are created by the production installer and then removed with original bytes
verified; DllMain, Unreal functions and rendering are not executed. Fork
capability tests inspect the actual fork image and assert no hook installation.
The SDK mod-registry getter must remain unused during installation on all paths.
CPU regressions cover relocated published code, different layouts, altered
branches/readiness/returns, bad anchors, unwind chains and duplicate matches.

The user confirmed successful DI2 and Hogwarts launches with stock UEVR 01143
and selected this initialization approach. Earlier OSR/AER, required-UI budget
recovery and DI2 F8 reset fixes remain included. Intermittent DI2 startup
failures were also observed; their initiating cause remains unconfirmed, so
this is not a claim of a startup-stall fix. Additional builds need headset
confirmation; mapped-code tests do not establish game compatibility.
