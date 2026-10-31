# OpenXR panel support

Adapted from djules75/OFXR-Bridge V508, V509, V510, V511 and V574.
Upstream IDs refer to that project's independent numbering.

Panel identity includes its type, source swapchain, space, atlas rectangle,
array slice and eye visibility. Matching consumes each previous panel once.
A resized rectangle can match only the sole unmatched panel on both sides.
Immutable projection snapshots retain panel identities across private image
ownership and asynchronous workers; synthetic outputs use their own fraction.
REAL VR panels retain their original placement, following upstream V524.

Game/headset acceptance remains pending. No scheduler policy is imported.
