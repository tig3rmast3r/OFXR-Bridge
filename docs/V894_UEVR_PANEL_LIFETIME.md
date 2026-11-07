# V894: release inactive generated panels

Build V894 retains the previous UI/input corrections and improves optional-panel resource lifetime.

A Ghostwire V893 startup trace records one admitted UEVR panel and
another refused with `XR_ERROR_LIMIT_REACHED` (-10). Later application EndFrame
calls return a latched runtime failure. F9 was off at the first failure, so the
trace does not establish which worker or native operation originally failed.
These traces do not establish that the specific headset freeze or earlier GUI image exchange is solved.

OSR OFF does not reserve OpenXR swapchains in the UEVR reconstruction path.
Its inactive observer contains CPU metadata; GPU capture/render entry points
require the reconstruction to be active. Optional panel generation is a
separate feature, admitted from submitted quad/cylinder layers even with OSR
off. It uses independent native output rings rather than projection pooling.

Previously a source marked as a panel was excluded from unused-projection
cleanup indefinitely. A panel disappearing from submitted layers retained its
generation output rings until teardown, settings reconfiguration or budget
recovery. The new path drains old readers, releases only absent panels' private
generation/history and allows a later appearance to rearm. The application's
original swapchain remains intact; a still-visible panel retains its generator.
No extra wait is added to steady frames with unchanged panel membership.

Panel admission now checks the existing budget-refusal result. If only the first
of two panel generators fits, release that optional generation immediately and
keep real auxiliary images plus world generation. Do not wait for a subsequent
auxiliary-copy allocation to discover the same exhausted budget. Existing V854
copy-allocation recovery remains active.

A fatal worker transaction leaves one small failure report even with F9 off.
This preserves the originating error when logging is re-enabled after a freeze;
it does not enable continuous diagnostic logging.

Validation: the identical close/reopen test with pre-fix production code fails
at the hidden panel's retained generator. Candidate passes nine targeted checks:
separate-panel close/reopen and partial admission with OFXR and RIFE 2X, existing
panel atlas paths, metadata ownership, general inventory and D11/D12 two-panel
pixel identity. Native runtime is mocked; GPU generation and layer paths are
real. Close/reopen tests run sixteen inputs to cover valid reprimes after panel membership changes. No game or pacing matrix.

OSR pixel/pose/history, RIFE worker/models/compiler, NR, benchmark rules and
private runtime transport implementation are unchanged from V893.
