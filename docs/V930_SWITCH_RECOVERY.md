# V930: native consumer status and RIFE switch lifetimes

Local trial from V929, ABI92501. No merge, remote push or live profile edit.

- Native guide status now follows the current session's fresh submitted
  game-motion receipt. A legacy/held-eye mismatch cannot replace a working
  consumer's Ready state. Missing proof, expiry and actual provider failure
  remain visible. Optical fallback is never proof of native-vector success.
- AER Swap remains Advanced-only and is reset to its default in Basic, including
  old saved values. Desktop and headset use the same settings policy.
- RIFE suspension now produces a transport-only context rather than recreating
  another RIFE worker. All shared-history eye consumers retire before provider
  replacement; old RIFE working resources are released before the first new
  eye is allocated. Lazy restart establishes its private queue before producer
  synchronization. Completed publication fences remain valid.
- Partial CUDA buffer construction and failed configure close their imports,
  mappings and unconsumed duplicated handles. Model math, engine cache keys
  and TensorRT/compiler code are unchanged. The packaged worker overlay is updated.
- Drained private marker readers release their queues instead of waiting for
  the process-wide LRU. Failure to allocate an optional marker queue on a
  healthy device keeps real-frame delivery and uses bounded retries; device
  removal remains an error. Stage1440 records the exact queue failure.

Owner GTA V929 native generation works; session loss followed a refused RIFE
allocation. Existing logs show memory dropping on switches and a stable final
worker sample; a continuous leak across healthy switches was not established.
The fixes address concrete resource lifetime, transition-peak and refusal paths.
Actual game/headset endurance, quality and Ghost performance remain owner tests.
