# V769: Luke colour origin for an eye slightly larger than the output

This nightly update changes only the Luke Ross colour-origin layout (how the
pairing mark is placed in the DLSS output and found in the eye image).
Reconstruction, frame generation, NR, delivery/pacing and the UI are
unchanged.

## Symptom

At one resolution, The Last of Us Part I under VDXR delivered a 1941x2119 eye
from a 2112-high DLSS output. The layout rejected any eye even one row taller
than the output, so that eye could never be paired with its guide.

## Change

An eye image up to 5% larger than the output, in either dimension, is treated
as the output slightly magnified and laid out as a full-size crop. The band
then moves by a few pixels at most, inside the reader's translation window. A
larger mismatch is still rejected.

## Test

`xrfg_luke_color_origin`: the 3920x2112 output with a 1941x2119 eye has a
layout; an eye more than 5% taller does not. Titles whose eye is not larger
than the output take the same path as before.
