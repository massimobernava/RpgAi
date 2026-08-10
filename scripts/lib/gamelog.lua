-- gamelog.lua — Persistent disk log for any RpgAi adventure.
-- Generic: works with any adventure using adventure.lua framework.
-- Activate via adv.set_config({ log_file = "saves/mygame.log" }).
-- CoderAI can read_file the log and analyze NPC behavior.
--
-- Format: T{turn}[{time}] [{TAG}] {message}
-- Tags: PLAYER, TOOL, NARR, TICK, TICK_LLM, TICK_EVT, MOV, SIM, TICK_ERR

local M = {}

local _path     = nil   -- absolute/relative path to log file
local _state_fn = nil   -- () -> state table (for turn/time context)

local function _ctx()
    local s = _state_fn and _state_fn() or nil
    local ts = (s and s.time)  and ("[" .. s.time .. "]") or ""
    local tn = (s and s.turn ~= nil) and ("T" .. tostring(s.turn)) or "T?"
    return tn, ts
end

-- Append one line. Opens/closes file each call (safe for concurrent access).
local function _append(line)
    local f = io.open(_path, "a")
    if not f then return end
    f:write(line .. "\n")
    f:close()
end

-- Initialize log. Writes session separator.
-- state_fn: function() -> state table (may be nil).
-- Returns true, or false + error message.
function M.init(path, state_fn)
    _path     = path
    _state_fn = state_fn
    local f = io.open(path, "a")
    if not f then
        _path = nil
        return false, "cannot open log: " .. tostring(path)
    end
    f:write(string.format("\n=== SESSION %s ===\n", os.date("%Y-%m-%d %H:%M")))
    f:close()
    -- Expose path as Lua global so C++ can read it for @log expansion
    _GAMELOG_PATH = path
    return true
end

function M.active() return _path ~= nil end
function M.path()   return _path end

-- Write one log entry.
-- tag: "PLAYER"|"TOOL"|"NARR"|"TICK"|"TICK_LLM"|"TICK_EVT"|"MOV"|"SIM"|"TICK_ERR"
-- msg: string (will be tostring'd)
function M.write(tag, msg)
    if not _path then return end
    local tn, ts = _ctx()
    _append(string.format("%s%s [%s] %s", tn, ts, tag, tostring(msg or "")))
end

-- Write a visual section separator (no turn/time prefix).
function M.separator(label)
    if not _path then return end
    _append(string.format("--- %s ---", label or ""))
end

return M
