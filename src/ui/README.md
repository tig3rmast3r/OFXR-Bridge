# Standalone controls

This module supplies a Dear ImGui tray settings window and in-headset menu.
`settings_widgets.cpp` physically replaces the former tray-only controls; both
locations draw the same options and publish only edited fields. No OptiScaler
module is required. The tray owns its Win32/D3D11 window; the headset uses the
existing FPS OpenXR compositor path and a private ImGui context.

Both surfaces use one 1024x768 layout: a left rail of sections, the controls in
the centre, and a fixed status pane on the right that also shows help for the
hovered or focused control. Controls are segmented buttons rather than dropdowns
where the options fit, because every option stays visible in the headset. Labels
are plain text, not items: `update_menu_drag` needs empty space with no hovered
item. `settings_fonts.inc` holds Atkinson Hyperlegible subsets (SIL OFL) and
`settings_icons.inc` holds the icons as polylines; both are generated files.

The headset menu is a separate 1024x768 quad sharing FPS's VIEW space. It is
updated only on application frames, appended to original/synthetic submissions,
and does not modify game images, graphics state or frame-generation guides.
D11 uses a private driver context-state object with complete immediate-state restoration, including SINGLETHREADED devices.
D12 uses the accepted immutable OptiScaler renderer adapted to private command
lists and exact per-image queue/fence receipts. Slow GPU work skips UI updates,
retains ordered input events and never resets a pending allocator or atlas.
Shared ImGui context serialization covers multiple XR session threads.

Input follows an actual foreground HWND in the game process. F10 toggles;
Escape, loss of desktop/XR focus, missing compositor slots, disarm and teardown
close capture. Raw relative/absolute movement drives the cursor that ImGui draws
in its own framebuffer. Legacy mouse messages contribute normalized deltas,
so desktop origin, mirror dimensions and DPI do not place a separate pointer.
Win32/raw input and pre-existing DirectInput keyboard/mouse reads are masked
while open. Game-held releases retain their ownership. Raw registrations,
cursor clipping and capture requests are restored on close. Controller/XInput
polling is not intercepted; the menu is operated by keyboard and mouse.

Opening the panel never activates the layer. The tray starts inactive; only
the explicit Activate command stages the package and registers its OpenXR and
Vulkan manifests. The OpenXR layer identity is `XR_APILAYER_OFXROFXR-BRIDGE`,
distinct from the original OFXR tray. The DLL retains the existing component
filename for build and packaging continuity.

`%LOCALAPPDATA%/ofxr-bridge/ofxr-bridge.ini` owns renderer, AFW, NR, overlay
and logging preferences. `tray.ini` in that directory retains activation/API
bridge and pause-key preferences. The version cache and manifest cleanup stay
inside this profile's `RuntimeLayer` directory. Manifest filenames use the
`XR_APILAYER_OFXROFXR-BRIDGE_manual-` prefix; the original OFXR profile and
its registry entries are outside this tray's ownership.

The plain CPU interface in `xrfg/autonomous_control.hpp` publishes revisions
through a versioned, profile-specific shared mapping. Writes persist atomically
before publication. Frame-thread readers retain the last coherent settings
while the UI writes; they never wait on its disk commit. Runtime status reports
the producer PID, heartbeat, applied revision, session readiness and AFW/NR
activity. A status older than five seconds is no longer presented as connected.

The layer consumes every UI value through its production control transaction:
OFXR engines and guide source, FSR/DLSS-G, optical flow quality/resolution,
DJ multiplier/deep policy, output storage, AFW mode/warp parameters, complete NR
model/colour/comparison settings, headset overlay and logging. Motion vectors
and depth remain mandatory for FSR and DLSS-G. The accepted depth-aware Luke
correction is a runtime invariant and has no menu bypass.

Activating a package requires its FSR helper, AFW plugin and NR forwarder. The
NVIDIA NR vendor runtime is optional and is copied only when the user supplies
it in the package's `ofxr` directory. No game directory is modified.

For engineering inspection, `--settings-only` opens just the panel,
`--settings-smoke` opens it briefly, and `--settings-preview <image.bmp> [section] [headset]`
captures the panel's own backbuffer. These paths perform no activation or
registry cleanup. The test profile override `OFXR_BRIDGE_PROFILE_DIR`
keeps IPC and fake-runtime fixtures separate from the user's configuration.

The new UI/control code is part of the bridge and covered by the repository's
GPL-3.0 license. Dear ImGui retains its MIT license and original
copyright notices; source provenance and the small standalone integration
adjustments are recorded in `vendor/README.md` and `vendor/imgui/LICENSE.txt`.
