# V971: evidence for AER frame generation

The Elliott meter now reports AER independently of ordinary generation FPS.
Enabling the checkbox or successfully preparing a pair cannot make this row
active. ACTIVE requires a fresh retimed mixed-eye output, a positive generator
result (where the provider supplies one), and successful native EndFrame.
Private virtual admission alone is insufficient. This is native submission
proof, not a claim of physical headset scanout or correct source timestamps.

- Out: confirmed new retimed outputs in the applied generator configuration.
- Byp: non-retimed outputs or refused generation/delivery attempts.
- ?: bounded receipt observation expired or could not be retained; never success.
- BYPASS supersedes earlier success. Proof expires after one second without
  confirmed output; disabled generation, settings application and disconnection
  take precedence. Held repetitions do not increase Out.
- Each generator reconfiguration has a separate observer. An old delayed
  receipt cannot activate a newly selected method or phase.

Input classification remains separate: alternating releases do not establish
pixel freshness when a mod recopies old content into a new release. Paired
releases explicitly say `AER phase assumed`; they cannot distinguish simultaneous
stereo from staggered AER without additional producer/pixel timing evidence.
Swap still reverses the assumed phase and never exchanges logical eyes.

Observation uses existing GPU result and native-delivery receipts. No GPU query,
readback, synchronization, scheduler, interpolation or pose policy is added.
The pending observer is bounded to 64 outputs and one second; only AER-tagged
new content is tracked. Ordinary all-off delivery creates no AER receipts.
IPC ABI is 97101, so tray and both layer architectures must be kept together.
F9 presenter_transition 9231 logs confirmed outputs (a), bypasses (b), and
`(unknown << 2) | confirmed_source_delivery` (c), at most once per second.

Validation covers meter state/colour, GPU-pending versus native-pending versus
confirmed/refused outputs, held output exclusion, configuration replacement,
and shipping D11/D12 AER alternate ingress/delivery on CPU WARP. Headset UI
validation remains a user test; the counters do not certify subjective quality.
