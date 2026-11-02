# V823: private RIFE application-frame overlap

Based on V821 dd8fff53, retaining the complete Director and Framework pacing.
The isolated V822 rollback candidate is not used as the base.

When the application calls the next Wait before the current End, the legacy
presenter promotion could mark private RIFE as unsupported for 3X/4X.
Keep the application overlap bookkeeping, but exclude private RIFE from that
legacy promotion. Other engines, GPU queues and native delivery are unchanged.

The real application capture retained only RIFE2X; the specific 3X failure was
reproduced with actual layer calls and overlapped Wait/End, then corrected.
This is not evidence that TensorRT compilation or every VR entry failure is fixed.
Five GPU cases cover 2X,3X,4X and live2X->3X->4X->3X->2X with Framework/Director.

The experimental post-preparation native Wait is rejected and excluded: while
it maintained full refresh, it lost compositor deliveries below refresh.
Buffer/Fast latency inversions remain unresolved; no pacing change is claimed.
