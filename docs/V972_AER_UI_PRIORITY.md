# V972: Give AER framegen priority over saved reconstruction UI state

AER frame generation disables reconstruction controls while it is active. The sidebar explains that OSR/AFW is off because AER is selected; saved reconstruction preferences are retained. Overlay readiness uses confirmed AER delivery while still showing bypass and provider failures.

Validation: Shared desktop/headset widget and Elliott meter regressions pass. User confirmed the interface behavior.

The full test suite was deliberately not run at the owner's request.
This change was accepted in local builds on the V971 baseline.
The complete V972-V977 assembly is built and packaged as V977.
