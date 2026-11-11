# V977: Restore OSR when global frame generation disables saved AER

Guide capture uses effective AER state at startup and live reconfiguration. A saved AER selection cannot suppress Luke OSR guides when the global frame-generation switch is off. Disable the remaining generation panel, show AER as off, and preserve saved preferences for re-enable. Reconstruction and Neural Rendering remain independent.

Validation: 1295 shared desktop/headset widget checks, effective-guide policy/geometry and meter checks pass. User confirmed OSR engagement with global generation off and AER still saved.

The full test suite was deliberately not run at the owner's request.
This change was accepted in local builds on the V971 baseline.
The complete V972-V977 assembly is built and packaged as V977.
