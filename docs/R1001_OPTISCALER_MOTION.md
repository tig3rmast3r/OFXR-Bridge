# R1001: original OptiScaler motion endpoints

Local Radeon experiment, from R1000. Stock OptiScaler and the standalone FidelityFX SDK remain unmodified. The matching OptiScaler INI must keep `EnableDlssInputs=true`, `EnableFfxInputs=false`, with OptiScaler frame generation disabled. Disabling FFX *inputs* does not disable FSR upscaling of DLSS input.

Ghost of Tsushima R1000 F8s captured with Hybrid/FidelityFX and FSR FG show original adjacent-eye N-1 input motion. Depth/camera reprojection fits its static scene at median 0.10-0.52 input pixels, whereas the native NVIDIA same-eye N-2 assumption misses by 11-39 pixels. The latter is a producer-specific contract, not a property of the game name when its SR implementation changes.

Verified stock OptiScaler now explicitly publishes confirmed original N-1 motion. Existing camera/depth-aware N-1 composition supplies same-eye history for downstream reconstruction. Native NVIDIA measured N-2 title rules, guide completion/device/list checks and frame delivery remain unchanged. This is not a constant motion offset or an optical fallback.

Regression checks exercise the shipping replacement dispatch for Ghost and GTA, retain native Ghost/Horizon N-2 behavior and check N-1 guide composition. Game/headset quality, OSR combinations and Radeon hardware still require owner verification; an offline fixture pass does not establish live acceptance. FSR 4.x driver-only/custom providers bypassing the observed SDK dispatch remain outside this prototype.

R1001 installs to its separate version directory; restart the game and previous tray before changing the loaded layer. No automatic profile edit, install or push is performed.
