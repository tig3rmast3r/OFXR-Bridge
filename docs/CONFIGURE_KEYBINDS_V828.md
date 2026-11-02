# V828: Configure keybinds

Base V827 `4d0edf239b61332d8e997e52ba74e0336bcdc6f4`. The tray entry
"Pause key" becomes "Configure keybinds..." and opens a native five-row dialog:

| Action | Default |
|---|---|
| Open interface | F10 |
| Toggle NR | F11 |
| Toggle framegen | F7 |
| Enable logging | F9 |
| Dump (debug) | F8 |

Each Change button activates the existing Windows hotkey control and conversion
code for that row. A complete key/chord finishes capture; Clear disables one
binding. Restore defaults edits the draft only. OK rejects duplicate actions and
checks global chord availability before saving. Cancel preserves active values.

Bindings are global tray settings, serialized in the global configuration's
[tray] section, never a selected desktop/game profile. Canonical entries are
interface_hotkey, nr_hotkey, framegen_hotkey, logging_hotkey, dump_hotkey. The
legacy pause_hotkey alias remains for older trays. The old shipped Ctrl+Alt+F7
value migrates to the requested F7 default; other legacy custom chords survive.
An explicit framegen_hotkey has priority over the legacy alias in either order.

A tray-global field in the existing control mapping publishes the five parsed
chords live; readers bypass game/desktop profile selection. This requires matching
controlABI82801 and a game/tray restart when upgrading from827. Thereafter the
same running828 game receives binding edits without restarting its input hooks.
A renewable two-second capture lease suspends runtime shortcut actions during
the dialog; framegen registration is released until it closes. Press edges are
latched across suspension and binding changes, so held keys do not fire on save.
Modifier matching is exact, including existing INI Win-modifier support.

Framegen retains the existing armed tray pause/resume signal; it does not toggle
tray activation. The interface binding retains the existing headset menu input
ownership and game focus contract. NR and logging update the same active-game
settings as their UI controls. Logging keeps its existing on/off behavior.
Dump starts only with file or RAM diagnostics active; both ordinary two-frame
capture and Luke ring save use the configured chord. Legacy extra-Ctrl ring
control remains when it does not collide with a configured action. Configured
bindings take priority over the fixed advanced diagnostic chords. No synthetic
keyboard events are injected into other applications.

Validation was restricted to THREE relevant checks, all PASS in0.72s: launcher
parsing/serialization; actual modal native capture for all five rows, Clear,
defaults, duplicate rejection, OK/Cancel, global persistence while an offline
profile is selected, exact modifiers/held edges and a separate bound-game process
reading the published bindings; existing overlay input lifecycle plus live
remapping (old key returns to game, new key opens/closes, capture suspension).
Native dialog preview was rendered and visually inspected. No headset, GPU,
pacing, RIFE/inference or broad renderer suites were run. No live install,
registry activation, user process stop, profile edit or publication performed.

V827 pacing and V826 RIFE queue algorithms are byte-identical. NR model/rendering
implementation is unchanged; only its requested enable setting gets a keybind.
