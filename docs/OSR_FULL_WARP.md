# Full OSR: OFXR's own reconstruction of the peer eye

OSR (OFXR Stereo Reconstruction) is OFXR's own work on the reconstructed eye
of Luke Ross titles, written from the public provider ABI and OFXR's inputs
only. The first piece (`docs/OSR_HOLE_FILL.md`) kept AFW as the renderer and
filled the band it smears. Full OSR renders the peer eye itself. It consumes
exactly what AFW consumes (the source eye colour, the converted depth, the
camera data of both eyes) and nothing else, so every title OFXR prepares for
AFW works with it unchanged, and the provider (PDAFWPlugin.dll) is not loaded.

## Reconstruction, Luke Ross tab

| `luke_osr` | Label | Renderer of the peer eye |
|---|---|---|
| 2 | OSR (first choice, default) | OFXR |
| 0 | AFW (legacy) | the provider (PDAFWPlugin) |

V764 removes the combined AFW + OSR band-fill option. Saved value 1 migrates
to OSR (2); an explicit AFW choice (0) stays AFW. Missing settings and Reset
to defaults select OSR. Persisted values remain independent of menu order.

The runtime ini carries the same key in `[experimental]`. The option applies
live; each reconstruction keeps its own history, so the first frame per eye
after a switch is a pure stereo warp.

## What Full OSR does

Four compute passes on the AFW command list, `src/d3d12/osr_full_warp.hlsl`,
driven by `src/d3d12/osr_full_warp_pass.hpp` and the CPU rules of
`include/xrfg/osr_full_warp.hpp`. They run where the provider's
`EvaluateFrameWarp` would, with the provider's own `CameraData` (reversed-depth
variant included), and their output takes the provider's place in the copy to
the generated eye.

1. **Stereo splat.** Every source depth texel is forward-projected into the
   destination eye through `source clip -> view -> world -> destination view
   -> clip`; each destination texel keeps the nearest depth that lands on it
   (2x2 footprint, `InterlockedMax` on the reversed depth). This is the
   coverage pass of the band fill.
2. **History splat.** When the layer holds the destination eye's own last
   real frame (its colour, converted depth, composed same-eye motion and
   camera, kept by Full OSR at the time that eye was the source), that depth
   is forward-projected into the current destination camera the same way,
   into a second target. Each history texel first moves to where its surface
   is now: its composed motion points to that eye's frame two publications
   back and includes the camera's share, which is what the texel's own depth
   projects to through the two cameras (the layer keeps the previous camera
   of each eye as well); what remains is the object's motion over two
   publications, extrapolated forward by one. A second dispatch records, for
   every destination texel, where the surface that won it lies in the
   history image, so the resolve fetches the colour there. This is what keeps
   swaying vegetation from doubling in the band beside itself (the capture
   pair of 2026-11-01 11:00 showed AFW and Full OSR with the same error
   against the next real frame, AFW without ghosts because its previous
   frame follows the motion vectors). Without motion data (the first two
   frames of an eye, or a gap) the texel stays where it was. A texel whose
   composed screen motion is small against the camera's share
   (`kOsrFullLockedRatio`, half, of a share above `kOsrFullLockedCameraTexels`,
   one texel; tighter limits let the gun drop out for a frame whenever a strafe
   slowed or reversed) is
   attached to the camera, as the first-person weapon and hands are: it
   keeps its place in this eye's screen, and where the other eye's warp
   shows the same surface the warp wins, being of this very instant; the
   previous frame only supplies the side of it the other eye cannot see.
   Reprojected as world geometry, a gun 30 cm away was thrown 160 to 230
   texels while strafing and came out half missing. Its composed motion is
   no measure of its own movement: composition adds camera and
   stereo-interval terms evaluated at its depth, which do not cancel for an
   object moving with the camera.
