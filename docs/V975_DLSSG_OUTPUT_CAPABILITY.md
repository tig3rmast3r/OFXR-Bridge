# V975: Clamp newer DLSS-G capabilities to supported output capacity

A provider reporting more generated outputs than OFXR supports remains available. Clamp positive isolated and shared capability answers to the three-output transport capacity instead of rejecting maxima above three. Zero or failed capability queries remain unavailable.

Validation: The packaged provider reports five generated frames on the tested GPU. Isolated AER N1/N2 GPU generation and ownership-alias checks pass without shared NGX initialization; user confirmed DLSS-G gameplay.

The full test suite was deliberately not run at the owner's request.
This change was accepted in local builds on the V971 baseline.
The complete V972-V977 assembly is built and packaged as V977.
