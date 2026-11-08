# V916: Windows-safe offline TensorRT progress

V915 could stop a found-game precompile batch with `WinError 5` while
publishing `*.progress.json`. The failure was in progress reporting after
engine preparation, not an engine compiler failure. The exception handler
attempted the same progress write again and could exit without a final result.

The PowerShell progress reader now opens JSON with read/write/delete sharing,
closes the stream before parsing, and permits the writer's atomic replacement.
Offline progress and final-result publication retry Windows errors 5, 32 and 33
at most five times (385 ms total backoff). Other I/O errors are not retried.
If progress still cannot be published, its first failure is logged and the batch
continues; subsequent refreshes can recover. Engine failures and cancellation
still determine the final status. Publishing the final result remains mandatory.

The changes are confined to offline tools. Runtime overlay files, TensorRT graph
and cache identity, engine validation, runtime leases and live inference paths
are unchanged. Existing valid engines are reused and checked as before. No
administrator rights, runtime reinstall, cache deletion or permission changes
are required.

Focused checks cover a real Windows handle blocking replacement, release and
retry, bounded permanent failure with old JSON preserved and temporary files
removed, genuine I/O failures, the production PowerShell reader during 500
concurrent atomic replacements, complete batches despite blocked progress,
real compilation failure and cancellation reporting. Existing progress/cancel
window fixtures exercise the actual PowerShell window without GPU or downloads.

## Horizon V915 first-frame refusal

Both owner attempts (PIDs 55860 and 43460) returned
`XR_ERROR_CALL_ORDER_INVALID` (-37) from the first downstream EndFrame.
The private endpoint logged refused-end reason 1 with no begun output frame.
Luke then destroyed its OpenXR instance and the game stayed flat; the logs
show a handled OpenXR shutdown, rather than a Windows process crash.

V915 admitted the November HFW layout and re-enabled the existing fresh-only
application clock. Its application Wait/Begin are predictions. With the meter
object present, early EndFrame reservation ran before the content callback
which opens the corresponding output Wait/Begin. Reservation therefore failed
before the callback could execute. Output-cycle opening is now shared by
reservation and submission, and runs before reservation, once per output.
Normal application clocks and an already-active presenter retain their owners.
No duplicate Wait/Begin is added on submission.

A focused test uses the actual layer implementation, private endpoint and
meter object: first and repeated frames, ordinary/fresh-only clock, and meter
present/absent. It uses empty startup frames and a test-owned D3D12 device;
no game, headset, RIFE inference or general pacing matrix is involved.
