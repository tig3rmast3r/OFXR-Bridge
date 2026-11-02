# V759: AFW-only preference before the refresh lock

An eligible, completed AFW-only result reaching at least 90% of the measured
headset refresh takes priority over generated routes, including those at full
refresh. If AFW 1x1 and 2x2 both qualify, 1x1 always wins; the former relative
10% FPS gain rule is removed. Existing resolution gates still apply.

Without qualifying AFW-only, the full-refresh lock and the ordinary route,
vector, latency and stability rules remain. Unknown refresh, failed or partial
measurements cannot activate AFW priority. The Luke early completion checkpoint
uses the same 90% threshold.

KCD2 V758's 79/87 FPS at 90 Hz now selects AFW-only 2x2. An 81 FPS full-quality
result selects 1x1 even against 90 FPS generation or a faster AFW 2x2 result.
This changes selection only, not rendering, pacing, AFW math or providers.
