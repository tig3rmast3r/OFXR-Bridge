# Interface language and display scaling

English is the default. Open Settings (or the headset menu) and use **Language**
at the bottom of the left rail to choose **English** or **简体中文**. The same open
window switches in either direction. Existing game profiles are retained.

The preference is stored in `%LOCALAPPDATA%/ofxr-bridge/interface.ini`:

```ini
[interface]
language=zh-CN
```

Use `en` for English. Missing or unrecognized values use English. The preference
is global and separate from the active game's configuration, shared Settings
layout, runtime messages and CSV schema. Open desktop/headset interfaces poll it
at most four times per second, so a choice in one process reaches the other.
Native tray menus and dialogs use the selected language when next opened.

## Display pixels

The desktop settings backbuffer follows the current client size in physical
pixels. Fonts are rasterized at the monitor's DPI, and Win32 input is converted
to logical coordinates before duplicate filtering. The initial window fits the
monitor's work area; it can then be resized. Short windows can scroll status
help instead of cutting it off.

The headset keeps a 1024x768 logical canvas for layout, hit testing and drag
coordinates. At session initialization its physical texture is chosen from the
submitted eye viewport width in quarter-density steps between 1x and 2x,
subject to the runtime's maximum image size. For example:

| Eye viewport width | Menu texture (with sufficient runtime limits) |
| --- | --- |
| 1024 | 1024x768 |
| 1536 | 1280x960 |
| 2048 | 1536x1152 |
| 2688 or greater | 2048x1536 |

The shared UI atlas follows that allocation. Selection stays latched for the
session, preserving GPU resource and submission ownership; live game viewport
changes do not cause per-frame texture reallocation. Menu angular size and
pointer movement remain independent of physical texture density. This does not
change game rendering resolution. More UI pixels use more UI memory and GPU time.

## Translation maintenance

`src/ui/locales/zh-CN.json` maps the original English display strings to Chinese.
Unknown strings fall back to English. Widget IDs remain derived from canonical
English labels in either language. Hidden IDs, configuration keys, logs and
runtime decision strings must not be translated.

After editing the catalog, run:

```powershell
python tools/generate_ui_translations.py
python tools/generate_ui_translations.py --check
```

The committed generated table means normal Windows builds do not need Python.
Chinese glyphs are merged into the existing Latin faces using a redistributable
Noto Sans SC subset (SIL OFL 1.1), without depending on installed Windows fonts.
After adding Chinese characters, regenerate that subset from the source font:

```powershell
python -m pip install fonttools
python tools/build_zh_font.py 'NotoSansSC[wght].ttf'
python tests/ui_translation_assets_tests.py
```

The generator records the source-font hash. Source and license are documented
in `licenses/ATTRIBUTION.md` and `licenses/Noto-Sans-SC-OFL.txt`. The subset covers
the interface catalog; arbitrary names containing uncommon characters may need
additional glyphs.

## Focused checks

The UI language tests cover default English, persistence, cross-process reading,
English fallback, and both directions through the actual language combo. They
also check unchanged widget IDs across language changes. Native window tests
resize the real D3D11 backbuffer and verify its captured dimensions. Mouse tests
cover the 150% coordinate/deduplication collision and 200% scaling.

WARP/fake-OpenXR composition and atlas policy tests exercise D3D11/D3D12 geometry,
resource ownership, and restricted runtime limits. These are interface checks,
not headset, game-performance, or anticheat validation.
