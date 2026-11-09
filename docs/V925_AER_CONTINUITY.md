# V925: AER source continuity and mixed-output measurements

Local trial based on V924; restart the matching tray and game (ABI 92501).
Use Frame generation > Advanced > AER framegen, with the usual generator.
Swap remains a paired temporal-phase option; physical eye identity is retained.
The existing 2X limit and ordinary pacing/worker paths remain unchanged.

The source-eye cursor now belongs to the observed application stream, separately
from the generator's retained anchor. Retiring a generation job, a refused
output or a menu/discontinuity cannot turn the next alternating eye release
into a newly observed stereo pair. A missing generation anchor still forces
priming: the change never interpolates across dropped source samples. Held-eye
camera/FOV metadata follows the observed pixels even across generator resets.
No image readback, fingerprinting, extra GPU pass or GPU wait is introduced.

Every successfully retimed AER presentation contains one exact raw eye and one
reconstructed eye. The meter labels these contributions **Real eye equiv.**
and **Gen eye equiv.**, normalized to stereo frames: 90 distinct mixed outputs
per second contribute 45 real plus 45 generated equivalents, not 0+90 or 90+90.
Repeats and failed/pending provider outputs cannot inflate the new-output rate.
The physical output cadence remains the ordinary measured output boundary.

Mixed output receipts also supply a cycle reference. OFXR time, input-cycle
length and its minimum use the actual admitted input cycle and the first
successful mixed native submission; a mixed output is not counted as a second
generated refresh before itself. This measures the source-to-output processing
cycle, not the older raw eye's render age, scanout or motion-to-photon latency.
Missing receipts/readiness/producer times remain N/A rather than being invented.

The owner identified the final gameplay segment of DOOM's V924 run as legacy
AER + AER framegen, followed by menu activity; earlier AER v2 experiments are
not treated as the same test. The old source classifier can oscillate after
generation resets, and its all-synthetic mixed-output tag suppresses real-cycle
metrics. CPU/runtime fixtures cover reset/repeat/gap/epoch recovery, mixed FPS
equivalents and actual mixed delivery receipts. In-game/headset cadence,
quality and performance still require the owner's V925 retest.

Source releases remain evidence of releases, not proof of new pixels: a mod
recopying old pixels into a newly released image needs producer metadata or a
separate content detector. This change does not pretend to detect that case.

## Generic all-effects-off delivery

With FG, OSR/AFW and NR all disabled, newly accepted private-runtime packets
use clean bypass: no optional Director/Buffer pacing, no retained paired-content
time and no black-bar prediction shift. Native-cycle validity checks and
renewal of an expired runtime prediction are retained. Turning an image effect back on
restores the selected policy for future packets; accepted frames keep their
immutable ownership. This is generic, not a DOOM title exception.

The hook, private swapchain image ownership, required copies/conversions,
virtual-call bookkeeping and real runtime Wait/Begin/End remain in place.
This is not equivalent to unloading the layer or a promise of zero overhead.
Tests preserve exact FIFO images and balanced frame calls across D11/D12
stale-clock delivery; no live all-off pacing/performance acceptance is claimed.
