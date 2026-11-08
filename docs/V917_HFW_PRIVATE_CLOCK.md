# V917: one frame-admission owner for Horizon on the private endpoint

The owner's V916 HFW run (PID 56728, 2026-11-08) entered VR but stopped updating
after level loading. Its retained tail shows source identity 4190 repeatedly
rejected as a duplicate (`luke_filter=11`), application Wait returning in 3-10 us,
and more than one million application tickets in about 52 seconds. The retained
benchmark records its last private physical delivery at 21.435 seconds. The
application continued calling EndFrame successfully while no new packets reached
the headset; it did not fail with V915's first-frame call-order error.

V915's November HFW layout recognition reactivated the older inline fresh-source
clock. That clock predicts application tickets and can consume an EndFrame
without runtime delivery when its CPU identity is held. A private endpoint
already owns Wait/Begin/End admission and its physical delivery FIFO. Combining
these owners allows a held HFW identity to suppress all physical submissions,
with immediate private predictions producing an unbounded application spin.

After a private endpoint is successfully installed, it now remains the sole
frame-admission owner: the inline HFW fresh-only clock/filter is disabled for
that session. The November/September layout recognition and read-only identity
reader remain available. AFW/OSR input identity still comes from their existing
color markers and release-bound guides. No shader, pose, motion interval,
RIFE/NR, engine cache, or private FIFO/cadence algorithm is changed.

Sessions which do not install an endpoint, including native Legacy and
AFW-only, keep their existing inline clock. The log emits `luke_filter=16` when
installation supersedes the inline clock. The V916 progress-file and initial
output Begin/admission fixes are retained.

Focused coverage calls the production endpoint installer, then the actual
application Wait/Begin and meter/End admission for repeated frames, with a held
source identity and meter present/absent. It checks sole endpoint ownership and
drains accepted packets. The earlier first-frame regression and unchanged
layout/GTA/motion-interval tests remain included. These are test-owned devices
and runtime callbacks, without launching a game or headset benchmark. The
owner's actual loading-to-gameplay transition still needs live confirmation.
