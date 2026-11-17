# UEVR menu resource trial (V984, round 4)

## Evidence and corrections

Reporter PID13476's flight log records panel arming failures at 165.948 s
(`swapchain_eligibility` reason12, synthetic private allocation) and 203.578 s
(reason11, current private allocation). In both records b is unsigned -10,
XR_ERROR_LIMIT_REACHED. Stage761 independently records failure to arm these
panels. The earlier diagnosis missed these reasons by checking only reason13.
The archive contains optical OFXR, not the subsequent reported RIFE/F10 run.

The earlier 17-versus-17 colour-buffer comparison covers ordinary projection
sources only. Generated auxiliary/menu panels followed a different allocation
path. V986's native allocation change excludes private SteamVR and is not this
fix. No claim is made that these defects explain every reported slowdown.

## Independent fixes

1. **Pool a panel's native publication destination.** Ordinary private D3D12
   eye outputs already opt into source-scoped pooling. `arm_panel_generation`
   did not enable it for auxiliary panel sources. At 4X one source retains two
   real and six synthetic independent work images, but previously allocated a
   native swapchain for every work image. It now shares one compatible native
   destination for that source, while retaining all eight work images and their
   existing descriptor/CPU/GPU reader ownership. Different panels never share
   destinations. Luke, bridges and native-runtime allocations retain their paths.

   For a three-image native runtime, the output group changes from 8*(3+1)=32
   source-shaped images to 3+8=11. This is a 65.625% reduction in that panel's
   output group, not total process VRAM. At 2560x1440x4 it models about 295 MiB
   saved per panel before alignment. Application/history/flow/capture/driver
   resources are outside this calculation. The GPU fixture measures actual
   GetResourceAllocationInfo sizes, one native sink per panel and eight private
   work images; the old-bank equivalent is labelled as a calculated comparison.

2. **Defer busy auxiliary retirement.** A CompositionImageSlot CPU use count of
   one does not prove its private endpoint has retired its reader/native
   consumer. Optional unused-output cleanup in application EndFrame called
   blocking destroy and could wait behind that reader. Private output cleanup
   now uses the existing try_destroy_swapchain API and retries on a later frame.
   An in-flight source stays intact. Ordinary application destruction remains
   blocking, and teardown retains its existing drain contract.

These are independent changes and must remain separate logical commits if
later accepted for publication. This trial is uncommitted and keeps V984.
Round-1 timing and round-3 bounded completion-delay recovery are included.

## Reproductions

- Two unused-UI retirement fixtures hold an actual private endpoint reader after
  releasing all CPU descriptor owners. Both block on old production cleanup,
  fail the nonblocking-retirement requirement, and repeat individually. Candidate
  returns immediately, retains the in-flight pixels, and releases the unused
  output on a later call after native retirement. Both pooled and unpooled cases
  finish with no native handles remaining.
- A shipping worker/transport test with two independently moving 4X panels and
  a ten-native-handle cap fails to arm both panels before the allocation fix;
  it repeats individually. Candidate retains both generators. The strengthened
  candidate fixture uses three native images, checks separate publication sinks,
  real GPU panel pixel identity and poses, and actual output allocation sizes.

The runtime is simulated; graphics resources, shaders, worker and endpoint
ownership are production implementations. Full-suite and reporter/headset
acceptance remain pending. No settings, registry, NVIDIA profile, installation,
arming, commit or push is part of this investigation.
