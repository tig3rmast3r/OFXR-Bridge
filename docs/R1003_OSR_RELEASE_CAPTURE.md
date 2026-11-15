# R1003: retain OSR source colors with inactive FSR selected

R1002 fixes the inactive FSR admission gate at EndFrame. The owner's next Ghost of Tsushima log reaches OSR, but all release-time color captures are still skipped: FSR is saved as provider, framegen is disabled, and no pre-resolved native stereo guides exist. OSR intentionally resolves guides later against its captured colors. Discarding those colors also prevents the fresh-eye receipt from being recorded, leaving reconstruction permanently unavailable.

Only active FSR generation can suppress a split-eye capture for absent native guides. With framegen off, both real release colors are now retained and the arriving logical eye is recorded normally, allowing OSR's exact marker/depth/motion join to proceed. No guessed alternation, fabricated motion, guide-check relaxation or new history algorithm is introduced. R1001 original OptiScaler N-1 and native NVIDIA contracts remain unchanged.

A focused regression uses the actual shipping release function and D3D11/D3D12 split-eye GPU capture: FSR selected, framegen off, OSR on, no pre-resolved guide set. Both capture tickets and arriving-eye receipts must survive. It reproduces the R1002 failure before this correction; live OSR quality remains a separate owner check.

Use the same OptiScaler/INI, OFXR framegen off, AER framegen off, OSR on. OSR plus FSR generation is not newly enabled; standard non-AER FG is a separate native-stereo test. Local unsigned build, ABI98401, no deployment or push.