3. **Resolve**, per colour pixel. The pixel and its splatted stereo depth are
   projected back into the source image. If the source depth under the
   landing point agrees with the expected depth (relative tolerance 3% on
   reversed depth, i.e. on distance, over the 2x2 texels of the bilinear
   footprint), the source colour is sampled there with a Catmull-Rom filter
   clamped to the four nearest pixels. Bilinear sampling at the fractional
   positions of a warp erases single-pixel detail: the generated eye kept
   0.76 of the real frames' 1-pixel detail where AFW keeps 1.00, and in
   foliage the sharp/soft alternation between real and generated frames is
   seen as shimmer. Otherwise the
   same is tried against the history image through the history camera. A
   pixel neither image can account for is a hole. Forward depth with backward
   colour gives filtered colour without the cracks of a pure splat, and the
   agreement test keeps the 2x2 footprint from pasting a near object's colour
   onto the background beside it. When the pixel's own splatted depth
   disagrees, the farthest and then the nearest depth of the 3x3 texel
   neighbourhood are tried before the pixel counts as a hole: at a silhouette
   that finds the background behind the one-texel halo of the footprint, so
   objects keep their true width, and it closes most cracks in thin geometry
   such as vegetation (the first headset run of 2026-11-01 showed a 1-2 px
   hole line on every depth edge and 70% of hole runs 1-3 px wide).
   The previous frame of this eye, motion compensated, is the primary
   source: wherever it shows the same surface as the stereo depth (within
   the depth tolerance, or sky for sky), it is taken, and the warped other
   eye only fills the rest. Each eye's DLSS reconstructs sub-pixel detail
   with its own jitter sequence, so the other eye's fine pattern differs
   from this eye's, and showing it every other frame is shimmer in dense
   foliage: in the Cyberpunk F8 of 2026-11-01 11:12 (head still) this eye's
   previous frame predicted the next real frame's fine detail with
   correlation 0.95 and the warped other eye with 0.70, and AFW's output
   matched its previous frame exactly in fine detail. Rounds 7 to 11
   applied the rule only where the warped source is weakest, as a resolved
   pixel when the previous frame shows the same surface: sky-depth content
   (no parallax, and it carries this eye's own glow and haze) and the pixels
   whose 3x3 depth-texel neighbourhood spans a silhouette or thin geometry
   (`kOsrFullEdgeRatio`, 10%) or touches a band (a texel nothing from the
   source reached). The source colour there is the source eye's
   anti-aliased edge, a blend of the object with the background the source
   eye saw beside it; warped, it lands 1-3 px from the band on both sides as
   an outline of the object (2-3 times the normal error in the F8s of
   2026-11-01 12:05). The depth grid is half the eye resolution, so
   the warped source places edges and blades at that granularity and they
   shimmer against the real eye between frames, while the previous frame is
   a real render of them: in the Cyberpunk bushes of 2026-11-01 it erred 6.6
   against the next real frame where the warped source erred 9.7. Smooth
   surfaces keep the warped source, which is the fresher picture.
   A previous-frame pixel within one depth texel of a nearer surface in its
   own frame is that frame's anti-aliased silhouette, a blend of the object
   with what lies behind it, carrying the depth behind. When the object has
   moved relative to that background since (parallax while strafing), the
   blend would be left where the edge used to be: a thin dashed copy of the
   edge one frame's parallax beside the object (Cyberpunk lamp post and sign,
   F8s of 2026-11-01 13:15, strafing left). The halo is searched over
   `kOsrFullHaloRadius`, two depth texels (about 4 eye pixels): around a
   bright object the blend spreads past one texel through glow and the
   bright rim (F8s of 13:21). Whether the edge still stands where the halo
   is, is decided by parallax: the destination pixel's ray is projected into
   the history frame at the halo's depth and at the nearer surface's depth.
   Standing still or turning, both land on the same spot and the old edge is
   the current edge; while strafing they separate, and past
   `kOsrFullHaloParallax` (half a texel) across the edge the halo is not
   taken. Only the component toward the nearer surface (the mean offset to
   it in the halo search) counts: moving along an edge, such as the top of a
   roof while strafing, keeps the halo on it, and measuring the whole
   parallax drew a dashed line along every top edge (Spider-Man, 14:2x). (A test for
   a near surface within the radius kept a 3-5 px stale edge beside a
   bollard, F8 of 13:32:41.)
   When the previous frame shows a nearer surface than the warp, it is
   projected into the source eye: if the source holds something farther
   there, it would have seen the surface had it existed, so the surface is
   stale and the warp stands; if the source holds the same or a nearer
   surface, the surface is one only this eye sees and the previous frame is
   kept. The case is a first-person gun 30 cm from the face: its disparity
   is a few hundred pixels, so along the destination ray beside it the
   other eye sees sky while this eye sees the gun's side (Cyberpunk F8 of
   2026-11-01 13:40:39, the right half of the pistol replaced by sky in
   every generated frame).
   The warped source has the same halo: a warped pixel within the halo
   radius of a nearer surface in the source image is the source eye's
   anti-aliased silhouette carrying the depth behind, and along the
   destination ray the two depths project apart by the stereo parallax. It
   would land one disparity beside the object, a dashed line in the leading
   eye while strafing once the previous frame's clean pixel there is dropped
   by its own halo rule (lamp post, F8 of 2026-11-01 13:50:23). Such a pixel
   joins the band and takes the mirrored background.
   The object motion applied to the previous frame comes from the composed
   motion vectors, which can be wrong for some materials: the menu curtains
   of The Last of Us Part I barely move between this eye's frames, yet their
   vectors claim about one stereo disparity, flipping sign with the eye (-41
   then +32 px), and the moved curtain doubled against the other eye (F8 of
   2026-11-01 14:28:11). Where the motion-compensated and the camera-only
   previous frame differ by more than `ArbiterGap`, the other eye's warp,
   which shows this very instant, decides which one is taken.
   The previous frame counts as the same surface when any texel within one
   texel of it matches the warp's depth: fine fringes such as hair have
   noisy depth, each eye and each frame covering different strands, and
   requiring the exact texel let the other eye's strands through every other
   frame (Joel's hair against a bright wall, The Last of Us, F8 of 14:45:38:
   the generated frame 34% further from its real neighbours than they are
   from each other at the fringe, only 72% of fringe pixels from this eye's
   previous frame against 98% in the background).
   At a fringe (depth contrast or a band within one texel in the warp), the
   previous frame is taken whatever the warp's depth says, once the halo
   rule has kept it: depth cannot tell a stale surface from a strand the
   other eye happens to miss, and requiring agreement let the other eye's
   strands through in both directions (3% of Joel's fringe pixels flicked to
   the light background, 4.4% to a strand). A blend weighted to this eye
   still let the light background through where this eye has a strand.
   Whether the other eye "should have seen" a previous-frame surface is only
   a question when the camera has translated: the destination ray projected
   into the previous frame at the surface's depth and at infinity landing
   within `kOsrFullHaloParallax` means the previous frame looked along the
   same ray (standing still, or only turning the head), and with object
   motion compensated nothing can have made it stale. Judging it from the
   other eye's depth there rejected correct pixels wherever that depth is
   noise (hair, thin edges): on The Last of Us captures of 15:03:44 and
   15:04:13, head still, the rejected pixels erred 30 and 23 levels against
   the next real frame and one in five by more than 40, against 3 and 4 for
   this eye's previous frame, and the band fill put light specks on Joel's
   hair.
   The history colour is read at the nearest pixel, unfiltered: each depth
   texel moves the history by its own motion vector, and on hair the vectors
   differ strand by strand, so a filter samples a different phase in every
   texel. Catmull-Rom kept 0.68-0.89 of the real frames' 1-pixel detail on
   Joel's hair (F8s of 15:15-15:23) where AFW keeps 1.05, and the sharp/soft
   alternation between real and generated frames shimmered. The nearest
   pixel errs by half a pixel at most; the other eye's colour keeps the
   Catmull-Rom fetch.
   History texels nearer than `kOsrFullLockedDistanceMetres` (0.6 m, the
   reversed depth computed from the destination projection and the
   calibrated game units) are camera-attached whatever the motion: the
   camera's share only reveals the gun when the camera moves across it, and
   walking forward the gun aimed at the view centre has almost none, so it
   was reprojected as world geometry and went transparent and flickered. On
   the Cyberpunk F8 of 18:53:57, 18.2% of depth texels lie within 0.5 m and
   still 18.2% within 0.75 m.
   A history surface is legitimate band content only if the source eye
   could not see it: its reprojected position in the source must hold a
   nearer surface (the occluder). A history surface the source sees at the
   same or a farther depth has moved since the previous frame, a swaying
   twig for instance, or the warp missed it; it is tagged moved and filled
   like a hole. (An earlier rule compared the history depth with the band's
   background edge; against the sky that rejected every frond behind a neon
   sign and striped the band with mirror fill.)
