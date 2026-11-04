# V851: NR startup observation correction

Based on V850, preserving exposure normalization, post-SR staging, checked composition, exact submitted-fence protection and F8 witnesses.

The V850 PID18988 capture contains no NR evaluations. Its log shows desktop feature1 initialized, then observed again before its initialization command list was submitted. V850 incorrectly latched the entire NR session unavailable on this legitimate pending condition. This was not a failed submitted GPU evaluation and must not trip the watchdog.

V851 defers that observation without touching in-flight resources or marking NR failed. It must not block the thread that still owes ExecuteCommandLists. The same safety rule applies to pending evaluations: no state reuse before actual submission/retirement. The100ms watchdog and explicit OFF->ON recovery for an actual submitted evaluation remain unchanged.

Targeted host tests repeat both stereo handles before initialization submission, assert no model evaluation/state reuse and no sticky failure, then submit and verify both NR passes run. Existing controlled24ms fence wait, true100ms watchdog latch/retry and post-SR array-slice preservation remain checked. No other NR rendering/guide/model/pacing/UI change.

This fixes the empty NR capture/startup regression. It does not diagnose or claim to fix the photographed ghost contour. The next F8 with2-pass NR active will contain original, pass1 and pass2 for that diagnosis. No settings change is required to take it.
