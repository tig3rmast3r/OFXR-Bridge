# Stock UEVR eye-history bootstrap

OSR and guided AER need two independently advancing eye histories. Stock
UEVR's ghosting fix stops requesting construction after its global known-state
count reaches two, even when those states belong to an old scene. OFXR requests
two bounded game-thread construction frames through the existing UEVR function.
It keeps UEVR's AFR, Ghosting Fix, local-player, constructor and view-extension
checks. Extra construction cameras do not replace the rendered camera receipt.
Distinct advancing native guide streams remain mandatory before admission.

The initial implementation was restricted to official nightly 01143. Discovery
now identifies the function through its diagnostic-string reference and x64
unwind chains. Zydis validates the count comparison, the common return-one
failure target, all four later readiness checks, the return-one/two calculation
and matching epilogues. The two hooks are placed at decoded instruction
boundaries. Member offsets and addresses are learned from code; OFXR never
dereferences private UEVR objects. No DLL hash, commit or fixed RVA is required.

SDK 2.34 and later retain the required API prefix; other major versions are
rejected. A nonempty `VR_GhostingFixBootstrapViewStates` capability leaves the
fork's implementation in charge, including when the setting is false. Unknown
instruction shapes, ambiguous matches, entry detours and malformed metadata
leave the hook disabled. This supports equivalent builds, not arbitrary future
changes to UEVR's rendering logic. Stage 1462 records installation/refusal;
stage 1466 records discovered function, count-branch and observation RVAs.

Discovery uses temporary CPU memory once during initialization. There are no
new GPU resources, frame queues, GPU passes or steady-state presentation waits.
The existing bootstrap attempt limits, cooldown, scene resets and fork path
are unchanged. No UEVR binary or game setting is modified on disk.

Validation includes mapped binaries from stable 1.05, SDK 2.36/2.37 builds,
nightlies 01127 and 01143, and the installed Callisto/latest builds. Both hooks
are created by the production installer and then removed with original bytes
verified; DllMain, Unreal functions and rendering are not executed. Fork
capability tests use a controlled SDK getter and assert no scan or hook attempt.
CPU regressions cover relocated published code, different layouts, altered
branches/readiness/returns, bad anchors, unwind chains and duplicate matches.

The preceding r4 changes were headset-confirmed by the user in DI2 and Hogwarts:
stock UEVR OSR and AER, required-UI swapchain-budget recovery, and the DI2 F8 reset
Lua script. Broader binary discovery still needs headset confirmation on each
additional build; mapped-code tests do not establish game compatibility.
