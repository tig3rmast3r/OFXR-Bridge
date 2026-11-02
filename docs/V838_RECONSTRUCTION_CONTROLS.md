# V838: reconstruction controls and source reporting

The main Reconstruction menu is now **Off / OSR / AFW**. Off sets the existing
runtime `afw_mode=0` switch, which blocks both engines and clears selected
reconstruction history at the existing live transition boundary. Selecting OSR
or AFW restores Combined when the prior mode was Off. The last selected engine
and all tuning values are retained. Existing profile values need no migration.

The OSR advanced mode menu contains Other eye only / On. The AFW advanced mode
menu contains Alternate eye / Previous frame / Combined. Neither advanced mode
menu contains Off. Depth dilation's separate Off option remains a filter control.
Advanced reconstruction controls are hidden when the primary choice is Off.
Frame generation, Neural Rendering and the startup Frame Reconstruction only
choice remain independent.

Desktop/headset UI and the headset meter show a separate green source row:
**OSR: Off**, **OSR: Luke Ross**, or **OSR: Other**. The legacy engine uses an AFW
label while active. Source identity requires a fresh successful reconstruction
report; merely selecting OSR or detecting guides does not claim active output.
Explicit Off overrides an old activity receipt. Missing/stale output is Off.
Other compatible mods are not disabled or identified as Luke Ross.

The previous display declared reconstruction not applicable for non-Luke games,
even when their actual reconstruction report was active. The new source row uses
that activity report and leaves other errors in the diagnostic status untouched.
All three source states are valid green states. No shader, model, input-capture
algorithm or delivery pacing is changed. The Dying Light report motivated the
change, but no Dying Light game run is claimed by the focused regression checks.
