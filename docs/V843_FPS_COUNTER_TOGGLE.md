# V843: independent FPS counter visibility and F6 shortcut

Display now has an **On-screen FPS counter** checkbox and a **Counter type**
dropdown. The dropdown contains the six existing corner/controller/bottom types;
Off belongs to the checkbox. The selected type remains editable and remembered
while hidden. Both desktop and headset use these same controls.

The active profile stores `overlay_enabled` separately from `overlay_position`.
Existing Off/position0 settings migrate to disabled with Bottom selected; existing
positive selections retain their type and enabled state. Runtime INI output still
uses the existing `overlay.position=off` when hidden, so the established overlay
polling, release and rendering behavior remains intact. Toggling does not change
frame generation, NR or pacing. Disabled controller meters do not request their
controller gesture action set.

The global **Configure keybinds...** dialog gains **Toggle FPS counter**, default
**F6**. The sixth binding uses the existing capture, Clear, defaults, duplicate
validation, exact modifiers and held-key/capture suppression. Its global tray key
is `fps_counter_hotkey`; the counter's visibility still follows the active game
profile like the prior display setting. F6 updates the same checkbox state and
restores the selected type. Existing five binding indices and defaults remain.

The control settings layout and keybind array change, requiring matching V843
tray/layer and a restart when upgrading. Subsequent visibility/type/keybind edits
apply live. Validation is restricted to shared widgets, settings migration and
persistence, six native binding rows and the existing D3D11/D3D12 overlay fixtures
with hide/restore actions. No game, pacing, reconstruction or RIFE matrix.
