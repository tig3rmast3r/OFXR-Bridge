# V929: native AER guides, method switching and source timing

Local trial based on V928. Public layer/tray ABI remains 92501.

- Native AER marker reads use a separate DIRECT queue, waiting only the exact
  retained colour-capture fences. The bounded read deadline is 32 ms instead
  of 8 ms. A timed-out read retains its own ROI for non-submitting completion
  polling; a later colour cannot replace that readback before decoding.
- Confirmed native N1 vectors traverse the exact intervening publication to
  the previous same-eye endpoint. The existing GPU guide composer performs
  this conversion for AER FidelityFX, native DLSS-G and Hybrid. Confirmed N2
  keeps its existing path. Missing/ambiguous history still rejects generation.
- Leaving asynchronous RIFE restores the original session transport queue,
  after draining the outgoing method. Repeated switches keep that ownership.
- OSR-only delivery now carries the reconstructed packet's actual source
  stamp and native completion receipt instead of an empty generation packet.
- AER source-cycle timing includes the measured earlier same-eye endpoint
  needed for interpolation. CPU call time remains separate; no fixed display
  period is added. These are application/runtime timings, not measured scanout
  or motion-to-photon latency. Missing native completion remains N/A.

The owner accepted V928 Framework AER and RIFE/OFXR switching in GTA. Ghost
performance/switching and native vectors in both games still require a new
headset run; fixture checks cannot establish those live results. No game
profiles, live DLLs, remote branches or releases are changed by this build.
