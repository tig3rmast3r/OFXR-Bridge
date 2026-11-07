# V897: share headset UI only when SteamVR needs native handles

Build V897 makes separate, compact native surfaces the default again.
No settings option, profile format, desktop UI or benchmark rule changes.

Only the known SteamVR private transport installs an automatic native-count
source. Its CPU-only try-lock snapshot deduplicates actual native destinations,
including shared projection/auxiliary endpoints. The sampled UI counter is not
used for admission. At the opening of a menu/counter pair compute:

    projected_separate_total = current_native_total - own_native_UI_handles + 2

At 13 or less keep two native surfaces. At 14 or more use the shared UI atlas,
leaving margin under SteamVR's observed16-handle limit. Existing own handles are
subtracted exactly once, including an already-shared handle. A busy inventory
defers opening the second UI; it does not guess a count or fail the game frame.
Other runtimes, unavailable count sources and too-small atlas limits retain
separate surfaces. No runtime's limit is inferred from SteamVR's limit.

The decision is latched while both UIs remain enabled. Changes in external
allocations or the handle just saved do not cause continuous reconstruction.
Closing either UI clears the decision; reopening evaluates the budget again.
Closing the menu while the counter remains on returns to its compact direct
surface, releasing the large atlas and its extra copy/scratch cost. A single
UI and ordinary/non-SteamVR paths do not allocate the atlas. Counter types remain
mutually exclusive and wait for an obsolete direct counter to retire before
allocating its replacement, preventing a transient third native UI handle.

Changing representation returns old handles before allocating the new one.
GPU receipts and in-flight readers defer optional cleanup to later application
frames; input is still polled/buffered and mouse gestures finish before their
ImGui owner is rebuilt. New surfaces retry promptly while warming, rather than
waiting for the next quarter-second counter refresh. No new steady-state wait
or native-count sampling loop is added.

The UI-only private proc resolver now uses a nonblocking retirement request:
busy source readers/native consumers leave the chain intact for a later poll.
The application's ordinary xrDestroySwapchain retains its blocking contract.
Shared-mode GPU copying and the V895 exhausted-generation real-frame fallback
are preserved; game synthesis/pacing, OSR/RIFE models, NR and ranking are unchanged.

Validation: eleven targeted tests pass. WARP D11/D12 checks cover direct FPS,
meter/menu, small-runtime fallback, actual projected13/14 allocation, subtracting
existing UI handles, busy inventory, both latched decisions, close/reopen,
compact single UI and all-off release. Conversion succeeds with a hard cap of
one UI native handle, proving no temporary extra slot is needed. Real D11/D12
private-runtime tests hold an old native End: UI retirement returns busy without
waiting, preserves its source, then succeeds on a later poll; ordinary application
destruction still waits for its consumer. Busy retirement polls are retried according to the nonblocking API contract.
No general game/pacing/RIFE matrix; owner headset validation remains pending.
