-- scripts/lib/log.lua
-- Shared warning ring buffer for RpgAi libs.
--
-- Console print() is invisible in web mode — designers never see it. Every
-- lib routes its warnings here; adventure.lua shows the recent entries in
-- /debug (and /validate), so problems surface where the designer is looking.
--
--   local log = require("lib/log")
--   log.warn("world", "cannot write world file ...")
--   log.recent(10)   → array of formatted strings
--   log.count()      → total entries since startup (ring keeps the last 50)

local M = {}

local _entries = {}
local _total   = 0
local MAX      = 50

--- Record a warning. Also echoes to console (visible in console mode).
-- @param src  Short source label ("world", "persona", "adventure", ...).
-- @param msg  Message.
function M.warn(src, msg)
    _total = _total + 1
    table.insert(_entries, {
        t   = os.date("%H:%M:%S"),
        src = tostring(src or "?"),
        msg = tostring(msg or ""),
    })
    while #_entries > MAX do table.remove(_entries, 1) end
    print("[" .. tostring(src) .. "] WARNING: " .. tostring(msg))
end

--- Last n warnings as formatted strings (oldest first).
function M.recent(n)
    n = n or 10
    local out = {}
    for i = math.max(1, #_entries - n + 1), #_entries do
        local e = _entries[i]
        table.insert(out, string.format("[%s][%s] %s", e.t, e.src, e.msg))
    end
    return out
end

function M.count() return _total end
function M.clear() _entries = {}; _total = 0 end

return M
