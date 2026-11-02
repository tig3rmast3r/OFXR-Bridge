# V820/V821: qualified SteamVR Director timing

The retained native timing policy applies only to Director on SteamVR.
Framework keeps its existing consumer, normal/Buffer clock and Wait worker.
VDXR/Legacy, native Pimax and AFW-only routing retain their existing behavior.

## Qualification near headset refresh

Director may overlap one future native Wait after the accepted output cadence
qualifies. Qualification uses the last 16 output intervals, rather than the
compositor's refresh counter, which can include repeated images. For one
continuous second, the inverse median interval must exceed headset refresh
minus 1 FPS, and the mean rate must reach at least 99% of refresh. At 90 Hz,
these thresholds are a median rate above 89 FPS and a mean of at least 89.1 FPS.

Normal delivery continues while qualifying. The entry timer adds no one-second
FIFO delay or fixed early-End timer. Once active, a mean rate below 99% of
refresh or an idle gap longer than two physical periods disables lookahead at
the next policy decision. There is no one-second exit delay. An already started
Wait is consumed and balanced rather than cancelled. Disabling generation
clears qualification; re-enabling must qualify again.

The physical refresh period must be known before qualification can begin.
The consumer uses the cached clock without adding an OpenVR call or observer
lock to each frame.

## Spacing below refresh

When lookahead is not qualified, Director leaves at least one physical period
between native handoffs: approximately 11.111 ms at 90 Hz. It waits only the
remaining interval, keeping two ready images from being handed to SteamVR a
millisecond apart. It does not round late preparation onto another refresh-grid
slot or charge every image an unconditional extra period. A selected adaptive
Buffer reserve can add its learned refill delay independently.

Qualified full-refresh delivery relies on the native frame loop without this
additional minimum handoff interval. End uses the current native prediction's
display time. GPU completion fences, FIFO image order and source leases retain
their ownership contracts in both states.

## Native cycle renewal

A native cycle obtained before lengthy image preparation may be stale even
while its predicted display time remains in the future. Director renews an
unsubmitted cycle when preparation completed at least one physical period after
that Wait returned, as well as when the display prediction is too late for the
handoff. Age is measured at preparation completion, excluding a later artificial
handoff delay.

Renewal retains the same FIFO image and uses the Wait/Begin frame-discard
transition while serializing Begin/End. This is a SteamVR delivery workaround;
the one-period preparation-age rule is not an OpenXR specification requirement.
The frame-loop contract remains defined by
[xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html).

## Verification limits

Focused coverage checks qualified entry and exit, mode changes, native-cycle
balance, ordered D3D11/D3D12 pixels, handoff spacing, stale-cycle renewal,
RIFE multipliers and live switching. These checks do not guarantee a constant
headset frame rate under every game, GPU load or runtime configuration. See
[the Director policy notes](STEAMVR_DIRECTOR_V821.md) for the corresponding
controlled SteamVR comparisons.
