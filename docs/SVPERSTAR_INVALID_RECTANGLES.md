# Invalid UEVR projection rectangles

Reject an entire invalid projection layer on the private UEVR path while retaining other valid layers. Do not invent a crop or forward INT_MIN/out-of-range rectangles that poison native delivery. Retire temporal continuity and allow valid frames to resume. Unit and live private GPU cases preserve exact source pixels and native call ownership. This change is independent of the depth-history allocation reduction.
