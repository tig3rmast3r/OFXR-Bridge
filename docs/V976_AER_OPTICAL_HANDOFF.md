# V976: Space AER optical Framework handoffs by one display period

Fast optical AER work can fill both native credits before the runtime supplies backpressure. Apply one physical-period spacing per accepted packet for Luke AER plain OFXR on the Framework path. Rebase late input without catch-up, retaining native display timestamps, FIFO image order and camera metadata. Hybrid, FSR, DLSS-G, RIFE and existing Director/Buffer policies retain their paths.

Validation: D11/D12 regressions cover minimum handoff spacing, late input, exact pixels/poses and live policy changes. The registered WARP check passes. User confirmed smooth GTA stick rotation.

The full test suite was deliberately not run at the owner's request.
This change was accepted in local builds on the V971 baseline.
The complete V972-V977 assembly is built and packaged as V977.
