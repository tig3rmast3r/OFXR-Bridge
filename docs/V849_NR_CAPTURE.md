# V849: F8 Neural Rendering witnesses

Diagnostic build based on V848; no NR rendering, model, shader, pacing or benchmark changes.
The existing configurable Dump key (default F8) retains OFXR vectors and all current captures.
With Diagnostics enabled (F9) and NR actually evaluating, the same F8 folder also contains:

- `nr_source_color`: original game colour, before NR encoding (pre-SR Color or post-SR Output).
- `nr_encoded_color`: full-size encoded proxy before the optional model downsample.
- `nr_pass1_color`, `nr_pass1_depth`, `nr_pass1_motion`: copies recorded immediately before the actual NR call.
- `nr_pass1_output`: raw model answer immediately after a successful call.
- The corresponding `nr_pass2_*` images when 2x NR is enabled; they capture its actual clamped input and independent model answer.
- `nr_resolved_color`: image after NR composition, including the selected Inspect/Compare view.

`nr-evaluations.tsv` joins images by `handle=stream` and `serial`, with pass and before_sr,
result, feature/parameter/resource identities, dimensions, HDR, reset, guide scales, tuning,
and parameter readbacks. The latter are post-call observations, not proof of useful enhancement.
No rows means no model evaluation was witnessed; do not infer correctness from a success counter.
Native views are identified by their NGX handle, not guessed left/right labels.

Use the currently selected pre/post SR mode; the recorder does not change it or enable NR.
For useful normal output comparisons leave Inspect and Compare Off; diagnostic views can also
be captured explicitly. A normal scene, Model input and raw output can all be examined from a
single F8 capture without repeating that scene with different UI modes.

Full F8 only: no automatic NR capture or readback allocations between requests, none in the F9
rolling ring or automatic OSR origin-loss recorder. One exact evaluation per view, max four views,
including both passes: max48 extra images,768MiB total,128MiB per image. Existing OFXR budget
is separate and preserved. Repeated F8 creates separate folders after the previous writer retires.
Unsupported formats, allocation failures and submission timeouts are explicit in manifest.tsv;
never treat incomplete data as valid. Copies restore source states and use native submission
fences before mapping; no synchronous GPU wait added to the render path.

Capture folders remain `ofxr-capture-v23286-<pid>-<timestamp>-<sequence>` beside the game exe;
this historical prefix is shared with existing F8 tooling. Protected/read-only game directories
(such as MSFS WindowsApps) fall back to `%LOCALAPPDATA%/ofxr-bridge/Captures/`. `manifest.tsv` specifies format, source
format, resource size, ROI, row pitch and file. RAW bytes are unchanged (including padding).

Optional offline previews with Python3 (no extra packages):

```
python tools/Inspect-NrCapture.py "path to ofxr-capture folder"
```

Creates `nr-inspection/*.png` and `statistics.json`. Previews are limited to1024px and explicitly
label their visualization transform. Raw files are authoritative; depth previews use min/max,
vectors signed scaling, HDR source/resolve preview x/(1+x). Model proxy remains encoded RGB.

Validation uses shipping host/codec/recorder with D3D12 GPU copies and mocked NGX inference.
It checks pre/post SR, stereo, four views at half resolution including array depth, both passes,
raw readback/writer and independent budgets. It does not certify any live game's NR model output.
No nightly publication: local diagnostic package only.
