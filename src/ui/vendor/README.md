# Dear ImGui source provenance

The files in `imgui/` are copied from the Dear ImGui
sources used by the accepted OptiScaler V33034 source in
`OptiScaler/include/imgui/`. Only the core and Win32/D3D11 backend source files
required by the tray and private OpenXR menu are included. Integration changes remove the game-specific `pch.h` include from the
Win32 backend, use the executable module for viewport window classes, and disable the FreeType define in `imconfig.h`, selecting the bundled
STB font rasterizer and avoiding a new external binary dependency. No OptiScaler host,
generated build tree, binary, font, or game injection code is required.

Dear ImGui retains its MIT license in `imgui/LICENSE.txt` and the original
upstream copyright and authorship notices in every source file.

`../overlay_menu_renderer.cpp` extracts the immutable draw implementation from
accepted OptiScaler V33034 `menu/InUpscalerDx12Renderer.h` at commit
`6c36e598b9027a852ce2b759a1851378468a0938`, under its retained GPL-3.0 notice.
It replaces borrowed game-list state hooks with caller-private RT lists and
per-image submission receipts. `../overlay_menu_input.cpp` adapts the accepted
OptiInput cursor/raw/masking/DirectInput ownership rules to a foreground-owned
OpenXR menu and one framebuffer cursor. The adaptations have their own WARP,
Win32/DirectInput and fake-OpenXR fixtures; borrowing accepted source is not
treated as headset validation.

The Win32 backend also exposes a per-context mouse coordinate scale for the
standalone DPI-aware window. Positions are converted before ImGui deduplicates
input, and navigation warps use the inverse scale. Default 1.0 retains existing
backend behavior. The native DPI input test covers 150% and 200% scaling.
