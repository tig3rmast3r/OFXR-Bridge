# V825: desktop profile selection

The desktop Settings header is a dropdown. Global default is always the first
entry, followed by the regular INI files directly inside the shared Profiles
folder, sorted by filename. The folder is read again whenever the popup opens;
no restart is needed to discover added or removed profiles.

Selecting a saved profile loads its persisted settings and edits only that file.
Pending edits are finished in the previous profile before a manual or automatic
switch. The selection is scoped to the desktop frame: tray commands continue to
target the active game, including messages pumped by a native file dialog.
Bound game/headset configuration always takes priority over a desktop scope.

A new game connection or a restart with a new process ID automatically selects
that game's profile even while the desktop window is open. A manual selection
remains until the next new game connection. With automatic following, disconnect
returns to Global default. Busy discovery preserves the previous selection.

The headset retains its text header and never invokes the desktop dropdown.
Based on V824; Director, Buffer, Fast, NR, frame generation and RIFE unchanged.
Only targeted profile/picker/header checks are required for this desktop change.
