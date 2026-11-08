# V914: Basic mode enforces hidden defaults

AdvancedMode remains a global preference, never a per-game option. While it is
off, loading, editing, switching generators and serializing settings normalize
all advanced-only controls and every inactive generator profile:

- Optical flow: Medium,50% per axis, bidirectional Off (including Hybrid).
- OFXR motion source: optical flow; FSR/DLSS-G/Hybrid retain required native guides.
- RIFE model4.4. Its visible Quality and multiplier remain user choices; its
  independent legacy scale defaults to1/6, not the optical-flow scale.
- Reconstruction: enabled hidden submodes return to Combined. Explicit Off stays
  Off. Debug/clear/dilation/shading overrides and old band controls return to defaults.
- Experimental R19 Off; hidden combined pacing returns to Director. Framework
  remains selectable. Visible Buffer/Fast, NR, engine and OSR Quality choices stay.

Flow tuning is now advanced-only on desktop and headset. Global Advanced Off is
published to already open game mappings as one settings revision. Existing runtime
application/drain handles that revision; observation no longer hides controls while
leaving old rendering settings applied. No disk reads/writes or waits are added to
snapshot polling. Each thread checks only new revisions/policy changes. A running
suite stops on a global policy change, and restoring it cannot re-enable Advanced.

Old profile files are normalized on load; no bulk rewrite of user profiles. An
explicit save records normalized values. Advanced On permits custom values again.
No rendering/shader/marker/RIFE/pacing algorithm change; V902-V913 retained.
