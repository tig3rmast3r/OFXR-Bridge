# V809: adaptive SteamVR buffer on the RIFE lane

Based on stable V805, retaining V800 RIFE, V801 handoff, V803 admission/auxiliary,
V804 optional second NR pass and the V758/V759 public fixes. Public V759 remains
the chosen 0.3.2 release candidate pending the owner's Luke Ross Quest tests.
All new work continues on the 800 lane. No Forza diagnostics from V806/V807.
The first V762 implementation used the wrong pure-nightly base and is retained
only as an implementation reference; it is not the requested trial.

Increase buffer supplies three bounded delivery credits instead of two. On
SteamVR with OFXR enabled it now also builds and retains an adaptive timing
reserve in Framework, Director and the advanced combined policy. The common
terminal transport covers optical/vector generators and RIFE, Normal/Fast,
and their supported output multipliers. Native VDXR Legacy, native Pimax,
direct AFW-only and the two-credit policy retain their paths. Other private
runtimes retain their V805 capacity/clock policy.

The existing native VDXR OpticalPacingReserve learner is reused unchanged:
64 successful output intervals, near-refresh 94–104% qualification, bounded
lateness/jitter, at most 0.5ms growth per window and 0.75 display period total.
Quiet output retains the learned reserve; sustained slow output does not train
it. No displayed FPS estimate trains the controller.

Framework uses the unchanged Legacy TwoOutputSchedule for its intended final
cadence. Native Wait can overlap accepted input preparation; exact GPU fences,
copies and FIFO ownership still gate delivery. Each newly learned increment
shifts that cadence once. The runtime supplies every actual prediction and
Begin/End, including existing expired-cycle renewal.

Director applies the retained offset once to each RAW physical-clock sample.
It does not alter the display period or accumulate offset on resampling. Small
learning steps therefore do not each skip a complete display slot. Without
physical feedback the existing fallback phase receives the same incremental
hold. Native Wait lookahead includes the learned reserve, with 0.5ms initial
additional lookahead to enter the near-refresh learning range. That initial
lookahead advances Wait only; it adds no historical per-packet age floor.

V805 already contained an older three-credit Director clock smoother. The new
SteamVR adaptive mode bypasses that old smoother to avoid smoothing away the
deliberate reserve changes. Other private-runtime policies retain it. The
physical schedule class itself is unchanged from V805; V749 is not imported.

Director feedback includes sub-slot preparation/native-Begin lateness against
the preceding target plus one period. Rounding a slightly late packet to a new
slot must not erase that deficit. Deliberate refill is excluded from this signal.
There is no acceptedAt + historical-service eligibility floor and no output drop,
duplicate or source/pose substitution. Timing reserve is bounded; storage still
backpressures through existing credits and generation leases.

Buffer/pacing changes drain accepted outputs before switching policy. Backend,
enabled-state, session and refresh changes reset learning. Buffered reconfiguration
drains before capture/GPU locks. OFF clears adaptive timing even with saved
capacity three. V803 owned EndFrame reservations and auxiliary handling are
retained. No generator, shader, model, provider, NR or shared-runtime changes.

F9 private transport stage29 (flight code5739) records packet/reserve/increment;
stage30 (5740) records application of a learned increment. Learning and timing
operate with logging off. Benchmark columns and the existing buffer preference
remain; the build number identifies the changed ON behavior.

Automated validation uses deterministic learner tests, actual D11/D12 GPU
resources and real generators with a mocked downstream XR runtime. It checks
bounded one-time refill, retained quiet reserve, slow-input exclusion, refresh
reset, exact pixels/poses/order and OFF reset. The route matrix includes both
pacing modes, vectors, 3X and retained RIFE/NR/admission coverage. This does not
establish game/headset pacing gains; hardware acceptance is still pending.

Only the small normal build is packaged. The existing single shared RIFE runtime
is reused, with no model/runtime copy or promotion. Nothing is installed or
published by preparation of this local candidate.

Final validation: 64 unique checks pass. The verified receipt combines the first
run's unchanged-production passing cases with final checks for corrected test
registrations. Two new three-credit cases were initially sent to the deliberately
two-credit overlap probe, whose strict capacity assertion correctly rejected them;
they now use the existing tail/no-future GPU identity probe. The original two-credit
overlap checks still pass unchanged. A missing multiframe test executable was
then built and its test passed. Original failed/not-run receipts are retained;
no production assertion was weakened and no headset improvement is claimed.
