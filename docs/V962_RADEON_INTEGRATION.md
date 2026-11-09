# V962: integrate the complete Radeon RIFE backend

Second of two independent commits on nightly V960:
1. V961 ports the shared V955 RIFE/SteamVR handoff policy without Radeon code.
2. V962 imports the remaining complete V957 Radeon work (`9cc0514e`), including
   native Vulkan/ncnn inference, preparation/compiler/cache, nonblocking cold
   loading, long Unicode cache paths, tray selection and managed installation.

The full ZIP includes the sealed Radeon installer payload. Use the tray's RIFE
installation dialog to choose NVIDIA CUDA/TensorRT or AMD Radeon Vulkan. AppData
is the automatic destination; previous runtime preservation and staged verification
remain in place. Radeon starts with RIFE4.4 and device-specific preparation.
The first new shape can take time; pending preparation transports real images
and reports zero generated outputs rather than latching a permanent worker error.

Nightly V960's Djules GTA/new-Luke, RDR2/Cyberpunk, AER and clean-off changes are
retained, as are the native-consumer status and RIFE suspension/resource fixes.
NVIDIA's Python/model/TensorRT overlay is unchanged. The Vulkan worker/compiler
and dependency pins are the reviewed V957 implementation, reused with its sealed
payload; hardware validation on Radeon remains pending.

Shared-control ABI is now96201 because Vulkan adds runtime status. Close older
trays and use matching tray/layers together. No live profile/runtime installation
is performed by building or publishing this integration. The pacing policy is
RIFE/SteamVR-specific; other generators and native VDXR keep their existing policy.
