# V909: restore GTA camera corrections on Luke2611.5.2.5692

The GTA-specific camera reader supported an explicitly validated October Luke
module. The November5 update relocated the runtime pointer and GTA vtable.
The reader correctly refused that unknown layout, but this also left the
existing GTA camera corrections inactive for both AFW and OSR. Consequently
the OSR-specific aiming pose protection introduced in V842 could not activate.

The new build supports both observed layouts. November admission verifies its
exact PE timestamp/image size, state and yaw instructions, runtime-owner load
and GTA vtable constructor. The selected pointer and vtable are cached once.
Per-evaluation reads retain the existing object/render/state race guards and
mode validation. Unknown builds remain refused. No runtime signature search,
memory writes, new hooks, module loading or continuous diagnostic tracking.

This restores the existing camera behavior; it does not add a new camera rule
or alter reconstruction, motion composition, RIFE, NR or pacing. Both AFW and
OSR consume the same corrected camera. The accepted V907 OSR field remains.

The owner's V908 driving capture has missing GTA tracking receipts in both
eyes. It also uses span1 native motion, so the newer span2 OSR current-history
path is excluded. A separate offline current-field experiment improved one
rear-car feature but is not shipped: AFW also exhibits driving artifacts, and
that experiment alone does not establish the common cause. After V909, a new
aiming/driving F8 must confirm actual GTA receipts and the remaining artifacts.
