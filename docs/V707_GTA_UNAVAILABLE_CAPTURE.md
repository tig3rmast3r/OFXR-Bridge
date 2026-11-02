# V707 GTA source-unavailable diagnostic

The owner disputes attribution of the V705 capture to the reported scripted
scene. It may be pause/menu. Its pixels and quad metadata describe that captured
instant only; they do not establish how the owner's problematic cutscenes are
rendered. V706 is held, not an accepted fix or the basis for this build.

V707 branches from V705. It changes diagnostic triggering, not AFW admission,
camera, vectors, colour routing, one-second continuity, history, NR or pacing.
The V706 opaque-quad early bypass and new status are absent.

The old auto dump triggered on a missing physical CRC before continuity. It
could capture while AFW still produced successfully using the bonus. The new
trigger observes source selection AFTER continuity. It records an unavailable
source only if the bonus also failed to admit it. This diagnoses source-loss
N/A; unrelated downstream GPU/provider failures are not reclassified.

F9 rising arms a bounded diagnostic window even if no healthy sample was logged
first. Four episodes per F9 window, sustained recovery/cooldown within the
window, existing recorder byte limits and eight-frame timeout remain. A new
explicit off/on rearms an already persistent failure. Logging OFF performs no
new image copies. Manual F8 remains available.

On the failing EndFrame, the dump freezes the exact two scratch colours and
any original D11 application quad from its last released image, along with
the same EndFrame's poses, layer flags, image indices and display target. The
quad snapshot no longer depends on a subsequent release. Guide state locks
are released before application swapchain locks or diagnostic image copies.
No capture's appearance is used to decide whether gameplay is 2D.

`afw_unavailable_after_bonus` in the timeline identifies the trigger, while
`origin_loss_crc` retains physical per-eye read results. `na_application_quad`
is a diagnostic image, not an AFW input. Original release snapshots and future
NGX/AFW evaluation witnesses remain separately labelled.

Requested reproduction: enable F9 only during the actual troublesome scripted
sequence (or already in its N/A), avoid pause/menu until that section finishes,
then switch F9 off to flush. No manual F8 is required. Hardware evidence pending;
this diagnostic is not advertised as fixing the underlying N/A.
