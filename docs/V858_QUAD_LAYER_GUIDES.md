# V858: quad layers and panels no longer resolve game guides

## Symptom

Dead Island 2 (UEVR) with Hybrid at 3X showed a red "Queue mismatch" while
Hybrid used the guides.

## Cause

UEVR's 1280 x 720 UI quad was resolved against the game's guides at every
release. No DLSS output is its size, so each resolve ended in a queue
mismatch, about 40 a second. The guide status is the last writer's: Hybrid's
"used" and the quad's failure alternated, and 3X's timing left the failure
shown.

## Change

A swapchain only ever submitted as an auxiliary layer, or a panel source, is
no longer resolved: neither takes game guides.

## Flight log

`luke_afw_stage` a=1195: which queue-mismatch check refused a guide, result =
site 1-9. Logged when the site changes and every 256th time.

## Test

Dead Island 2: Hybrid 3X without the red status.
