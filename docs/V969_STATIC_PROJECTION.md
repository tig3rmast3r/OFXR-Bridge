# V969: immutable projection alongside a generated scene

UEVR may submit a static dummy projection before a mutable VR scene. Static
OpenXR swapchains deliberately have no temporal capture. The shared generator
mapping included this projection, so the missing-capture admission check rejected
the entire scene even while both real eye captures were arriving.

Static projections now remain validated composition inputs without entering the
capture/generation mapping. Their original subimages remain in real and generated
submissions. Every mutable projection still requires its generated destination;
missing mutable output and invalid static rectangles continue to fail safely.
No game-name or image-size heuristic, runtime budget, AER ordering, provider,
DLSS guide ownership or RIFE pacing changes are involved.

The CPU regression covers packed, split and array scenes, static layers before
and after the scene, real/generated submissions and validation refusals. It fails
on the previous implementation. Ghostwire headset behaviour, performance and its
separate UEVR stack-overflow report still need owner validation. The unclassified
swapchain inventory warning is unchanged.
