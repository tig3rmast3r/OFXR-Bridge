# V905: activate OSR current vectors for interleaved per-eye producers

V904 owner Ghost of Tsushima F8 PID42500,20261107-141848-168-1 and flight log
showed ZERO current-path eligibility (181 history+motion events, bit4 absent).
Frame8435 sourceR stream2293874039672,8436 sourceL stream2293874049320. Native
span2 and temporal camera were valid; previous resources/publications contiguous.
V904 wrongly required peer.stream/epoch==current.stream/epoch, a single-producer
assumption. Its shader prototype was not actually enabled in this game. Owner
reported unchanged and possibly worse flicker; no claim about the latter without
matched motion/scenes. V904 is rejected as a successful game test.

V905 compares each stored eye to ITS immutable native receipt. Source same-eye
history must match current producer/epoch and native N-2 receipt; peer history
must match the previous-opposite receipt at N-1, with its own stream/epoch. Epoch,
publication, reset, native span and temporal checks remain. Existing upstream
native AER validation supplies the chronological producer chain.

The pure identity policy is now called directly by production and by GPU fixture
setup. Captured stream/publication keys, deliberately differing per-eye epochs,
both eyes, wrong handle/epoch/publication negatives, and single-stream positives
are checked. GPU deformation tests activate through this SAME gate, no manually
supplied success boolean. Shader math/resources/passes/cost unchanged from V904.

F8 snapshots own/native history depths whenever history exists, even if current
path is vetoed, so a CPU gate cannot hide its diagnostic inputs again. Flight1310
bit2(value4) reports current CPU eligibility; GPU scene state and per-surface checks
can still reject. Headset flicker improvement remains unconfirmed until owner test.
No pacing/FG/pose/RIFE/NR/AFW/default/profile changes. Local only, no push.
