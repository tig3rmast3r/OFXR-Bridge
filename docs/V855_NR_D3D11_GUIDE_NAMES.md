# V855: NR is never handed a D3D11 texture as a D3D12 resource

This nightly update changes the D3D11 guide transport's NGX parameters only.

## Symptom

Ready or Not crashed entering VR with NR before SR on: access violation
reading 0x10 in `DlssNr::EvaluateBeforeUpscale`, from the D3D11 evaluate hook.

## Cause

Entering VR recreates the D3D11 guide transport. Its D3D12 Output exists only
after the copy that follows the DLSS evaluate, but NR's before-SR pass runs
first. NR's typed read found nothing; its untyped read fell back to the game's
parameter block and returned the game's D3D11 output texture, and NR called
`GetDesc` on it as an `ID3D12Resource`.

## Change

The transport's parameters refuse Color, Output, MotionVectors and Depth when
the transport holds none, instead of falling back to the game's D3D11
resources. This also stops withheld depth leaking through. NR already sizes
from the colour texture when there is no Output.

## Test

Ready or Not (D3D11), NR before SR: enters VR without crashing.
