# V770: Luke colour origin inside black bars

This nightly update changes only the Luke Ross colour-origin layout (how the
pairing mark is placed in the DLSS output and found in the eye image).
Reconstruction, frame generation, NR, delivery/pacing and the UI are
unchanged.

## Symptom

With Luke's Optimize VFOV on, The Last of Us Part I under VDXR (Quest 3)
never engaged AFW or OSR: the overlay counter stayed red, every colour-origin
read found nothing, and every generation prepare reported a missing capture.

## Cause

Luke drew black bars over the eye image (159 rows at the top, about 350 at the
bottom and 80 columns at the left of a 3456x3773 eye). They covered the
pairing mark at rows 34-42, so no colour was ever paired with its guide.
Unchecking Optimize VFOV removed the bars and the problem.

## Change

The reader also reads the middle column and middle row of the first eye image
and counts exactly black samples from the top, bottom and left edges. A line
black throughout (a loading screen) measures nothing. A measurement is adopted
once 8 consecutive ones agree within 2 pixels, so a dark scene beside a bar
does not move the mark; flight log luke_afw_stage 1250 records each adopted
size (b = left<<32 | top<<16 | bottom, c = eye extent).

Writer and reader place the mark at the title's inset or the bar plus 8
pixels, whichever is larger, using the bottom bar for a bottom band, and never
more than a tenth of the eye in from its edge. Without bars the title's inset
is unchanged.

## Test

The Last of Us Part I under VDXR with Optimize VFOV on: the overlay turns
green within a second of gameplay and `ofxr-luke-origin-pid*.log` counts OK
reads. Titles without bars (Cyberpunk 2077, Spider-Man Remastered, Kingdom
Come Deliverance II) engage as before. `xrfg_luke_color_origin` covers the
inset, the limit, the bottom band and the bar measurement.