4. **Inpaint.** Every pixel that is not a stereo pixel belongs to a band: a
   run of history, moved and hole pixels along the row between two stereo
   pixels. The nearer bounding pixel is the occluder; the band is background
   revealed beside it. History pixels keep the previous frame; moved and hole
   pixels take the background pixel mirrored across the run's background
   edge (a 3x3 average only when that pixel is not resolved: a blurred strip
   in a sharp image reads as a band). While strafing, the leading eye's band
   is background no input has seen, since the other eye and this eye's
   previous frame are both behind it, and is filled this way every frame
   that eye is generated. When the two ends of the
   row are at similar depths, the row cannot tell which side is background:
   where a post meets a rail both ends are the object, and the mirror pasted
   rail or post colour into the gap (The Last of Us stairs, F8 of 14:31:28).
   Such holes, and any the row cannot fill, take the farthest surface found
   in eight directions within `FillReach` (48 px): every direction that
   reaches the farthest surface contributes its first background pixel,
   weighted by inverse distance (mirroring the single nearest hit drew a
   slanted streak when it lay diagonally). So do pixels whose mirror lands
   on a surface nearer than the background edge: where a lone background
   pixel ends the row inside an object (wall seen through a gap in a rail),
   the mirror landed back on the object and pasted its colour into the band,
   drawing light strokes beside the stair posts (F8 of 15:03:26) and light
   specks on Joel's hair (15:15:33). Against the next real frame those
   pixels erred 6.9, 15.0 and 60.6 levels mirrored, 4.8, 4.0 and 16.0 with
   the background fill. The band is not blended into its background edge:
   that pixel is the source eye's anti-aliased silhouette, and a 6 px blend
   drew an outline of every object. A run
   open on one side (the image border, or longer than `kOsrFullMaxRun`)
   keeps its history or stretches the other side.

