# V833: Advanced mode is a global UI preference

The authoritative setting remains `[ofxr] AdvancedMode` in the main
`ofxr-bridge.ini`. It is not stored in game profiles.

## Persistence and profile behavior

Game-profile reads override legacy per-game AdvancedMode copies with the main
INI value before parsing/sanitation. This avoids losing advanced-only choices
when the global preference is enabled but a game profile contains an old zero.
An absent global key uses the existing basic-mode default. Per-game values
never migrate back into the global preference.

The settings serializer excludes the preference by default. Only explicit
global serialization includes it. Normal game saves remove old copies when
the existing OFXR section is replaced. There is no bulk rewrite of profiles.
New profiles inherit rendering preferences without copying AdvancedMode.

Game requests and delta edits use the global preference; they cannot change
it through a game profile. Reset to defaults preserves this global UI choice,
while resetting the selected rendering settings. The same applies to resetting
the global rendering template.

Snapshots mirror the preference from the existing global shared mapping used
for tray preferences, including in an already-bound game process. Observing a
global UI change does not change the game revision or sanitize/reconfigure its
currently running rendering route. Normal initialization and explicit settings
edits still apply the existing basic/advanced validation rules.

Frame-thread reads use a nonblocking shared-memory read and last coherent
value, not disk polling. File reads occur when loading a channel/profile.
AdvancedMode remains an INI-only option; manually editing the main INI follows
the existing matching game/tray restart workflow, not a new live file watcher.
The existing global value is preserved.

## Focused verification

Focused CPU/UI regression checks cover:

- `xrfg_game_profiles`: two-process game isolation, profile inheritance/reset,
  no AdvancedMode in new/saved game files, conflicting legacy values ignored,
  direct/delta game edits unable to override the global preference, a bound
  child process observes a global change, and a running advanced route/revision
  stays intact merely while observing it. Game edits leave the main file bytes
  unchanged.
- `xrfg_autonomous_control_tests`: explicit global codec round trips retain
  advanced-only choices; default game serialization excludes AdvancedMode.
- `xrfg_settings_widgets_tests`: actual shared desktop/headset controls and
  basic/advanced configuration behavior remain functional.

Snapshot reads change visibility only, without silently sanitizing a live route.

## Scope

This change affects UI preference persistence and visibility. It does not change
rendering, camera metadata, pacing, NR, RIFE or keybind behavior. Use matching
tray and layer binaries, and restart an already running game/tray after updating.
