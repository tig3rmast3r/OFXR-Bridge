# V918: Recover displaced Luke origin markers

The shared Luke color-origin reader searches the full eye image after two
seconds of continuous CRC loss. One recovery snapshot per queue is decoded on
an independent background worker; attempts are at least two seconds apart.
Black/menu frames keep recovery pending. A successful search supplies only a
position hint: the current eye must yield a new complete sync/payload/CRC before
its token can select a guide. No token from an older snapshot is reused.

A live recovered marker locks a compact read region and stops wider searches.
That region survives transient loss, so the returning marker cancels recovery
without another full-eye GPU copy. Each new loss has its own two-second grace
period; repeated misses do not postpone the deadline. Geometry changes reset
learned positions. The existing negotiated crop scan remains the normal path.

The recovery reader has no game-name gate. Existing title-specific marker
placement and RGB decoding policy remain unchanged. Completely erased or
clipped markers cannot be recovered by position search.

Focused coverage: unchanged-size marker relocation, black-menu/new-token
recovery, stale-identity refusal, cooldown/retry deadlines, transient recovery,
geometry reset and ambiguous CRC refusal. The r7 implementation passed five
focused CTests. Only the combined V919 tip is built for headset acceptance;
there is no separate V918 test binary.
