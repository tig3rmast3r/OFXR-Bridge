# V820 SteamVR pacing trial

Based on V819, retaining its OpenKneeboard panels, RIFE2/3/4X panel queue/banks,
and the accepted NR changes inherited from V813. Local game/headset trial only.

SteamVR Framework and Director now share the native frame consumer tested in
V814/V818: no CPU slot rounding that discards a usable refresh. Real native
Wait/Begin/End own timing; expired unsubmitted native cycles are renewed while
retaining the same FIFO image. GPU fences, source leases and native-image
ownership are unchanged. Mode/capacity updates change future admission without
draining already accepted images. Buffer retains the existing2/3-slot choice.

One future Wait is permitted only after qualification: FPS from the median of
the last16 output intervals must stay >physical-refresh-1 for1s, together with
the99% mean-production guard. At90Hz this means median>89 and mean>=89.1fps.
The timer resets if qualification breaks. Normal delivery continues during
qualification; no1s latency, extra FIFO depth or fixed early-End timer is added.

Exit remains fast: mean below99%refresh, or idle>2periods at the next decision.
At90Hz these are89.1fps and22.22ms. There is no1s exit delay/minimum ON time.
An already-started native Wait is consumed and balanced, never cancelled.

OFXR OFF always disables lookahead even at full source cadence. ON clears the
old samples and qualifies again. The test reproduces the missing OFF guard
before the fix and passes afterwards. Framework is distinct from OFXR OFF:
lookahead can qualify in either enabled Framework or enabled Director mode.

Enabled automatically only for true SteamVR identity in install_private_runtime.
VDXR/Legacy, native Pimax and AFW-only bypass preserve their existing routing.
The cached physical clock must be known before qualification can begin; no
OpenVR call or observer lock is added to the frame consumer.

Rejected forced lookahead, calibrated222us timer and physical1222us scheduling
experiments are not imported. No NR, engine/shader, panel, worker/model/provider
code is changed from V819. Tray/DLL implementation identity is820, control ABI
82001; runtime/provider protocols stay unchanged.

53 selected tests pass, including actual D11/D12 pixels, qualified transitions,
native-cycle balance, OFF/ON, modes/capacity, all V819 panel regression cases,
RIFE2/3/4 and live multiplier switching, VDXR and AFW-only scope. Not a full-suite
claim. Original failures are retained: instantaneous mock Wait cannot validate
a runtime-owned pacing clock; the mock now throttles and checks distinct ordered
display targets plus sustained cadence. The strict old CPU-entry floor remains
checked for the legacy path. OFF deliberately retains an unthrottled fault model
to detect any client-added floor. This models the Khronos xrWaitFrame contract:
https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html

Game/headset smoothness still needs owner acceptance. The V818 near89fps and
transition presentation losses, GPU queue delays and virtual rendering-clock
coherence limitations are not claimed fixed. A recovery longer than1s can still
qualify. Compare the same game/runtime/settings against sealed V819.

To test: close the old game/tray and start the V820 tray from its extracted
folder. Test SteamVR Framework and Director with Buffer as desired. No live
installation, profile/registry/driver changes or public push are performed here.
