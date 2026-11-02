# V826: isolate asynchronous RIFE GPU queues

Base: V825 `0ad5c1ba960b0dc127ef4ed0f8369e045d114369`.

The owner recorded an MSFS2024 startup freeze with RIFE2X selected, Framework,
Buffer on, Fast off. V825 flight capture pid15204 shows successful initialization
and CPU submission of both prime jobs, followed by repeated one-second waits for
both still-incomplete prime fences. Both first-pair jobs later block waiting for
their GPU pack; the application finally blocks acquiring a private output image.
This is earlier than CUDA inference. The capture does not contain GPU queue packet
ordering, so the exact interleaving remains inferred, not directly observed.

Each swapchain already owns a CPU RIFE worker, but all eyes used the same GPU
synthesis queue. One worker can append a wait for a future producer/acquire signal
before another worker appends the commands and completion signal needed to make
that future producer progress. Queue Wait returns on the CPU while leaving this
cycle on the GPU, explaining successful worker enqueue receipts with unfinished
prime fences.

Only asynchronous RIFE now creates a private DIRECT queue per synthesizer, using
the original queue description (device/node/priority/flags). Each worker owns its
GPU command order; existing history, output acquire, and consumer fences remain
mandatory. No global enqueue lock or CPU serialization is added. Synchronous
RIFE, other generators, shaders, models, private presentation policy, Fast image
allocation, and the V825 desktop profile selector are unchanged.

## Focused regression

`xrfg_d3d12_history_tests rife-eye-queue` places a foreign future-acquire dependency
on the supplied session queue, completes that dependency only after the first eye
reaches the consumer, and primes two independent synthesizers. A 700 ms watchdog
releases only the test-owned fence so the old implementation can drain without
hanging the machine. Both eyes must finish within their 250 ms wait budgets and
preserve their exact distinct input pixels. This deliberately constructs the
queue dependency implicated by the log; it is not a replay of MSFS GPU packets.

The unchanged V825 generator plus this regression failed with `ERROR_BUSY`
(-2147024726) at the first eye, as expected; the watchdog allowed clean drain.
The isolated-queue candidate passed the same regression and exact pixel checks.
All four focused suites passed (67.66 s total): synchronous stereo RIFE,
asynchronous RIFE/FIFO/shutdown, the queue regression, and model2/3/4 at3X/4X
including asynchronous and synchronous variants. After the control ABI bump and
local variable rename, the final executable repeats the async and queue checks.
Build/control version826/82601; matching tray is required.

MSFS startup acceptance remains pending: the reduced test proves the queue cycle
and its isolation, not the exact scheduling history of the captured game. No
headset scene, pacing matrix, live install, registry/profile edits, or publication
was performed.
