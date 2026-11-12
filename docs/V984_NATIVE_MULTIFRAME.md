# V984: Native OpenXR multiframe delivery and reporting

Compatible native OpenXR sessions now activate 3X/4X, publish every generated
image and report the effective multiplier. This consolidates local commits
7fa3652, 6794aa5 and 34611e3 onto origin/nightly b683b68 (V983), with one public
version increment. The accepted local V985 r5 runtime implementation is unchanged;
publication changes version metadata to V984 and adds these release notes.

## Behavior

- Ordinary native D3D12 sessions, including split Wait/End loops, can use the
  ordered multiframe worker. Eligibility follows supported graphics and worker
  contracts rather than a VDXR-only allowlist.
- Compatible unsplit Luke Ross OSR/AFW sessions with an isolated synthesis
  queue publish S2/S3 using their own D3D11 completion receipts. Unsupported
  interop, single-threaded D3D11 and AFW-only combinations retain their gates.
- Native optical 3X/4X can prepare the next batch in separate output banks
  while the previous batch is delivered. Each output is released at its own
  submission. Fast mode retains the output capacity needed for every frame.
- Outside VDXR, new ordered native multiframe delivery uses runtime WaitFrame
  pacing with a half-period minimum from the preceding EndFrame return.
  VDXR and private SteamVR keep their existing clocks. RIFE 2X retains its
  original overlap behavior.
- Ordinary native Pimax D3D12 uses an independent runtime binding queue.
  Ready output delivery cannot inherit future game work. During ordered
  3X/4X, the producer retires its exact source receipt before image release,
  outside the GPU lock; it does not put that pending wait ahead of delivery.
  The binding is immutable and therefore also applies to Pimax 2X, which
  keeps asynchronous image-release ordering. VDXR/SteamVR queue policies and
  Luke/interop transports are unchanged.
- Packed stereo guide matching handles two recent, unambiguous eye outputs
  rounded up to four pixels. It crops guide rectangles to the actual eye
  extent and preserves the right-eye seam. Exact matches retain their path;
  unrelated sizes, mono/UI images, stale and ambiguous pairs remain rejected.
- `status.json` reads the effective runtime multiplier, including capability
  fallback and OSR; without a session it reports the configured multiplier.
  JSON schema stays 1. Control ABI 98401 requires matching tray and layer.

## Evidence

The user accepted ordinary multiframe in MSFS 2024 on SteamVR and VDXR in all
tested modes, and a DI2 sanity run. The separate OSR extension was also accepted.
For the final native-runtime work, MSFS 2024 and DI2 on VDXR worked at r4;
DI2/Pimax still lacked visible intermediate motion despite successful native
submissions. The user confirmed r5 working after Pimax queue isolation.

The restored-resolution Pimax run had matching 4340x4284 guides and eye images,
successive display predictions near 11.111 ms and positive display-time lead.
A separate run at 3798x3748 per eye published 3800x3748 DLSS output, reproducing
the strict guide-matching rejection. These were distinct problems.

The accepted r5 source passed 100 focused checks: native/SteamVR 2X/3X/4X,
live switches, batch overlap, per-output publication, post-OSR GPU content,
Pimax queue dependencies, guide alignment and vector GPU checks, pacing and
status reporting. Native fixtures now consume on the actual selected binding
queue; the old fixture silently gave native runtimes SteamVR's independent
delivery queue. The SteamVR queue-policy fixture also needed explicit session
routing instead of its Legacy default. Its initial failure reproduced alone
and passed after that fixture correction. This is not a full-suite claim.

Historical test details and sealed r5 hashes are in
[the local acceptance record](NATIVE_MULTIFRAME_STATUS_TRIAL.md).
The publication build's checks and artifact hashes are recorded in its
`BUILD_PROVENANCE.json`. Native accepted submission FPS is not independent
evidence of physical headset scanout. User acceptance covers the reported
runs, not every supported runtime/mode combination.

Use matching V984 tray/runtime files and restart the game. V984 runs log to
`RuntimeLayer/v984`, distinguished by PID and timestamp. The build process
does not install or arm the layer or change live game settings.
