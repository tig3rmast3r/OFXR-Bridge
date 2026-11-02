# V764: OSR first and default

The Luke Ross Reconstruction selector now has two choices, in this order:
OSR, then AFW (legacy). OSR is the factory/reset default. The former
AFW + OSR band-fill option and its advanced controls are no longer exposed.

Existing configuration identifiers remain stable: 0 is AFW, 2 is OSR.
The retired value 1 migrates to 2 on profile load, live settings requests,
and runtime INI generation. Explicit saved AFW selections are preserved.
The same selector is used by desktop and in-headset settings.

This build includes the original two Full OSR commits by djules75 on top of
public V763. The OSR/AFW shaders and reconstruction algorithms are unchanged
from that source. The RIFE experimental lane is separate.

Use matching V764 tray and layer; close the old game and tray before upgrading.
Reconstruction changes apply live. Frame Reconstruction only retains its
existing game-restart requirement. Headset quality validation remains separate
from the targeted configuration and UI checks.
