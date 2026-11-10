# V966: shared ownership proof for Luke AER framegen guides

V965 GTA logs show FSR rejecting motion resource ownership (pair failure20) and
DLSS-G refusing motion or depth device identity (detail11) before generating.
The owner reports the persistent mono state did not recur in that test.

Luke AER consumers now use the retained snapshot allocation receipt and the
existing cached backing-state challenge when Resource.GetDevice exposes a
different COM identity. This is shared by FSR admission, native DLSS-G, Hybrid
motion/depth binding, depth search and pair preflight. Canonical identities retain
their fast path. Generic consumers stay strict; adapter equality alone never
admits an alias, and missing or unproved allocation receipts refuse it.

N1 composition now records the allocation device of its newly created motion
texture. Depth retains its own original allocation receipt. Chronology, colour
tokens, eye assignment, camera crop, GPU fences and retained lifetimes are
unchanged. No extra texture copy or per-frame GPU ownership search is introduced.
Hybrid's existing bounded guide-admission verdict is kept in the F9 benchmark
sidecar so optical-flow fallback can be distinguished from actual guide use.

Regression fixtures use a forwarding device/queue alias over real D3D12 work,
original depth and N1-composed motion. They fail with the V965 consumers and pass
with V966 through FSR, Hybrid and actual private DLSS-G generation, including
held peer eyes and separate producer fences. Missing/forged/unproved aliases
and generic alias admission remain refused. Private DLSS-G still makes zero
shared NGX initialization calls. This is not game/headset visual acceptance.

RIFE inference/runtime/model/pacing and the Radeon payload are unchanged.
Shared-control ABI remains96201. Test GTA with AER framegen, same resolution,
FSR/Hybrid/DLSS-G switching, F9 and an F8 while moving; validate stereo and guide
alignment as well as actual generated frames. No Luke settings are rewritten.