The history is the destination eye's last accepted frame
(`osr_full_history_usable`): recorded by Full OSR, the frame the layer last
accepted for that eye (the publication matches the layer's own record for the
eye), at most four publications old. Stream identity is not compared across
eyes: Cyberpunk 2077 runs one DLSS stream per eye, and the first run never
used history for that reason. The provider mode
after the priming rules still decides whether history is consulted at all:
the alternate-eye prime (effective mode 1) is a pure stereo warp, as it is
for AFW. A producer reset, origin change or geometry change drops the history
through the same resets that drop AFW's.

### Finite far planes

Sky depth is 0 and is carried as `FarDepth` (1e-6) through the splats. With
an infinite projection (Cyberpunk) a reprojected sky depth stays at that
value; with a finite far plane (Spider-Man Remastered) reprojection spreads
it by several percent (0.92e-6 to 1.08e-6), past the relative depth
tolerance. Two depths at sky level (`SkyDepth`) are therefore always the same
surface, and sky is never farther than anything: without this half of the
sky disagreed with itself, the warp failed there, the previous frame's sky
was taken for stale, and whole areas came out black (F8 of 2026-11-01
14:12:57, 9.5% of the eye). A reprojected depth that comes out slightly
negative (at or beyond a finite far plane) is clamped to far, never rejected.

### De-jittered depth

