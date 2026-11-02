# status.json: OFXR Bridge's live state for other tools

OFXR Bridge's tray writes a small JSON file that other programs can read to
know whether frame generation is armed, paused or running, which multiplier is
active, how many frames the headset is getting and the frame latency. It was
added in OFXR 0.2 for [MSFS AutoFPS](https://github.com/ResetXPDR/MSFS_AutoFPS),
which adjusts the simulator's level of detail to hold a target frame rate, and
is meant to replace parsing `tray-lifecycle.log`. The 0.3 file keeps the 0.2
format and adds `latency_ms`.

## Where and when

- **Path:** `%LOCALAPPDATA%\ofxr-bridge\status.json`, next to `tray.ini`.
- **Opt-in:** enable **Export status.json** in Display, immediately below
  the headset counter selector. It defaults OFF and follows the active game profile.
- **Written by the tray** once a second while enabled, and on arm/pause changes.
  The exporter keeps its own FPS/latency measurement request alive when panels
  are closed. Turning it OFF releases that request and removes the current file;
  other open panels and active benchmarks keep their own requests.
- **Atomic:** each write goes to `status.json.tmp` first and replaces
  `status.json` in one rename, so a reader never sees half a file. If your
  program keeps the file open, the tray cannot replace it and tries again a
  second later, so open, read and close.
- **When the tray exits** it writes a last file with `"armed": false` and no
  session. A missing file can mean export is disabled.

## Example

A game running in 3X:

```json
{
  "schema": 1,
  "armed": true,
  "paused": false,
  "multiplier": 3,
  "generating": true,
  "app_fps": 29.8,
  "output_fps": 89.4,
  "display_hz": 90.0,
  "latency_ms": 41.3,
  "updated": "2026-10-28T09:12:03.512Z",
  "ofxr": { "build": 735 },
  "session": {
    "process": "FlightSimulator2024.exe",
    "pid": 23456,
    "runtime": "SteamVR",
    "deep_pipeline": true
  }
}
```

With no game running, the game's fields are `null`:

```json
{
  "schema": 1,
  "armed": true,
  "paused": false,
  "multiplier": 2,
  "generating": null,
  "app_fps": null,
  "output_fps": null,
  "display_hz": null,
  "latency_ms": null,
  "updated": "2026-10-28T09:15:41.007Z",
  "ofxr": { "build": 735 },
  "session": null
}
```

## Fields

The top-level keys are the stable part. A program that only needs the basics
should read those and ignore everything else.

| Key | Type | Meaning |
|---|---|---|
| `schema` | number | Format version, currently `1`. Fields may be added within a version; a key will not change meaning or disappear without a new number. |
| `armed` | bool | The tray has OFXR armed: games started now load it. |
| `paused` | bool | Frame generation is paused from the tray (menu or pause key). |
| `multiplier` | number | Frames shown per game frame: `2`, or `3` in 3X. With a game running, the mode in it (a Luke Ross R.E.A.L. VR game runs at `2`); otherwise the configured mode. |
| `generating` | bool or null | Frames were generated in the last second, by OFXR or by AFW alone. `false` while paused and while waiting (loading, focus loss, settings being applied). `null` without a game. |
| `app_fps` | number or null | The game's own frames per second, one-second average: the settings window's "Real submitted". `null` until measured. |
| `output_fps` | number or null | The same valid measured Output FPS as the on-screen meter. SteamVR uses observed compositor presents excluding repeats; other runtimes use the available native output receipt. Unknown is `null`, never the sum of real and generated counters. |
| `display_hz` | number or null | The headset's refresh rate, once OFXR has measured it. |
| `latency_ms` | number or null | Frame latency in milliseconds, one-second median: the settings window's "Cycle minimum", from the game's WaitFrame entry through the real frame's observed completion at the runtime, bounded below by one refresh per generated frame shown before it. Not panel latency. `null` until measured. |
| `updated` | string | When this file was written, ISO 8601 UTC. |
| `ofxr.build` | number | OFXR's internal build number. |
| `session` | object or null | The running game: `process` (executable name), `pid`, `runtime` (`SteamVR`, `VDXR` or `Other`, as the benchmark CSV names it) and `deep_pipeline` (Prefer FPS over latency). `null` without a game. |

## Reading it reliably

- **Tolerate a missing or unreadable file:** the tray may not be running, or
  may be replacing the file at that instant. Keep your last good values and
  read again a second later.
- **Check `updated`:** if it is more than a few seconds old, the tray is not
  running any more (for example, it was killed). Treat OFXR as not armed.
- **A game that crashes** without closing its OpenXR session is dropped from
  the file a few seconds after it stopped reporting.
- **One game at a time:** if two OpenXR games run at once, the file describes
  the one that reported last; `session.pid` says which.

## Telling when frame generation is struggling

`app_fps × multiplier` is what the headset should get when frame generation
keeps up; `output_fps` is what it actually gets. For example, in 3X with the
game at 28 frames per second, the headset should show about 84; an
`output_fps` of 40 means frame generation is delivering about half of that,
usually because the GPU is out of time, and lowering the game's load helps.
`latency_ms` rising while `output_fps` holds means the frames shown are older:
the pipeline is deeper, not slower.
