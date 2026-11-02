# V824: Director Normal owns its required delivery capacity

Based on V823 (including the private RIFE3X/4X overlap fix).
The first step requested by the owner is Director Normal, Buffer OFF and Fast OFF.

Native SteamVR Director now always has three independent delivery credits:
current native delivery, prepared successor and producer. Previously the Buffer
checkbox was required to obtain the third credit. Framework retains the selected
two/three credits, including when switching live back from Director. VDXR/Legacy
and private prototype transports are unchanged.

The native expiry check reuses the existing100us handoff preparation guard.
It does not change the display time, physical cadence or sub-refresh spacing.
This targets the expiry boundary observed during the V823 qualifier tests.

Fast retains its existing reduced swapchains and memory behavior. No generator,
worker, provider or inference algorithm changes are made. The Director Buffer
still has the same three credits as Normal at this first step: its additional
adaptive timing reserve and a possible fourth credit are subsequent work, not
claimed as fixed in this build. The tooltip describes the actual current behavior.

Validation must cover full refresh and sub-refresh native compositor receipts,
bounded admission, exact GPU pixels and returning to Framework2/3. Headset/game
acceptance is required; synthetic results alone cannot prove game smoothness.
