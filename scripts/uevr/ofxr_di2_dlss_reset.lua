-- DI2: stock UEVR equivalent of the fork's F8 FSR2/DLSS rebuild.
-- Copy to %APPDATA%/UnrealVRMod/DeadIsland-Win64-Shipping/scripts/.
-- No startup commands: only an F8 press or the script-panel button requests a reset.
local ticks_left = 0
local key_held = false
local button_requested = false
local f8 = imgui.ImGuiKey.Key_F8

local function fork_owns_f8()
    local ok, value = pcall(function()
        return uevr.params.vr:get_mod_value("VR_RunConsoleScriptKey")
    end)
    return ok and tonumber(value) == 119 -- Windows VK_F8 used by the fork
end

uevr.sdk.callbacks.on_draw_ui(function()
    if imgui.button("Restart DI2 FSR2 / DLSS (F8)") then
        button_requested = true
    end
end)

uevr.sdk.callbacks.on_pre_engine_tick(function()
    local down = imgui.is_key_down(f8)
    local pressed = down and not key_held
    key_held = down
    local requested = button_requested or (pressed and not fork_owns_f8())
    button_requested = false
    if requested then
        uevr.api:execute_command("r.FidelityFX.FSR2.Enabled 0")
        ticks_left = 10
        print("[OFXR DLSS reset] FSR2 disabled; rebuilding after 10 engine ticks")
    elseif ticks_left > 0 then
        ticks_left = ticks_left - 1
        if ticks_left == 0 then
            uevr.api:execute_command("r.FidelityFX.FSR2.Enabled 1")
            uevr.api:execute_command("r.FidelityFX.FSR2.QualityMode 1")
            print("[OFXR DLSS reset] FSR2 re-enabled at Quality; DLSS can reattach")
        end
    end
end)
