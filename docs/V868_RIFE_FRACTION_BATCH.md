# V868: RIFE 3x/4x fractional transactions

RIFE now packs all fractional inputs for a source pair before entering CUDA,
executes their inference requests in one worker transaction, then queues their
full-resolution compositions. This removes intermediate D3D12/CUDA handoffs,
pipe round trips and CPU completion waits between fractions. It does not batch
different eyes together or alter RIFE's network/timestep calculation.

Every fractional output has its own imported input/output buffers and command
allocators. Its target views, source rotations, ROI, timestep and output image
remain distinct. The last pack receipt on the same queue retires every pack
and the preceding compositions. The worker replies only after every CUDA
output copy has completed. The caller's existing final fence publishes all
compositions. Failures poison the transaction and retain the existing safe
real-frame fallback; no producer or output fence has been removed.

This applies to asynchronous RIFE 3x/4x, including both private presenters.
RIFE 2x and synchronous callers retain the preceding path. The worker advertises
the optional `fraction_batch` capability within protocol 2; an older worker
uses the previous fractional path safely. Start this release's tray and restart
the game to use the matching overlay. TensorRT model/compiler/cache identities
are unchanged, so existing engines remain valid. Additional scratch buffers
cost approximately 22 bytes per reduced-resolution pixel per additional output,
per source transaction, excluding D3D12 allocation overhead.

With diagnostics enabled, batched `rife_pack_wait`, `rife_infer` and
`rife_backend` records retain the configuration ID and dimensions. The high
32 bits of `c` still give view count; low word 2/3 identifies a batch of that
many fractional outputs, instead of the single-output float timestep bits.
Existing V867 dependency probes remain available.

Validation uses actual D3D12/CUDA/TensorRT, independent queues for two eyes,
fraction-specific camera rotations, and exact pixel comparisons against the
previous fractional sequence for 2x/3x/4x. It also checks both completion
policies, destruction with pending composition, and a worker rejection after
queued work. A receipt proves the new worker capability actually executed.
The ABBA microbenchmark measures the complete two-eye 3x transaction; it is
not a prediction of in-game FPS with NR or other GPU work.
