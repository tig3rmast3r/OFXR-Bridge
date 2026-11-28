local script_path = assert(arg[1], "reset script path required")
local function check(value, message) assert(value, message) end
local function fixture(binding)
    local f = {tick = 0, down = false, button = false, calls = {}, binding = binding}
    local callbacks = {}
    _G.imgui = {
        ImGuiKey = {Key_F8 = 579},
        is_key_down = function(key) check(key == 579, "F8 enum mismatch"); return f.down end,
        button = function() local value = f.button; f.button = false; return value end
    }
    _G.uevr = {
        params = {vr = {get_mod_value = function(_, key)
            check(key == "VR_RunConsoleScriptKey", "wrong fork setting")
            return f.binding or ""
        end}},
        api = {execute_command = function(_, command)
            check(f.inside_tick, "commands must run on the engine thread")
            f.calls[#f.calls + 1] = {tick = f.tick, command = command}
        end},
        sdk = {callbacks = {
            on_draw_ui = function(fn) callbacks.ui = fn end,
            on_pre_engine_tick = function(fn) callbacks.tick = fn end
        }}
    }
    dofile(script_path)
    function f:advance(down, count)
        self.down = down
        for _ = 1, count or 1 do
            self.tick = self.tick + 1; self.inside_tick = true
            callbacks.tick(); self.inside_tick = false
        end
    end
    function f:click() self.button = true; callbacks.ui() end
    check(#f.calls == 0, "loading a script must not change CVars")
    return f
end
local function completed(f, off_tick, on_tick)
    check(#f.calls == 3, "expected exactly one disable/re-enable/quality sequence")
    check(f.calls[1].tick == off_tick and f.calls[1].command == "r.FidelityFX.FSR2.Enabled 0", "disable mismatch")
    check(f.calls[2].tick == on_tick and f.calls[2].command == "r.FidelityFX.FSR2.Enabled 1", "ten-tick re-enable mismatch")
    check(f.calls[3].tick == on_tick and f.calls[3].command == "r.FidelityFX.FSR2.QualityMode 1", "quality mismatch")
end
local f = fixture(); f:advance(true, 30); completed(f, 1, 11)
f = fixture(); f:advance(true); f:advance(false, 2); f:advance(true); f:advance(false, 10)
check(#f.calls == 4 and f.calls[1].tick == 1 and f.calls[2].tick == 4 and f.calls[3].tick == 14,
    "a fresh press must restart the ten-tick cycle")
f = fixture("119"); f:advance(true, 30); check(#f.calls == 0, "fork native F8 must not be duplicated")
f:click(); check(#f.calls == 0, "UI must only request a game-thread reset")
f:advance(false, 11); completed(f, 31, 41)
f = fixture("-1"); f:advance(true); f:advance(false, 10); completed(f, 1, 11)
f = fixture(); f:advance(false, 40); check(#f.calls == 0, "idle must not change CVars")
print("DLSS_RESET stock F8, held key, restart, native-fork binding, button/game-thread, startup/idle PASS")
