# V902: remove the V890 OSR workaround from current V901

Restores OSR visible-surface/history behavior from V889 source86cd7261,
retaining V891 rejected-colour/background guards. All non-OSR V901 changes
remain, including V900 CPU meter/ABI90001 and V901 projection continuity.
There is no new threshold, pose correction, vector formula, diagnostic capture,
allocation or UI setting. Existing F8 stays unchanged. The character flicker
with frame generation is deliberately not claimed fixed; the owner requested
this previous-base checkpoint to measure its local XY displacement.

Retained tests restore the V889 history contracts; the separate V891 background
strip/colour-halo GPU checks remain. No general game/pacing matrix is required.
