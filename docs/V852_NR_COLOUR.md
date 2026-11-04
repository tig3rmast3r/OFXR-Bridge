> Superseded in V862: the owner reported no visible improvement. The additional relative-colour shader was removed; NR capture, exposure, post-SR staging and initialization fixes remain.

# V852: preserve original HDR colour when the NR model makes no edit

An unchanged blue HDR patch changed colour when only the surrounding image changed: with an identity-model output, the V851 resolve returned approximately(3.60,14.39,14.52) from original RGB(3,12,40) in a dark context, but returned the original blue in a bright context. Colour strength0 preserved it in both contexts. The absolute model-colour transfer was importing proxy clipping/desaturation into the final HDR picture.

V852 transfers the model's chromaticity CHANGE relative to its own proxy input onto the original HDR chromaticity. Colours are compared at equal luminance in OKLab, while keeping the accepted luminance response and gain ceiling. Identity model output returns the original pixel exactly, including saturated highlights. Real model colour changes remain effective and Colour strength still controls their amount. Near-black inputs without usable chromaticity retain the luminance-only result rather than amplifying an unstable colour ratio.

The replacement resolve applies only to the exposure-normalized HDR path introduced for MSFS R11G11B10 colour. It uses the existing resolve dispatch, descriptors and images: no extra image allocation or GPU pass. Other formats, Colour strength0 and Inspect views keep the accepted codec bytecode. Compare side-by-side/wipe, swap, aspect fit and separators are covered. Model input remains an encoded HDR proxy and may look different from the final game image; that alone is not an error.

Includes V849 F8 NR witnesses, V850 post-SR staging/exposure/retirement and V851 safe pending-initialization handling. The submitted-evaluation100ms watchdog still reports a failure rather than alternating NR on/off; it is not a fixed latency reserve. Failed NR can be retried with OFF->ON. F8 requires logging and an actual active NR evaluation.

Validation: focused real-D3D12 shader/host tests for neutral dark/bright contexts, original chromaticity with Colour0, non-neutral model outputs, Compare/Wipe and swap; existing codec, stereo/quad/array, startup, delayed-fence/watchdog/retry and recorder checks. Four frozen MSFS packed-HDR sources were replayed on NVIDIA through the new resolve with an identity model, zero D3D12 validation errors and exact original colour. These tests mock neural inference; live game image quality, all camera-dependent colour changes and the reported2-pass ghost contour are not certified fixed.

The nightly push contains separate V849,V850,V851,V852 commits for review. RIFE/model/installer, frame generation, pacing, UI and the provisional benchmark suite are preserved. No stable release/main/tag changes.
