# V896: one OpenXR swapchain for the headset interface

Build V896 introduces the shared headset UI atlas. V897 subsequently makes its use conditional on SteamVR handle pressure.

F10 menu, corner FPS counter and Elliott meter now share one native OpenXR
swapchain. Menu and counter remain independent composition quads, poses,
visibility and input. Sharing reduces native handles, not composition layers.
The existing preference for the menu when a runtime has only one spare layer
is preserved. Desktop rendering, settings format and keybinds are unchanged.

Each UI renderer retains its private draw targets and its original dimensions.
The producer copies the latest visible targets into a 1024x1280 RGBA atlas:
menu at y=0, counter at y=768. Only this atlas is handed to OpenXR. The internal
tokens never reach the application's frame submission or native inventory.
Both quad rectangles sample their own region with unchanged alpha and scale.

Copies run only when visible content changes. Checking unchanged content adds
no heap allocation, GPU copy or new blocking wait. A source image cannot be
reused until the atlas copy's fence has completed. Acquiring/waiting the real
atlas remains nonblocking; positive timeout never grants image ownership.
Native composition uses only a successfully released atlas and never shows
a newly opened region before its first publication. Closing/reopening one UI
keeps the other; switching both off releases the native handle during the session.

This applies to every existing D3D11/D3D12 headset-UI path, including native and
bridged runtimes. Runtime limits are checked first. If its declared maximum
texture dimensions cannot hold the atlas, the existing individual surfaces are
used instead. Such a runtime may still require two handles for both panels.
The swapchain inventory deduplicates the shared handle across its UI categories.

The tradeoff is explicit: there is an additional GPU copy on a UI update and
private scratch images remain allocated. An atlas image is exactly 5 MiB before
driver alignment; three images are 15 MiB. The private three-image menu and meter
sets occupy another 9+6 MiB when both exist, excluding their existing uploads,
font resources and runtime-specific copies. When both are hidden none of these
UI swapchain reservations remain. This trades handles for some VRAM rather than
claiming to reduce both. Isolated RTX 4090 driver checks also pass in D3D11 and D3D12. Total UI GPU work measured about 0.10 ms with the menu open in that fixture, including rendering and atlas composition. This does not measure the isolated extra copy or the SteamVR compositor.

Validation: eight targeted WARP D3D11/D3D12 checks pass: simple counter, Elliott,
menu/counter/meter, and low maximum-height fallback. The shared-menu test imposes
a native swapchain limit of one, verifies acquire/wait/release ownership, keeps
both quads present, switches counter types, tests close/reopen, focus, placement,
layer limits and release with both UIs off. The same cap fails on V895 when the
second UI opens. Actual atlas images with both menu and meter were read back
and visually inspected in both APIs. First fallback-test failure was a fixture
mix-up of width/height aggregate field order, corrected using named fields.
No game, pacing or RIFE matrix. Headset validation remains separate from the isolated driver checks.

The V895 UEVR 3-to-1 auxiliary sinks and exhausted-budget real-frame recovery,
V894 panel retirement and V893 input/control corrections remain. OSR/RIFE/NR,
private runtime transport, benchmark ranking and game output timing are unchanged.
