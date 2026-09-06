# FPS overlay (XRFG-V066)

The overlay shows **only the output FPS number**, on a fully transparent
background: green while fresh synthetic frames are being accepted, red when
generation is inactive. No labels, suffix, panel or extra counters are drawn.
V062 replaces the V060/V061 multi-line panel; no legacy display mode is retained.
V066 only reduces the visible quad and digits by 20%, retaining V065's centre;
all optical-flow paths retain V063 behavior.

## Controls

Tray -> **FPS overlay** -> **Upper left**, **Upper right**, **Lower left**,
**Lower right** or **Off**. Upper right is the clean-install default. While the
tray remains armed, this selection updates its matching running layer within the
next approximately 250 ms of application frame activity; no game restart is
needed. Backend/preset/flow-resolution changes still require a session restart.

For an embedded layer, edit the INI next to that loaded DLL instead:

```ini
[overlay]
position=upper_right
```

Values are `upper_left`, `upper_right`, `lower_left`, `lower_right`, `off`.
The setting is independent of diagnostic logging. A different version's tray
or INI cannot control this instance. Disarming does not unload an already
loaded DLL; a disarmed tray does not update its runtime configuration.

## Reading the number

The value counts nonempty downstream `xrEndFrame` submissions returning
`XR_SUCCESS`, including originals, fresh synthetics and any presenter repeats.
It is rounded to a whole number (0-999), using a roughly one-second rolling
window and refreshing at most four times per second.

Green means a fresh synthetic submission succeeded within 500 ms; red means
none did. This short hold prevents color flicker between original and synthetic
frames. Prime copies, repeats and failed/empty synthetic submissions never
activate green. Intermittent generation can remain green during that hold.

The number measures accepted submissions, **not physical headset scanout**, compositor
reprojection, GPU completion or frame quality. The runtime may drop frames even
after accepting them. A frozen game/runtime can leave the last panel frozen;
the panel is not an independent watchdog. No panel can be shown if the DLL was
never loaded, the session is not rendering, the runtime has no spare composition
layer or the optional overlay cannot initialize. Use the flight recorder to
investigate these cases.

## Rendering and cost

The overlay is a separate transparent, head-locked OpenXR quad, visible in both eyes.
It is appended after application composition layers on original and synthetic
submissions, including normal pass-through fallback. It never enters FidelityFX
or NVIDIA optical flow and never alters application images, pose/FOV, temporal
history or existing layer order. Empty application submissions remain empty.

The quad is inset within the common eye FOV, capped to the central 90 degrees
for wide-FOV headsets. V065 increases the horizontal/vertical edge margins to
25%/29% of that FOV's tangent span (V064 used 20%/24%; V063 used 14%/18%). Upper right moves left
and down, with mirrored inward movement for the other corners. V066 retains
V065's centre and reduces the 2:1 quad width and height to 80%, so the digits'
angular size is 20% smaller. Depth, glyph design and colors stay the same.
Its angular footprint follows that FOV. Texture density
uses the initial per-eye submitted image width (96-336 pixels wide), not a
fixed desktop pixel size; changing render resolution mid-session does not
recreate the texture. A new session adopts the new resolution.

Only digit strokes have alpha 1; all other texels are transparent black. The
quad enables premultiplied texture-alpha composition as specified by
[OpenXR layer flags](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerFlagBits.html).
The smaller 2:1 footprint replaces the former three-line panel.

Native D3D12 uploads use the bound queue, explicit RT/COPY_DEST/RT barriers and
per-image upload/allocator fencing. D3D11 bindings use the application's
end-frame thread and immediate context, not the asynchronous presenter. Uploads
are capped at four per second. Acquire/wait/release ownership is respected,
including positive `XR_TIMEOUT_EXPIRED`: retry that acquired image later and
reuse the last published panel. There is no explicit GPU fence wait during
frame submission; teardown waits for the overlay's own upload completion.

Off submits no overlay layer and performs no texture uploads. Metrics and the
low-rate configuration poll remain enabled so the panel can return live. The
overhead of an enabled quad is not zero and must be measured in-headset.

## Verification

Release CTest: 20/20. Coverage includes CPU cadence/color/geometry tests and real
D3D11/D3D12 WARP texture uploads through a fake OpenXR runtime: GPU red/green
and transparent pixel readback, source-alpha flag, exact numeric glyph layout,
timeout ownership, layer/next-chain preservation, fallback,
empty/failed frames, all live menu positions using the tray's atomic INI
replacement pattern, maximum layer count, session reset and balanced lifetime.
Existing renderer, interop, SteamVR and Flight Simulator call-chain tests pass.
Bitmap layout was visually inspected. The user sees the number in-headset but
found V064 placement still too peripheral; V065 moved it inward again, and V066's
smaller size is pending a headset check. CPU tests verify exact placement and
20% size reduction, including asymmetric
FOVs; both graphics API integration tests check the submitted quad position.