The DLSS depth is rendered with a sub-texel jitter that the TAA-resolved
colour no longer has, and the depth window's origin is rounded to a texel.
Measured on the Cyberpunk F8s of 2026-11-01 over sixteen frames: colour edge
= depth edge - 1.97 px per texel of jitter + 1.31 px along x, and - 1.69 px
per texel - 0.05 px along y. Full OSR therefore treats depth texel (i, j) as
the scene at (i, j) + 0.5 + (rounded - exact window origin) +
`kOsrFullDepthAlign` - jitter in every splat and every depth lookup, with
the frame's own jitter for the source and the stored jitter for the history
(`LukeGuideWindow::fraction_x/y`, `OsrFullConstants::source_dejitter`). The
alignment constant (0.36 texel in x) is the part the window rounding does
not explain, most likely Luke's integer crop of the eye out of the DLSS
output; it is measured, not derived, and game or resolution dependent until
that crop is read from the producer. A constant misalignment of the depth
against the colour puts a strip of background at the object's disparity on
one side of every silhouette, and a strip of object at the background's
disparity on the other; the strips swap sides with the eye: a ghost on
one side of every object, a different side per eye.
Without it every silhouette and blade moved by up to +-1 px from one
generated frame to the next against a stable real eye: unstable edges and
flickering vegetation.

## What stays the same

- The camera data, depth conversion, calibration, motion composition and
  every admission rule run unchanged; Full OSR starts after them.
- The provider (PDAFWPlugin.dll) is not loaded. With Full OSR selected the
  synthesizer runs native: the layer allocates what the provider's factory
  did (the converted depth, R32_FLOAT with UAV access; one colour buffer per
  eye, which receives the source copy and which F8 saves as the prior eye),
  skips the provider's descriptor registration (every layer pass binds its
  own descriptors) and records its own copies. Selecting AFW or AFW + OSR
  band fill live loads the provider into a fresh context (1048 b = 0);
  selecting Full OSR again unloads it (1048 b = 1). A history reset at a new
  scene only forgets the eye history. The provider file may be missing:
  only the AFW reconstructions need it, and the standalone launcher stages
  it only when present.
- The provider's shading rate does not apply: Full OSR renders at the eye
  resolution (quality divisor 1). Depth dilation, AFW's silhouette remedy,
  is not applied to the depth Full OSR reads: its edge rules assume the
  depth matches the colour. Framewarp mode offers Off, Other eye only
  (mode 1, no previous frame, for debugging) and On (Combined; Previous
  frame behaves the same for Full OSR).

## Diagnostics

- With Debug framewarp on, pixels taken from the previous frame are tinted
  green, history pixels rejected as moved orange and inpainted pixels
  magenta; stereo pixels are untouched.
- Flight log `luke_afw_stage` 1310 per evaluation: result holds the
  effective mode (bits 8+), whether history was used (bit 0) and whether its
  motion was used (bit 1), b the publication, c the history frame's
  publication. 1311 reports a failed
  preparation (HRESULT, once a second); that frame falls back to the provider.
- F8 captures add `osr_stereo_depth` (R32 at depth resolution, zero = nothing
  landed) and `osr_resolved` (R32 at eye resolution: depth bits with the low
  two bits as the tag, 0 hole, 1 stereo, 2 history, 3 moved), next to the usual
  `afw_generated_eye`.

## Tests

`xrfg_osr_full_warp` covers the history admission and the shader flags.
`xrfg_interface_metrics` checks the "OSR" labels: Full OSR replaces "AFW" in
the mode label ("FSR + OSR + Neural") and the status line ("OSR: Active").

## Limits

- Object motion in the previous frame is extrapolated at constant velocity
  over one publication from the composed same-eye motion; accelerating or
  oscillating objects land slightly off, and the moved-surface test then
  hands them to the mirror fill.
- Screen-space glow and haze are warped at the depth of what lies behind
  them, so a bright object's glow lands beside it, displaced by the object's
  disparity. Against the sky this is handled: sky-depth content has no
  parallax, so the resolve takes this eye's previous frame wherever both the
  warped source and the previous frame hold sky-depth content, which carries
  the glow where this eye sees it (the Cyberpunk neon sign of 2026-11-01 kept
  14 px of the source eye's glow beside it where the real eye has 2); these
  count as resolved pixels, not as bands. Glow
  over a textured background with real depth is still displaced by the
  difference of the two disparities; AFW shows it too.
- Mirroring repeats background features; it removes streaks, it does not
  recover detail. The exact content exists one frame later and would need a
  frame of latency.
- Without the provider, a frame Full OSR cannot prepare (1311) keeps the
  producer's peer eye instead of falling back to AFW.
