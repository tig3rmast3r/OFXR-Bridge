# V893: optional UI responsiveness and general swapchain inventory

The swapchain inventory moves from Reconstruction to Logging on both settings
surfaces. It now observes every supported OpenXR session, including ordinary
UEVR sessions without Luke Ross. Sampling remains once per second and reads
owned metadata only: it creates no images or GPU work. Counts identify unique
native endpoints, with application, generated, auxiliary and OFXR UI ownership.
It is an inventory of the observed session, not unrelated SteamVR applications.

## Input and resource lifetime corrections

Raw Input duplicate decisions now live only within the exact WM_INPUT dispatch
being forwarded by OFXR. Windows can recycle an HRAWINPUT value; a mod that reads
the packet without forwarding to OFXR's WndProc must not leave a stale decision
that suppresses a later packet using the same value.

Optional OFXR UI uses nonblocking acquisition in the private transport. If no
image is available, it keeps its preceding published image and retries later.
A positive timeout grants no image ownership and cannot trigger a draw, wait or
release. The game's public acquisition contract and rendering policy are unchanged.

Control channels retain process lifetime so late XR overlay destructors cannot
write to an already unmapped channel during DLL teardown. A Ghostwire shutdown
dump identified precisely that invalid access in set_interface_visible.

The earlier V854 panel-allocation fallback is retained. These corrections do
not establish that it caused the reported Ghostwire GUI exchange. The specific
in-game exchange remains subject to confirmation; it was not reproduced in the
focused identity test below.

## Focused validation

Focused checks cover: recycled Raw Input identity, late control-channel teardown,
ordinary-session inventory, shared widgets, D3D11/D3D12 optional-menu acquisition,
two delivery-admission concurrency cases, and equal-sized menu/meter identity.
The identity test checks actual pixels in D3D11 and D3D12 across layer reordering,
native image-index reuse, repeated source images and Framework/Director switches.
No game/pacing matrix or live headset test is claimed.

OSR, AFW, RIFE models/runtime overlay, NR, benchmark ranking and private transport
pacing remain unchanged.
