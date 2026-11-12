# V985: RIFE installation recovery

This nightly includes all V984 native OpenXR multiframe improvements and the
RIFE installer fix verified by the owner after an installation access error.
The experimental Independent pacing route is not included.

The installer retries temporary access-denied and sharing-violation errors
when activating a validated runtime. If activation remains blocked, its log
records bounded Windows lock diagnostics, completed validator state and the
original error. Failed rollback retains recovery files and its journal.
The installer does not terminate other applications.

Validation: 32 CPU installer tests, including real Windows directory locks,
retry success, exhausted retries, rollback and bounded lock diagnostics.
The normal NVIDIA/RIFE runtime location and existing game caches are unchanged.
Vulkan payload and the disabled Radeon installer choice are preserved.
Use matching V985 tray and layer files; control ABI remains 98401.
