# Selected RIFE development history

These 32 functional checkpoints are exported onto the released V773 source.
V788-V799 identify the twelve retained early RIFE checkpoints that originally
used private build numbers V733-V745. The private V739 runtime correction was
already public in V746 and is not imported again. Original source identities
are preserved below and in each commit message; original local checkpoints,
tags and release archives are not renumbered.

| Public checkpoint | Retained change | Original source |
|---|---|---|
| V788 | Add portable RIFE motion inference and native-resolution composition | `f1dca5e7` |
| V789 | Preserve the shared RIFE runtime locator during AppData staging | `7d646a66` |
| V790 | Submit RIFE asynchronously and report worker VRAM | `32fd10e1` |
| V791 | Add pinned RIFE 4.4 and 4.3 models | `f0d810ce` |
| V792 | Add ordered RIFE 3X and 4X output generation | `2a36c9aa` |
| V793 | Start the ordered worker for RIFE 3X and 4X | `3a66ed1c` |
| V794 | Preserve the first fatal generation worker failure | `8866009c` |
| V795 | Keep RIFE interpolation fractions independent of XR jitter | `ad871253` |
| V796 | Release ready RIFE real outputs without waiting for future inputs | `ee373615` |
| V797 | Default to RIFE 4.4 and retain compatible profile defaults | `106d0a92` |
| V798 | Extend post-AFW RIFE multiframe ownership and bounded buffer phase | `fcf3bc81` |
| V799 | Prefer the shared RIFE runtime with a portable fallback | `03adb721` |
| V800 | Optimize stereo RIFE with cached TensorRT FP16 and Torch fallback | `71017808` |
| V801 | Release the RIFE producer after output publication | `2a7406d4` |
| V803 | Keep delivery admission waits off the game producer and UI | `3b2d11e0` |
| V804 | Add optional NR second pass with independent stereo histories | `6a60502b` |
| V809 | Retain adaptive SteamVR pacing reserve and buffer state | `1295bf07` |
| V810 | Support quad-view NR and foveated FSR guide regions | `779c03cb` |
| V811 | Handle NR array guides and stable eye identities without the rejected copy queue | `63f93128`, `f14f8dcd` |
| V813 | Prepare buffered frames during native Begin and preserve guide chains | `aafeedf0` |
| V819 | Integrate RIFE 3X and 4X with independently owned OpenXR panels | `e711d552`, `bf01196f`, `bcf3a9f2` |
| V821 | Use qualified native SteamVR timing only in Director and preserve sub-refresh spacing | `5accb5c1`, `20ee765c`, `dd8fff53` |
| V823 | Fix private RIFE 3X promotion across overlapping application frames | `76042591` |
| V824 | Give Director Normal three delivery credits independently of Buffer | `bb99dbe2`, `e8d04db6`, `3c0df11d`, `66b800c8` |
| V825 | Add a refreshing desktop profile dropdown and automatic game selection | `0ad5c1ba` |
| V826 | Isolate RIFE eye queues to prevent cross-eye GPU wait cycles | `9097e4a6` |
| V827 | Enable the Director Buffer reserve with a fourth delivery credit | `4d0edf23` |
| V828 | Add five configurable global keybinds with live input routing | `5d96be82`, `f87f3fdc` |
| V829 | Fix latency telemetry and inherited RIFE multiplier handover | `15a6ba58` |
| V830 | Restore RIFE VRAM metrics and add GPU usage and Quality presets | `6ba5926e` |
| V833 | Keep Advanced mode global and outside game profiles | `6216b44e` |
| V836 | Add pinned RIFE setup, asynchronous runtime checking and complete packaging | `b19f338f`, `ae8fbec6` |

The V811 checkpoint excludes the discarded independent publication-copy queue.
V821 contains the corrected Director-only native contract and sub-refresh
spacing, retaining Framework as its reference. V819 adds RIFE-specific panel
ownership only; generic panel support was already released.

Public OSR, peer-position, scene-cut, marker, GTA feature and Fast-panel fixes
are retained from V763-V773 rather than published a second time. Their existing
public history remains authoritative. V836 also carries the additional regression
coverage and equivalent NGX predicate extraction from private V835.

Unretained diagnostics and prototypes V802, V806-V808, V814-V818 and V822
are omitted. Integration-only merges and the V812 rollback are not separate
functional checkpoints. Selected tests and documentation accompany their final
feature. This history is not a claim of fresh game/headset acceptance for each
intermediate checkpoint; see the RIFE guide for supported routes and limits.

The public export also corrects two old verification fixtures. Native Director
source arrivals use absolute periodic deadlines, with a bounded requalification
window and exact image/cycle checks. Foveated FSR fixtures prepare the independent
panels before counting projection startup and check panel source-pair ownership
separately from interpolated pose. These are test-harness changes; runtime
rendering, guide capture and pacing stay identical to the validated V836 source.
