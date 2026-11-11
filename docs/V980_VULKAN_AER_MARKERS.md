# V980: Vulkan AER markers independent of OSR

Vulkan's native-upload and D3D12-copyback marker paths previously required a
nonzero saved reconstruction mode. With OSR off, AER Hybrid/FSR/DLSS-G could
capture guides but never stamp their original colour identity. The reader
remained pending with no native output extent and generation stayed in bypass.

Request markers for the active consumer: AER Hybrid/FSR/DLSS-G independently of
OSR, or requested OSR when AER is inactive. Pure optical and RIFE AER do not
request native guide markers. Turning global generation off restores the saved
OSR demand. Both existing Vulkan marker transports use the same decision.
Stage 1191 bit2 records marker demand; bits0/1 retain copyback/native transport.
No new capture scope, guide admission relaxation or synthesis algorithm change.

The owner confirmed The Great Circle working in the V979-based r1 test build.
An enabled saved OSR mode could already satisfy the old gate, explaining why
another tester might not reproduce the failure. This is conditional, not a
claim about that tester's actual settings.

Focused checks cover the mode matrix, actual Vulkan/D3D12/Vulkan GPU marker
round trips with OSR off, camera contracts, unrelated-process exclusion and the
colour codec. x64/x86 builds passed; full suite deliberately skipped by request.
Release version remains 0.3.3; IPC ABI remains 97101.
