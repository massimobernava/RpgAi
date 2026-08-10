-- =============================================================================
--  scripts/lib/adventure.lua  —  RpgAi Adventure Framework
--
--  Eliminates boilerplate from adventure scripts. An adventure only needs:
--    - Static data (LOCATIONS, NPC_DATA, NPC_AGENTS)
--    - Adventure-specific state fields
--    - get_welcome_message, set_initial_state, get_json_schema
--    - process_ai_response (only adventure-specific updates)
--    - get_system_prompt (header + rules + adventure blocks; common blocks via M.prompt_*)
--    - Adventure-specific tools + commands
--
--  USAGE IN ADVENTURE SCRIPT:
--    local adv = require("lib/adventure")
--
--    local CFG = {
--        use_agents    = true,   -- think_as_npc tool
--        use_time      = true,   -- advance_time tool + state.time/day/giorno_index
--        use_inventory = true,   -- cambia_inventario tool + state.inventario/soldi
--        use_notes     = true,   -- remember tool + state.notes with scope
--        use_memory    = false,  -- memory_write/read (pass memory lib to get_tools)
--        days = {"lunedì","martedì","mercoledì","giovedì","venerdì","sabato","domenica"},
--        debug_log = false,
--    }
--
--    In set_initial_state(): call adv.set_state(state) after building state.
--    In restore_state():     call adv.set_state(state) after restoring.
--    In before_ai_turn():    call adv.before_turn() — resets _tool_calls + agents.
--    In get_tools():         call adv.get_tools(NPC_DATA, LOCATIONS, TRAVEL_MAP, memory, extra_tools)
--    In get_state_snapshot():call adv.snapshot()
--    In restore_state():     local data, res = adv.restore(snapshot, init_agents_fn); state=data
-- =============================================================================

local json      = require("json")
local agent_lib = require("lib/agent")
local wlog      = require("lib/log")

local M = {}

-- ============================================================
-- MODULE-LEVEL STATE (shared across all turns in the session)
-- ============================================================

local _state        = nil   -- reference to adventure's state table
local _agents       = {}    -- { id -> agent object }
local _turn_counter = nil   -- shared agent turn counter
local _tool_calls   = {}    -- reset every turn by before_turn()
local _debug_log    = false
local _dbg_tools    = {}
local _cfg          = {}    -- active CFG
local _debug_fn     = nil   -- optional fn() -> string, set by adventure for extra /debug lines
-- npc_data and locations stored at module level so tool closures
-- always see the latest values even if get_tools() was called before
-- set_initial_state() or restore_state() (which is the normal engine flow).
local _npc_data     = nil
local _locations    = nil
local _travel_map   = nil
local _turn_seen    = nil   -- last state.turn observed (backup _tool_calls reset)
local _gamelog      = nil   -- gamelog module instance (nil = disabled)
local _tick_fn_raw  = nil   -- original tick_fn before gamelog wraps it
local _tick_fn_logged = false -- true once gamelog log-wrap applied
local _events_registry = {}   -- { id -> event_def }
local _events_order    = {}   -- ordered array of ids (fire order)

-- Backup reset for per-turn caps: if the adventure forgot to call
-- adv.before_turn(), stale _tool_calls would block move_player/advance_time
-- forever. Detect a turn change via state.turn and clear the caps.
-- before_turn() remains the primary reset (also resets agents).
local function _turn_guard()
    if _state and _state.turn ~= nil and _state.turn ~= _turn_seen then
        if _turn_seen ~= nil then _tool_calls = {} end
        _turn_seen = _state.turn
    end
end

-- ── Unified location graph ───────────────────────────────────────────
-- Static TRAVEL_MAP (designer) + world.lua connected_to (LLM-generated)
-- merged into one navigation graph. Without this merge, generated rooms
-- would be unreachable by move_player and invisible to prompt_exits.

local _world_lib = nil
local function _world()
    if _world_lib == nil then
        local ok, w = pcall(require, "lib/world")
        _world_lib = (ok and type(w) == "table") and w or false
    end
    return _world_lib or nil
end

-- Visual-Novel asset lib (lazy; loaded only when an adventure opts in via
-- CFG.vn). Selects flat backgrounds + foreground NPC sprites by tag.
local _vn_lib = nil
local function _vn()
    if _vn_lib == nil then
        local ok, v = pcall(require, "lib/visualnovel")
        _vn_lib = (ok and type(v) == "table") and v or false
    end
    return _vn_lib or nil
end

-- Persona lib (lazy). Used by VN mode to read NPC-specific verbs/topics that
-- the main LLM curates in the persona file — no parallel store.
local _persona_lib = nil
local function _persona()
    if _persona_lib == nil then
        local ok, p = pcall(require, "lib/persona")
        _persona_lib = (ok and type(p) == "table") and p or false
    end
    return _persona_lib or nil
end

-- Merged neighbor list for one location.
local function _neighbors(id)
    local out, seen = {}, {}
    -- Support both array format {"loc1","loc2"} and hash format {loc1=true, loc2=true}
    for k, v in pairs((_travel_map and _travel_map[id]) or {}) do
        local n = (type(k) == "number" and type(v) == "string") and v
               or (type(k) == "string") and k
               or nil
        if n and not seen[n] then seen[n] = true; table.insert(out, n) end
    end
    local w = _world()
    if w and w.neighbors then
        for _, n in ipairs(w.neighbors(id)) do
            if not seen[n] then seen[n] = true; table.insert(out, n) end
        end
    end
    return out
end

-- Location data across static LOCATIONS and the world.lua registry.
local function _loc_info(id)
    if _locations and _locations[id] then return _locations[id] end
    local w = _world()
    if w and w.get_location then return w.get_location(id) end
    return nil
end

local function _loc_exists(id)
    return _loc_info(id) ~= nil
end

-- Iterate all known locations (static + generated).
local function _each_location(fn)
    for id, d in pairs(_locations or {}) do fn(id, d) end
    local w = _world()
    if w and w.all_locations then
        for id, d in pairs(w.all_locations()) do
            if not (_locations and _locations[id]) then fn(id, d) end
        end
    end
end

-- NPC structured response schema — separates thought from speech.
M.NPC_THINK_SCHEMA = [[{
    "type": "object",
    "required": ["intent", "speech"],
    "properties": {
        "intent": { "type": "string",
                    "description": "1-2 sentences: internal state, tensions, doubts. NOT dialogue." },
        "speech": { "type": "string",
                    "description": "Exact words spoken aloud. Empty string if NPC says nothing this turn." }
    }
}]]

-- Extended schema for autonomous off-screen beats: adds optional memory field.
-- Use this as opts.schema in tick_and_log + pass opts.on_memory callback.
-- memory is NOT required — only include when something significant happened.
M.NPC_OFF_SCREEN_SCHEMA = [[{
    "type": "object",
    "required": ["intent", "speech"],
    "properties": {
        "intent": { "type": "string",
                    "description": "1-2 sentences: internal state, tensions, doubts. NOT dialogue." },
        "speech": { "type": "string",
                    "description": "Exact words spoken aloud. Empty string if NPC says nothing." },
        "memory": {
            "type": "object",
            "description": "One persistent fact to remember about this event. Omit if nothing significant happened.",
            "required": ["entity", "category", "content"],
            "properties": {
                "entity":   { "type": "string", "description": "Who this fact is about (npc id or 'player')." },
                "category": { "type": "string", "description": "Short label, e.g. 'comportamento', 'segreto', 'relazione'." },
                "content":  { "type": "string", "description": "The fact to remember, 1 sentence." }
            }
        },
        "pad_delta": {
            "type": "object",
            "description": "Optional emotional shift from this event (PAD model). Use small values (±0.05 to ±0.15). Omit if no significant emotional impact.",
            "properties": {
                "p": { "type": "number", "description": "Pleasure delta: positive=more satisfied/happy, negative=more distressed/sad." },
                "a": { "type": "number", "description": "Arousal delta: positive=more activated/excited/tense, negative=calmer/spent." },
                "d": { "type": "number", "description": "Dominance delta: positive=more in control/assertive, negative=more submissive/helpless." }
            }
        }
    }
}]]

-- ============================================================
-- STATE REGISTRATION
-- ============================================================

-- Call after EVERY state assignment (set_initial_state, restore_state).
function M.set_state(s)
    _state = s
end

function M.get_state()
    return _state
end

-- Update NPC data, locations and travel_map used by tool closures.
-- Call from rebuild_npc_data() and after restore so tools see current data
-- even when get_tools() was called before set_initial_state/restore_state.
function M.set_npc_data(npc_data, locations, travel_map)
    _npc_data   = npc_data   or _npc_data
    _locations  = locations  or _locations
    _travel_map = travel_map or _travel_map
end

-- ============================================================
-- CONFIG
-- ============================================================

function M.set_config(cfg)
    _cfg = cfg
    _debug_log = cfg.debug_log or false
    -- Visual-Novel mode: load the asset catalog once (tag→bg/sprite matching).
    if cfg.vn and cfg.vn.catalog and _vn() then
        local ok, err = _vn().init(cfg.vn.catalog)
        if not ok then wlog.warn("adventure", "VN catalog: " .. tostring(err)) end
    end
    if cfg.log_file then
        local ok, gl = pcall(require, "lib/gamelog")
        if ok and type(gl) == "table" then
            local init_ok, err = gl.init(cfg.log_file, function() return _state end)
            if init_ok then
                _gamelog = gl
            else
                wlog.warn("adventure", "gamelog init failed: " .. tostring(err))
            end
        end
    end
    -- Expose entity editors as globals for the GUI design mode (idempotent).
    M.enable_editors()
end

-- ============================================================
-- ENTITY EDITORS (GUI design mode) — thin wrappers over persona.
-- Installed as Lua globals by M.enable_editors() (called from set_config).
-- The C++ routes /api/editor/npcs, /api/npc/<id>/full|patch, /api/npc/new
-- call these globals; each returns a JSON STRING. No parallel store: every
-- write goes through persona.patch (the same path the main LLM uses).
-- ============================================================

-- List NPCs for the editor picker → { ok, npcs:[{id,name,location}] }.
function M.editor_npcs()
    local persona = _persona()
    local out = {}
    if persona and persona.all then
        for id, d in pairs(persona.all()) do
            local loc = (_state and _state.npc_locations and _state.npc_locations[id])
                     or (_state and _state.gen_npc_locations and _state.gen_npc_locations[id])
            out[#out+1] = { id = id, name = d.name or id, location = loc }
        end
    elseif _npc_data then
        for id, d in pairs(_npc_data) do
            out[#out+1] = { id = id, name = (type(d)=="table" and d.name) or id }
        end
    end
    table.sort(out, function(a, b) return (a.name or "") < (b.name or "") end)
    return json.encode({ ok = true, npcs = out })
end

-- Full persona record + on-disk path → { ok, id, path, data }.
function M.editor_npc_get(id)
    local persona = _persona()
    if not persona then return json.encode({ ok=false, error="persona non disponibile" }) end
    local d = persona.get(id)
    if not d then return json.encode({ ok=false, error="npc non trovato: "..tostring(id) }) end
    local path = (persona.get_path() or "") .. tostring(id) .. ".lua"
    return json.encode({ ok=true, id=id, path=path, data=d })
end

-- Apply a persona.patch. patch_json is a JSON string (passed through from the
-- request body); identity fields go under patch.fields (whitelisted).
function M.editor_npc_patch(id, patch_json)
    local persona = _persona()
    if not persona then return json.encode({ ok=false, error="persona non disponibile" }) end
    local ok, patch = pcall(json.decode, patch_json or "{}")
    if not ok then return json.encode({ ok=false, error="patch JSON non valido: "..tostring(patch) }) end
    local res = persona.patch(id, patch)
    -- res is { ok=bool, error? }
    return json.encode(res or { ok=false, error="patch ha restituito nil" })
end

-- Create a new NPC persona file.
--   blank truthy → write an empty skeleton (register_static, NO LLM) the designer
--                  fills in by hand. This is how you "hook" a hand-made NPC: the
--                  file id IS the link — once <persona_dir>/<id>.lua exists, the
--                  adventure's persona calls (npc_object/agent_object/format) use it.
--   else         → LLM-generate from context (needs a non-empty context).
-- returns { ok, id, name } or { ok:false, error }.
function M.editor_npc_create(id, context, blank)
    local persona = _persona()
    if not persona then return json.encode({ ok=false, error="persona non disponibile" }) end
    id = (persona.sanitize_id and persona.sanitize_id(id)) or id
    if not id or id == "" then return json.encode({ ok=false, error="id non valido (snake_case)" }) end
    if persona.get(id) then return json.encode({ ok=false, error="esiste gia': "..id }) end
    if blank then
        -- register_static writes a minimal file from config and returns it.
        local data = persona.register_static(id, { name = (context ~= "" and context) or id })
        if not data then return json.encode({ ok=false, error="creazione scheletro fallita" }) end
        return json.encode({ ok=true, id=data.id, name=data.name, blank=true })
    end
    if not context or context == "" then
        return json.encode({ ok=false, error="serve un context per generare l'NPC (o usa 'Crea vuoto')" })
    end
    local data = persona.generate(id, context)
    if not data then return json.encode({ ok=false, error="generazione fallita (LLM)" }) end
    return json.encode({ ok=true, id=data.id, name=data.name })
end

-- Convert a chat/tile adventure to a Visual-Novel one in one shot. The VN scene
-- function is UNIVERSAL — adv.vn_scene({}) derives everything from existing state
-- (present NPCs, location/daypart bg tags, persona verbs/topics) — so we just
-- install it as get_vn_scene and load the catalog. The catalog file is created
-- (empty) by the C++ /api/scaffold route before this is called; the designer then
-- fills it with the VN Editor. Idempotent. path = absolute catalog/vn_scene.json.
function M.scaffold_vn(path)
    local vn = _vn()
    if not vn then return json.encode({ ok=false, error="lib visualnovel non disponibile" }) end
    if not path or path == "" then
        local root = rawget(_G, "ASSET_ROOT") or "."
        path = root .. "/catalog/vn_scene.json"
    end
    local ok, err = vn.init(path)
    if not ok then return json.encode({ ok=false, error="init catalog: " .. tostring(err) }) end
    _G.get_vn_scene = function() return M.vn_scene({}) end
    _cfg = _cfg or {}
    _cfg.vn = _cfg.vn or {}
    _cfg.vn.catalog = path
    return json.encode({ ok=true, catalog=path, modes={ "chat", "vn" } })
end

-- ── Location editors ─────────────────────────────────────────────────────────
-- Static locations (the adventure's LOCATIONS table) are read-only here; only
-- world.lua-generated locations are patchable (they are file-backed). The list
-- merges both and tags each with `source`.
function M.editor_locations()
    local out, seen = {}, {}
    for id, loc in pairs(_locations or {}) do
        out[#out+1] = { id=id, name=(type(loc)=="table" and loc.name) or id, source="static" }
        seen[id] = true
    end
    local w = _world()
    if w and w.all_locations then
        for id, loc in pairs(w.all_locations()) do
            if not seen[id] then
                out[#out+1] = { id=id, name=(type(loc)=="table" and loc.name) or id, source="generated" }
                seen[id] = true
            end
        end
    end
    table.sort(out, function(a, b) return (a.name or "") < (b.name or "") end)
    return json.encode({ ok=true, locations=out })
end

function M.editor_location_get(id)
    local w = _world()
    if w and w.get_location and w.get_location(id) then
        return json.encode({ ok=true, id=id, source="generated", editable=true, data=w.get_location(id) })
    end
    local loc = _locations and _locations[id]
    if loc then
        return json.encode({ ok=true, id=id, source="static", editable=false, data=loc })
    end
    return json.encode({ ok=false, error="location non trovata: " .. tostring(id) })
end

function M.editor_location_patch(id, patch_json)
    local w = _world()
    if not w or not w.location_patch then return json.encode({ ok=false, error="world non disponibile" }) end
    if not (w.get_location and w.get_location(id)) then
        return json.encode({ ok=false, error="location statica o inesistente: editabile solo se generata da world.lua" })
    end
    local ok, patch = pcall(json.decode, patch_json or "{}")
    if not ok then return json.encode({ ok=false, error="patch JSON non valido" }) end
    return json.encode(w.location_patch(id, patch))
end

function M.editor_location_create(id, data_json)
    local w = _world()
    if not w or not w.location_create then return json.encode({ ok=false, error="world non disponibile (richiede lib/world)" }) end
    local data = {}
    if data_json and data_json ~= "" then
        local ok, d = pcall(json.decode, data_json)
        if ok and type(d) == "table" then data = d end
    end
    return json.encode(w.location_create(id, data))
end

-- Install the editor + scaffold globals so the C++ routes can find them. Idempotent.
function M.enable_editors()
    _G.editor_npcs            = function()             return M.editor_npcs() end
    _G.editor_npc_get         = function(id)           return M.editor_npc_get(id) end
    _G.editor_npc_patch       = function(id, p)        return M.editor_npc_patch(id, p) end
    _G.editor_npc_create      = function(id, ctx, bl)  return M.editor_npc_create(id, ctx, bl) end
    _G.editor_locations       = function()             return M.editor_locations() end
    _G.editor_location_get    = function(id)           return M.editor_location_get(id) end
    _G.editor_location_patch  = function(id, p)        return M.editor_location_patch(id, p) end
    _G.editor_location_create = function(id, d)        return M.editor_location_create(id, d) end
    _G.scaffold_vn            = function(path)         return M.scaffold_vn(path) end
    _G.vn_reload_catalog      = function(path)
        local vn = _vn()
        if not vn then return false end
        return vn.init(path)
    end
    -- Object editor globals (delegate to world.lua if loaded)
    _G.editor_objects = function()
        local w = _world()
        if not w then return { ok=false, error="world.lua non caricato" } end
        return { ok=true, objects=w.editor_list_objects() }
    end
    _G.editor_object_get = function(id)
        local w = _world()
        if not w then return { ok=false, error="world.lua non caricato" } end
        return w.editor_object_get(id)
    end
    _G.editor_object_patch = function(id, p)
        local w = _world()
        if not w then return { ok=false, error="world.lua non caricato" } end
        return w.object_patch(id, p)
    end
    _G.editor_object_create = function(id, d)
        local w = _world()
        if not w then return { ok=false, error="world.lua non caricato" } end
        return w.object_create(id, d)
    end
end

-- Register an adventure-specific function called by /debug to append extra lines.
-- fn() must return a string (may be multi-line). Called only when /debug runs.
-- Example: adv.set_debug_fn(function() return "NXS-7 stock: "..state.formula.stock end)
function M.set_debug_fn(fn)
    _debug_fn = fn
end

-- ============================================================
-- DEBUG LOGGER
-- ============================================================

local function _log(name, args, result)
    if not _debug_log then return end
    table.insert(_dbg_tools, {
        name   = name,
        args   = args,
        result = type(result) == "string" and result:sub(1, 300) or tostring(result),
    })
    -- Cap: long sessions must not grow the log unbounded.
    -- remove-in-place — M.debug_tools references this same table.
    while #_dbg_tools > 100 do table.remove(_dbg_tools, 1) end
end

M.debug_tools = _dbg_tools  -- expose for /debug_log command

-- ============================================================
-- DEFAULT STATE
-- ============================================================

-- Returns a base state table. Adventure extends this with its own fields.
function M.default_state(cfg)
    cfg = cfg or _cfg
    local s = {
        player         = { name="", location="" },
        turn           = 0,
        npc_locations  = {},
        npc_activities = {},
        notes          = {},
        pending_event  = nil,
        _last_image_loc = nil,
    }
    if cfg.use_time then
        s.time         = "09:00"
        s.giorno_index = 1
        local days = cfg.days or {"monday"}
        s.day = days[1]
    end
    if cfg.use_inventory then
        s.inventario = {}
        s.soldi      = 0
    end
    return s
end

-- ============================================================
-- AGENT SYSTEM
-- ============================================================

-- Initialize all NPC agents. Call from set_initial_state (after state.player.name is set).
-- npc_agents: { id -> { system, model, provider, short_term_goals, long_term_goals, memory_enabled } }
-- max_calls_per_turn: max total LLM calls across all agents per turn (default 3).
function M.init_agents(npc_agents, max_calls_per_turn)
    _agents       = {}
    _turn_counter = agent_lib.new_turn_counter(max_calls_per_turn or 3)
    for id, a in pairs(npc_agents or {}) do
        _agents[id] = agent_lib.new(id, {
            system           = a.system or "",
            model            = a.model,
            provider         = a.provider,
            turn_counter     = _turn_counter,
            short_term_goals = a.short_term_goals or {},
            long_term_goals  = a.long_term_goals  or {},
            memory_enabled   = a.memory_enabled ~= false,
        })
    end
end

-- Register externally built agents (e.g. persona.agent_object) instead of
-- building them from configs. Creates and shares the turn counter across
-- all of them. Returns the counter (pass it to later persona.agent_object
-- calls for NPCs generated mid-game).
function M.set_agents(agents_map, max_calls_per_turn)
    _agents       = agents_map or {}
    _turn_counter = agent_lib.new_turn_counter(max_calls_per_turn or 3)
    for _, ag in pairs(_agents) do ag._turn_counter = _turn_counter end
    return _turn_counter
end

-- Add/replace a single agent after set_agents (e.g. NPC generated mid-game).
function M.add_agent(id, agent_obj)
    if agent_obj and _turn_counter then agent_obj._turn_counter = _turn_counter end
    _agents[id] = agent_obj
end

function M.snapshot_agents()
    local snaps = {}
    for id, ag in pairs(_agents) do snaps[id] = ag:agent_snapshot() end
    return snaps
end

function M.restore_agents(data)
    if not data then return end
    for id, snap in pairs(data) do
        if _agents[id] then _agents[id]:agent_restore(snap) end
    end
end

-- ============================================================
-- CLOCK / TICK SCHEDULER
-- ============================================================

local _tick_fn = nil

-- Register the off-screen simulation hook. Called once per simulated step
-- (default 30 min, override with CFG.tick_minutes) every time game time
-- advances — both via the advance_time tool and via M.advance_clock().
--   fn(time_str, day_str, giorno_index, minutes_step)
-- Typical body: NPC.tick(...), colocation checks, event accumulation.
-- Without this hook, a long advance_time is a single jump and the NPCs
-- teleport to their current routine slot: nothing "happened" in between.
function M.set_tick_fn(fn)
    _tick_fn = fn
end

-- Move the clock forward by mins on state s (single arithmetic step).
local function _advance_clock_step(s, mins, days)
    if not (s and s.time) then return end
    local h, m = s.time:match("(%d+):(%d+)")
    h = tonumber(h); m = tonumber(m) + mins
    if m >= 60 then h = h + math.floor(m/60); m = m % 60 end
    if h >= 24 then
        h = h % 24
        s.giorno_index = (s.giorno_index or 1) + 1
        if #days > 0 then
            for i, d in ipairs(days) do
                if d == s.day then s.day = days[(i % #days) + 1]; break end
            end
        end
    end
    s.time = string.format("%02d:%02d", h, m)
end

-- Advance game time by mins, running the tick hook once per simulated step.
-- Public: scripts can call it from commands (/sleep, /settime forward) and
-- get the same off-screen simulation as the advance_time tool.
-- Returns the number of ticks executed.
function M.advance_clock(mins)
    local s = _state
    if not (s and s.time) then return 0 end
    local days  = _cfg.days or {}
    local step  = _cfg.tick_minutes or 30
    local total = math.max(1, math.floor(mins or 0))
    local ticks = 0
    while total > 0 do
        local chunk = math.min(step, total)
        total = total - chunk
        _advance_clock_step(s, chunk, days)
        if _tick_fn then
            local ok, terr = pcall(_tick_fn, s.time, s.day, s.giorno_index, chunk)
            if not ok then
                wlog.warn("adventure", "tick_fn error at " .. tostring(s.time)
                    .. ": " .. tostring(terr))
            end
        end
        ticks = ticks + 1
    end
    return ticks
end

-- Run the tick hook N times at the given step WITHOUT advancing the game clock.
-- Use in before_ai_turn when you want NPCs to simulate their current state
-- (follow routine, fire events) but don't want the clock to jump forward.
-- The LLM's advance_time tool then controls all visible clock changes.
function M.run_tick(step)
    if not _tick_fn then return end
    local s = _state
    if not s then return end
    step = step or (_cfg.tick_minutes or 30)
    local ok, terr = pcall(_tick_fn, s.time or "00:00", s.day or "", s.giorno_index or 1, step)
    if not ok then
        wlog.warn("adventure", "run_tick error: " .. tostring(terr))
    end
end

-- ============================================================
-- ADVENTURE EVENTS
-- ============================================================
-- World-level scheduled/triggered events. Distinct from NPC-specific routine/
-- sequence events. Affect multiple NPCs, world state, weather, story flags.
-- Declared once at script load; fired by check_events() (called from before_turn).
-- Persist via state._events_fired = { id -> { turn, abs_min } }.
--
-- Event fields:
--   id        string        — unique identifier (required)
--   label     string        — human-readable name shown in /events + prompt
--   when      table         — { from="HH:MM", to="HH:MM" } time window (nil=any)
--   day       number|table  — giorno_index or list; nil=any day
--   prob      number        — fire probability 0.0-1.0 (default 1.0)
--   once      bool          — false=repeatable (default true)
--   cooldown  number        — minutes between repeats when once=false
--   condition fn(state)     — extra boolean guard (optional)
--   effect    fn(state)     — state mutations; returns narration string or nil.
--                             adventure script closures can reference local agents_map
--                             directly to call agent:react_live() for LLM beats.

function M.register_events(events_list)
    _events_registry = {}
    _events_order    = {}
    for _, ev in ipairs(events_list or {}) do
        if ev.id then
            _events_registry[ev.id] = ev
            table.insert(_events_order, ev.id)
        end
    end
end

local function _time_to_min(t)
    if not t then return nil end
    local h, m = tostring(t):match("(%d+):(%d+)")
    return h and (tonumber(h) * 60 + tonumber(m)) or nil
end

-- Check and fire eligible events. Called automatically from before_turn().
-- Returns list of { id, label, narration } for events fired this turn.
-- Fired events stored in state._pending_events for prompt injection.
function M.check_events()
    local s = _state
    if not s or #_events_order == 0 then return {} end
    s._events_fired = s._events_fired or {}

    local cur_min = _time_to_min(s.time)
    local day_idx = s.giorno_index or 1
    local abs_now = (day_idx - 1) * 1440 + (cur_min or 0)
    local fired   = {}

    for _, id in ipairs(_events_order) do
        local ev   = _events_registry[id]
        if not ev then goto continue end
        local last = s._events_fired[id]   -- nil | { turn, abs_min }

        -- once (default true): skip if already fired
        if ev.once ~= false and last then goto continue end

        -- repeatable: cooldown in game-minutes
        if ev.once == false and last and ev.cooldown then
            local elapsed = abs_now - (type(last) == "table" and last.abs_min or 0)
            if elapsed < ev.cooldown then goto continue end
        end

        -- day check
        if ev.day ~= nil then
            local days = type(ev.day) == "table" and ev.day or { ev.day }
            local match = false
            for _, d in ipairs(days) do if d == day_idx then match = true; break end end
            if not match then goto continue end
        end

        -- time window check
        if ev.when and cur_min then
            local from = _time_to_min(ev.when.from)
            local to   = _time_to_min(ev.when.to)
            if from and to and (cur_min < from or cur_min > to) then goto continue end
        end

        -- condition check
        if ev.condition then
            local ok, res = pcall(ev.condition, s)
            if not ok or not res then goto continue end
        end

        -- probability check
        if ev.prob and math.random() > ev.prob then goto continue end

        -- FIRE
        local narration = ""
        if ev.effect then
            local ok, res = pcall(ev.effect, s)
            if ok and type(res) == "string" and res ~= "" then
                narration = res
            elseif not ok then
                wlog.warn("adventure", "event '" .. id .. "' effect error: " .. tostring(res))
            end
        end

        s._events_fired[id] = { turn = s.turn or 0, abs_min = abs_now }

        local entry = { id = id, label = ev.label or id, narration = narration }
        table.insert(fired, entry)

        if _gamelog then
            _gamelog.write("EVENT",
                (ev.label or id) .. (narration ~= "" and (" | " .. narration:sub(1, 100)) or ""))
        end

        ::continue::
    end

    if #fired > 0 then
        s._pending_events = fired
    end
    return fired
end

-- Inject fired event narrations into system prompt.
-- Call from get_system_prompt() in the adventure script.
-- One-shot: clears state._pending_events after reading (re-called same turn = "").
function M.prompt_events()
    local s = _state
    if not s or not s._pending_events or #s._pending_events == 0 then return "" end
    local blocks = {}
    for _, ev in ipairs(s._pending_events) do
        local line = "## EVENTO: " .. ev.label
        if ev.narration ~= "" then line = line .. "\n" .. ev.narration end
        table.insert(blocks, line)
    end
    s._pending_events = nil
    return "\n\n" .. table.concat(blocks, "\n\n")
end

-- ============================================================
-- BEFORE_AI_TURN
-- ============================================================

-- Reset per-turn call guard and agent caches. Call from before_ai_turn().
-- Optional player_input: if provided and gamelog active, logs it to disk.
function M.before_turn(player_input)
    _tool_calls = {}
    _turn_seen  = _state and _state.turn or nil
    agent_lib.reset_all_turns(_agents, _turn_counter)

    -- Gamelog: section separator + player action
    if _gamelog then
        local turn = _state and _state.turn
        _gamelog.separator("TURN " .. (turn ~= nil and tostring(turn) or "?"))
        if player_input and player_input ~= "" then
            _gamelog.write("PLAYER", player_input)
        end
    end

    -- Gamelog: lazily wrap tick_fn so every NPC tick step is logged to disk.
    -- _tick_fn_raw saved so _run_sim can bypass the wrapper (no double logging).
    if _gamelog and _tick_fn and not _tick_fn_logged then
        _tick_fn_raw = _tick_fn
        local raw = _tick_fn_raw
        _tick_fn = function(time_str, day_str, gidx, chunk)
            local s = _state
            local before = {}
            for k, v in pairs((s and s.npc_locations) or {}) do before[k] = v end

            local ok_ag, ag = pcall(require, "lib/agent")
            if ok_ag then ag._sim_log_entries = {} end

            local ok_t, terr = pcall(raw, time_str, day_str, gidx, chunk)
            if not ok_t then
                _gamelog.write("TICK_ERR", tostring(terr)); return
            end

            if ok_ag and ag._sim_log_entries then
                for _, e in ipairs(ag._sim_log_entries) do
                    local tag = e.kind == "llm_dry" and "TICK_LLM"
                             or e.kind == "event"   and "TICK_EVT"
                             or "TICK"
                    local txt = (e.text or ""):sub(1, 90)
                    _gamelog.write(tag, (e.npc or "?") .. ": " .. txt)
                end
                ag._sim_log_entries = nil
            end

            for id, loc in pairs((s and s.npc_locations) or {}) do
                if before[id] and before[id] ~= loc then
                    _gamelog.write("MOV", id .. ": " .. (before[id] or "?") .. " → " .. loc)
                end
            end
        end
        _tick_fn_logged = true
    end
    -- Propagate the current turn to world/persona so their file-backed
    -- events get turn-stamped (post-undo reconciliation, see M.restore).
    local turn = _state and _state.turn
    if turn then
        local w = _world()
        if w and w.set_turn then w.set_turn(turn) end
        local okp, p = pcall(require, "lib/persona")
        if okp and type(p) == "table" and p.set_turn then p.set_turn(turn) end
    end
    -- Harvest completed async LLM jobs (ambient events, background
    -- generation). Results land here, at a safe point before the new turn.
    local okj, jobs = pcall(require, "lib/jobs")
    if okj and type(jobs) == "table" and jobs.poll_all then
        pcall(jobs.poll_all)
    end
    -- Check adventure events (fires eligible ones, stores in state._pending_events
    -- for prompt injection via M.prompt_events() in get_system_prompt).
    if #_events_order > 0 then M.check_events() end
    return nil
end

-- Call from after_ai_turn(narration, raw_reply) in adventure script.
-- Logs the narration and any tool calls found in raw_reply to disk.
-- No-op when gamelog not configured.
function M.after_turn(narration, raw_reply)
    if not _gamelog then return end
    if narration and narration ~= "" then
        local short = narration:sub(1, 250) .. (narration:len() > 250 and "…" or "")
        _gamelog.write("NARR", short)
    end
    if raw_reply and raw_reply ~= "" then
        local ok, data = pcall(json.decode, raw_reply)
        if ok and type(data) == "table" then
            local calls = data.tool_calls
            if type(calls) == "table" then
                for _, tc in ipairs(calls) do
                    local fn   = (type(tc["function"]) == "table") and tc["function"] or tc
                    local name = fn.name or "?"
                    local args = fn.arguments or ""
                    if type(args) == "table" then args = json.encode(args) end
                    _gamelog.write("TOOL", name .. ": " .. tostring(args):sub(1, 120))
                end
            end
        end
    end
end

-- ============================================================
-- SYSTEM PROMPT BLOCKS
-- ============================================================

-- All positions of all NPCs (not just present ones).
-- Shows [id] explicitly so LLM uses correct ids in tool calls.
function M.prompt_npc_positions(npc_data)
    local s = _state
    if not s then return "" end
    local lines = {}
    for id, loc in pairs(s.npc_locations or {}) do
        local npc = npc_data and npc_data[id]
        if npc then
            local act = (s.npc_activities or {})[id]
            table.insert(lines, string.format("  %s [id:%s] → %s%s",
                npc.name or id, id, loc, act and (" | " .. act) or ""))
        end
    end
    if #lines == 0 then return "" end
    table.sort(lines)
    return "\n\n## NPC POSITIONS\n" .. table.concat(lines, "\n")
end

-- Personalities of NPCs present at player's current location.
-- Shows [id] so LLM uses correct ids in think_as_npc / move_npc calls.
function M.prompt_npc_personalities(npc_data)
    local s = _state
    if not s then return "" end
    local loc_id = s.player.location
    local blocks = {}
    for id, loc in pairs(s.npc_locations or {}) do
        if loc == loc_id then
            local npc = npc_data and npc_data[id]
            if npc and npc.description then
                table.insert(blocks, string.format("=== %s [id:%s]%s ===\n%s",
                    npc.name or id, id,
                    npc.age and (" — " .. npc.age .. " anni") or "",
                    npc.description))
            end
        end
    end
    if #blocks == 0 then return "" end
    return "\n\n## NPC PRESENTI — PERSONALITÀ\n" .. table.concat(blocks, "\n\n")
end

-- Notes with scope filtering (player/public go to master prompt; npc:id only to think_as_npc).
-- "public" notes are witness-gated for think_as_npc (see M.get_tools) but
-- the NARRATOR reads this block for ALL of them unconditionally — it must
-- stay omniscient for coherent narration. That omniscience is exactly what
-- let a real bug through TWICE: a rare/dramatic private event marked
-- "public" got framed here as "tutti sanno", and the narrator itself wrote
-- an UNRELATED NPC (never in that scene) declaring "lo sanno tutti" about
-- it — the witness gate only blocks the per-NPC think_as_npc injection, it
-- can't stop the narrator's own free prose. So every public fact is shown
-- WITH its real witness list (when known) so the narrator has a concrete
-- fact instead of an assumption to reach for. A note with no witnesses
-- (legacy, or genuinely world-wide news) is flagged explicitly so the
-- narrator doesn't default to "everyone knows" for something private.
function M.prompt_notes()
    local s = _state
    if not s or not s.notes or #s.notes == 0 then return "" end
    local player_notes, public_notes = {}, {}
    for _, n in ipairs(s.notes) do
        if type(n) == "string" then
            table.insert(player_notes, n)
        elseif n.scope == "public" then
            local line = n.content or ""
            if type(n.witnesses) == "table" and next(n.witnesses) then
                local w = {}
                for id in pairs(n.witnesses) do table.insert(w, id) end
                table.sort(w)
                line = line .. "  [testimoni diretti: " .. table.concat(w, ", ") .. " — "
                    .. "SOLO loro possono saperlo per certo; NON far dire ad altri NPC "
                    .. "che 'lo sanno tutti']"
            else
                line = line .. "  [nessun testimone registrato — NON presumere che sia "
                    .. "conoscenza diffusa: non farlo citare da un NPC a meno che non "
                    .. "sia stato detto esplicitamente o sia un annuncio genuinamente pubblico]"
            end
            table.insert(public_notes, line)
        elseif (not n.scope) or n.scope == "player" then
            table.insert(player_notes, n.content or "")
        end
    end
    local block = ""
    if #player_notes > 0 then
        block = block .. "\n\n## NOTE PERSONALI (solo tu)\n"
        for _, n in ipairs(player_notes) do block = block .. "- " .. n .. "\n" end
    end
    if #public_notes > 0 then
        block = block .. "\n\n## FATTI (con testimoni — vedi annotazione per ciascuno)\n"
        for _, n in ipairs(public_notes) do block = block .. "- " .. n .. "\n" end
    end
    return block
end

-- Player's visible appearance, for injection into NPC agent prompts, image
-- prompts, and the arrival scene. ONE SOURCE: state.player.appearance (body)
-- + state.player.outfit (clothes). The master fills these in set_initial_state,
-- typically from a persona ("player"): state.player.appearance =
-- persona.format_appearance("player"). Returns "" when nothing is set.
function M.player_appearance()
    local p = (_state and _state.player) or {}
    local name   = p.name or "Il protagonista"
    local body   = p.appearance
    local outfit = p.outfit
    if body and body ~= "" and outfit and outfit ~= "" then
        return name .. ": " .. body .. " Indossa: " .. outfit
    elseif body and body ~= "" then
        return name .. ": " .. body
    elseif outfit and outfit ~= "" then
        return name .. " indossa: " .. outfit
    end
    return ""
end

-- Pending event injected into prompt then cleared.
-- Shows player's current location exits + all 1-hop reachable destinations.
-- Gives LLM exact IDs to use in move_player without guessing.
function M.prompt_exits(travel_map, locations)
    local s = _state
    if travel_map then _travel_map = travel_map end
    if locations  then _locations  = locations  end
    if not s or not (_travel_map or _world()) then return "" end
    local cur = s.player.location or ""

    -- Full BFS from current location (unlimited depth) over the MERGED graph
    -- (static TRAVEL_MAP + world.lua generated connections).
    -- visited[id] = true once seen; parent[id] = predecessor; depth[id] = hops.
    local visited = { [cur] = true }
    local parent  = {}
    local depth   = { [cur] = 0 }
    local order   = {}   -- BFS order (excludes cur)
    local bfs     = { cur }
    local head    = 1
    while head <= #bfs do
        local node = bfs[head]; head = head + 1
        for _, nb in ipairs(_neighbors(node)) do
            if not visited[nb] then
                visited[nb] = true
                parent[nb]  = node
                depth[nb]   = depth[node] + 1
                table.insert(bfs, nb)
                table.insert(order, nb)
            end
        end
    end

    -- For display: first hop from cur (for "via X" label)
    local function first_hop(id)
        local node = id
        while parent[node] and parent[node] ~= cur do node = parent[node] end
        return node
    end

    -- Group by depth; everything at distance >= 5 goes into the "5+" bucket
    -- (display loop below only renders d = 1..5).
    local by_depth = {}
    for _, id in ipairs(order) do
        local d = math.min(depth[id], 5)
        if not by_depth[d] then by_depth[d] = {} end
        local loc  = _loc_info(id)
        local name = loc and loc.name or id
        if d == 1 then
            table.insert(by_depth[d], string.format("%s (\"%s\")", id, name))
        else
            table.insert(by_depth[d], string.format("%s (\"%s\") — via %s",
                id, name, first_hop(id)))
        end
    end

    local out = "\n\n## SPOSTAMENTI DISPONIBILI — usa move_player(id) con un id esatto"
    local labels = { "Diretti (1 passo):", "Raggiungibili (2):", "Raggiungibili (3):",
                     "Raggiungibili (4):", "Raggiungibili (5+):" }
    for d = 1, 5 do
        if by_depth[d] and #by_depth[d] > 0 then
            local label = labels[d] or string.format("Dist. %d:", d)
            out = out .. "\n" .. string.format("%-20s", label)
                      .. table.concat(by_depth[d], "  |  ")
        end
    end
    out = out .. "\nmove_player(id) percorre automaticamente qualsiasi distanza."
    return out
end

-- ============================================================
-- VISUAL-NOVEL MODE  (opt-in: CFG.vn + get_vn_scene = adv.vn_scene)
-- ============================================================

-- Default verb-coin palette (override per-adventure via CFG.vn.verbs/moods/...).
M.VN_DEFAULT_VERBS = {
    npc  = { "parla", "chiedi", "dai", "mostra" },
    obj  = { "esamina", "usa", "prendi" },
    exit = { "vai" },
    item = { "usa", "esamina", "molla" },
}
M.VN_DEFAULT_MOODS       = { "neutro", "gentile", "arrabbiato", "seducente", "ironico" }
M.VN_DEFAULT_INTENSITIES = { "poco", "molto" }

-- System-prompt block: teaches the LLM how to verbalise a composed [AZIONE].
-- Inject into get_system_prompt() when VN mode is on. ONE LLM call per turn
-- produces both the player's line and the reaction (no extra call).
function M.prompt_vn_action()
    return [[

## MODALITÀ AZIONE COMPOSTA (Visual Novel)
Se l'input del giocatore inizia con "[AZIONE]" è un'azione scelta da menu, formato:
  [AZIONE] verbo=<v> target=<id> tipo=<npc|obj|exit|item> umore=<m> intensita=<i> argomento=<a>
Procedi così, in UNA sola risposta:
1) Scrivi la battuta/azione del GIOCATORE in PRIMA PERSONA, coerente con verbo+umore+intensità,
   come PRIMA riga tra virgolette basse «...» (dialogo) o descrizione tra «» (azione fisica).
2) Poi narra la REAZIONE di NPC/mondo.
umore e intensita modulano SOLO il tono della battuta del giocatore; assenti = neutro.
argomento (se presente, ultimo campo) = di cosa parla il giocatore: può essere
  "npc:<id>" (un'altra persona), "obj:<id>" (un oggetto) o "topic:<testo>" (un tema);
  fai sì che la battuta del giocatore verta su quell'argomento.
bersaglio=<id> (con tipo=item, verbo=usa) = il giocatore usa l'oggetto target
  SULL'entità bersaglio (un NPC o un oggetto): narra l'uso combinato.
Applica gli effetti con i tool (move_player, object_action, ...) come di consueto.]]
end

-- Structured 1-hop exits from the player's current location, over the merged
-- graph (static TRAVEL_MAP + world.lua connected_to). Returns
-- { {location=id, label=name}, ... }. Used by the VN scene (and any click-nav).
function M.direct_exits()
    local s = _state
    if not s or not s.player then return {} end
    local cur = s.player.location or ""
    local out = {}
    for _, nb in ipairs(_neighbors(cur)) do
        local loc = _loc_info(nb)
        out[#out + 1] = { location = nb, label = (loc and loc.name) or nb }
    end
    table.sort(out, function(a, b) return a.location < b.location end)
    return out
end

-- Build the Visual-Novel scene descriptor for get_vn_scene().
-- Derives the room, present NPCs and exits from existing state; the adventure
-- only supplies the TAGS that pick the assets (it knows its own outfit/mood).
-- opts = {
--   bg_tags  = function(state)->{tags} | {tags},  -- background selector tags
--                (default: { location, daypart(time) })
--   npc_tags = function(npc_id, state)->{tags},   -- per-NPC sprite tags (default {})
--   npcs     = function(state)->{ids},            -- override present-NPC list
--                (default: NPCs whose npc_locations == player's location)
-- }
-- Returns a JSON string (the script just `return adv.vn_scene{...}`).
function M.vn_scene(opts)
    opts = opts or {}
    local s   = _state or {}
    local cur = (s.player and s.player.location) or ""
    local vn  = _vn()
    if not vn or not vn.loaded() then
        return json.encode({ location = cur, npcs = {}, exits = M.direct_exits() })
    end

    -- Present NPCs at the player's location.
    local present
    if type(opts.npcs) == "function" then
        present = opts.npcs(s) or {}
    else
        present = {}
        for id, loc in pairs(s.npc_locations or {}) do
            if loc == cur then present[#present + 1] = id end
        end
        table.sort(present)
    end

    -- Variant tags: daypart + adventure-supplied world-state tags. Used to choose
    -- AMONG the backgrounds explicitly linked to this location (location field).
    -- opts.variant_tags(state)->{tags} lets the adventure add state flags
    -- (e.g. "frigo" when the object is present) so the right variant is picked.
    local variant_tags = {}
    if s.time then variant_tags[#variant_tags + 1] = vn.daypart(s.time) end
    if type(opts.variant_tags) == "function" then
        for _, t in ipairs(opts.variant_tags(s) or {}) do variant_tags[#variant_tags + 1] = t end
    end

    -- Background tags (LEGACY tag-matching fallback for catalogs without a
    -- `location` field: include the location id so it can be matched by tag).
    local bg_tags
    if type(opts.bg_tags) == "function" then
        bg_tags = opts.bg_tags(s) or {}
    elseif type(opts.bg_tags) == "table" then
        bg_tags = opts.bg_tags
    else
        bg_tags = { cur }
        for _, t in ipairs(variant_tags) do bg_tags[#bg_tags + 1] = t end
    end

    -- Per-NPC sprite tags.
    local npc_tags = {}
    for _, id in ipairs(present) do
        npc_tags[id] = (type(opts.npc_tags) == "function")
                       and (opts.npc_tags(id, s) or {}) or {}
    end

    -- Per-NPC context for sprite condition evaluation (outfit / stats / flags).
    -- persona.current_outfit reads the routine schedule; stats from state.npc_stats;
    -- flags from state.npc_flags (adventure can populate these as needed).
    local npc_ctxs = {}
    local p = _persona()
    for _, id in ipairs(present) do
        local outfit = ""
        if p and p.current_outfit then
            local ok, v = pcall(p.current_outfit, id, s.time)
            if ok and v then outfit = tostring(v) end
        end
        npc_ctxs[id] = {
            outfit = outfit,
            stats  = (type(s.npc_stats) == "table" and s.npc_stats[id]) or {},
            flags  = (type(s.npc_flags) == "table" and s.npc_flags[id]) or {},
        }
    end

    -- Collect display names so build_scene can include them in scene.npcs entries.
    local npc_names = {}
    local function _npc_name_early(id)
        if _npc_data and _npc_data[id] and _npc_data[id].name then return _npc_data[id].name end
        local pe = _persona()
        if pe and pe.get then local d = pe.get(id); if d and d.name then return d.name end end
        return id
    end
    for _, id in ipairs(present) do npc_names[id] = _npc_name_early(id) end

    local scene = vn.build_scene({
        location     = cur, time = s.time, day = s.day,
        state        = s,
        bg_tags      = bg_tags,
        variant_tags = variant_tags,
        npcs_present = present,
        npc_tags     = npc_tags,
        npc_ctxs     = npc_ctxs,
        npc_names    = npc_names,
        exits        = M.direct_exits(),
    })

    -- Attach the verb-coin palette (verbs by target type, moods, intensities).
    local vncfg = (_cfg and _cfg.vn) or {}
    -- Priority: CFG.vn.verbs > catalog palette section > engine defaults.
    local cat_pal = vn and vn.get_catalog_palette and vn.get_catalog_palette() or {}
    local verbs = vncfg.verbs or {
        npc = (cat_pal.npc_verbs and #cat_pal.npc_verbs > 0)
              and cat_pal.npc_verbs or M.VN_DEFAULT_VERBS.npc,
        obj = (cat_pal.obj_verbs and #cat_pal.obj_verbs > 0)
              and cat_pal.obj_verbs or M.VN_DEFAULT_VERBS.obj,
    }

    -- Per-verb second step "kind": talk verbs → conversation topics submenu;
    -- give/show verbs → pick an item to hand over; others → just mood. So after
    -- "dai" you choose an OBJECT, not a topic. CFG.vn.verb_kinds overrides.
    local TALK = { parla=1, chiedi=1, parlare=1, chiedere=1, racconta=1, raccontare=1 }
    local GIVE = { dai=1, dare=1, mostra=1, mostrare=1, regala=1, regalare=1, consegna=1, porgi=1 }
    local verb_kinds = {}
    for _, v in ipairs(verbs.npc or {}) do
        verb_kinds[v] = TALK[v] and "talk" or (GIVE[v] and "give" or "act")
    end
    -- Catalog palette verb_kinds overrides TALK/GIVE defaults — allows non-Italian verb names.
    if type(cat_pal.verb_kinds) == "table" then
        for k, val in pairs(cat_pal.verb_kinds) do verb_kinds[k] = val end
    end
    -- CFG.vn.verb_kinds wins over everything (script-level authority).
    if type(vncfg.verb_kinds) == "table" then
        for k, val in pairs(vncfg.verb_kinds) do verb_kinds[k] = val end
    end

    -- L2 verb options: from catalog verb_options + CFG.vn.verb_l2 overrides.
    local verb_l2 = {}
    if vn and vn.get_verb_options then
        for k, v in pairs(vn.get_verb_options()) do verb_l2[k] = v end
    end
    if type(vncfg.verb_l2) == "table" then
        for k, v in pairs(vncfg.verb_l2) do verb_l2[k] = v end
    end

    scene.palette = {
        verbs       = verbs,
        verb_kinds  = verb_kinds,
        verb_l2     = verb_l2,
        moods       = vncfg.moods       or M.VN_DEFAULT_MOODS,
        intensities = vncfg.intensities or M.VN_DEFAULT_INTENSITIES,
    }

    -- Per-hotspot verbs. Object hotspots (type "obj"/absent): palette obj verbs +
    -- the object's own state-machine actions (world.lua) — real gating, never an
    -- invented list. Exit hotspots (type "exit"): handled as navigation, no verbs.
    local w = _world()
    if scene.background and scene.background.hotspots then
        for _, h in ipairs(scene.background.hotspots) do
            if h.type ~= "exit" then
                local vlist, seen = {}, {}
                for _, v in ipairs(verbs.obj or { "esamina" }) do
                    if not seen[v] then seen[v] = true; vlist[#vlist + 1] = v end
                end
                if w and h.object and vn.verbs_for_object then
                    for _, v in ipairs(vn.verbs_for_object(w, h.object)) do
                        if not seen[v] then seen[v] = true; vlist[#vlist + 1] = v end
                    end
                end
                h.verbs = vlist
            end
        end
    end

    -- Per-NPC verbs + conversation topics. Sources are ALL engine state, never a
    -- parallel store: generic palette + persona.vn_verbs (curated in the persona
    -- file) for verbs; present NPCs + world objects in the room + persona.topics
    -- for the "parla" submenu. opts.npc_verbs/npc_topics add adventure extras.
    local persona = _persona()
    local function _npc_name(id)
        if _npc_data and _npc_data[id] and _npc_data[id].name then return _npc_data[id].name end
        if persona and persona.get then
            local d = persona.get(id); if d and d.name then return d.name end
        end
        return id
    end
    for _, npc in ipairs(scene.npcs or {}) do
        local id = npc.id
        local vlist, vseen = {}, {}
        local function add_verb(v) if v and v ~= "" and not vseen[v] then vseen[v] = true; vlist[#vlist + 1] = v end end
        for _, v in ipairs(verbs.npc or {}) do add_verb(v) end
        if persona and persona.vn_verbs then
            for _, v in ipairs(persona.vn_verbs(id)) do add_verb(v) end
        end
        if type(opts.npc_verbs) == "function" then
            for _, v in ipairs(opts.npc_verbs(id, s) or {}) do add_verb(v) end
        end
        npc.verbs = vlist

        local topics, tseen = {}, {}
        local function add_topic(ref, label)
            if ref and ref ~= "" and not tseen[ref] then
                tseen[ref] = true; topics[#topics + 1] = { ref = ref, label = label or ref }
            end
        end
        for _, other in ipairs(present) do
            if other ~= id then add_topic("npc:" .. other, _npc_name(other)) end
        end
        if w and w.get_location then
            local loc = w.get_location(cur)
            if loc and type(loc.objects) == "table" then
                for _, oid in ipairs(loc.objects) do
                    local obj = w.get_object and w.get_object(oid)
                    add_topic("obj:" .. oid, (obj and obj.name) or oid)
                end
            end
        end
        if persona and persona.topics then
            for _, t in ipairs(persona.topics(id)) do add_topic("topic:" .. t, t) end
        end
        if type(opts.npc_topics) == "function" then
            for _, t in ipairs(opts.npc_topics(id, s) or {}) do
                if type(t) == "table" then add_topic(t.ref, t.label)
                else add_topic("topic:" .. tostring(t), tostring(t)) end
            end
        end
        npc.topics = topics
    end

    -- Player inventory as composable targets ("usa X su Y"). Items are free-form
    -- strings (cambia_inventario); opts.inventory can override/shape them.
    local inv = {}
    if type(opts.inventory) == "function" then
        for _, it in ipairs(opts.inventory(s) or {}) do
            if type(it) == "table" and it.id then inv[#inv + 1] = { id = it.id, label = it.label or it.id }
            elseif type(it) == "string" then inv[#inv + 1] = { id = it, label = it } end
        end
    elseif type(s.inventario) == "table" then
        for _, it in ipairs(s.inventario) do
            if type(it) == "string" then inv[#inv + 1] = { id = it, label = it }
            elseif type(it) == "table" and it.id then inv[#inv + 1] = { id = it.id, label = it.label or it.id } end
        end
    end
    scene.inventory = inv

    return json.encode(scene)
end

-- Hot-reload the VN asset catalog (called by the engine after the VnEditor
-- saves catalog/vn_scene.json, so edits take effect without a restart).
function M.vn_reload(path)
    local vn = _vn()
    if not vn then return false end
    return vn.init(path or (_cfg and _cfg.vn and _cfg.vn.catalog))
end

function M.prompt_pending_event()
    local s = _state
    if not s or not s.pending_event then return "" end
    local ev = "\n\n[EVENTO AUTOMATICO] " .. s.pending_event
    s.pending_event = nil
    return ev
end

-- Standard WORKFLOW block for MODE C.
-- extra_tools: string with additional numbered tool lines.
-- extra_rules: string with additional tool-specific rules.
function M.prompt_workflow(extra_tools, extra_rules)
    local lines = {
        "",
        "════════════════════════════════════════════════════════════════",
        "WORKFLOW OBBLIGATORIO (tool PRIMA della narrazione, nell'ordine)",
        "════════════════════════════════════════════════════════════════",
    }
    local n = 1
    local function wl(s) table.insert(lines, string.format("%2d. %s", n, s)); n=n+1 end

    if _cfg.use_agents then
        wl("[opt] think_as_npc(id, situation) — reazione NPC presente. Cached/turno. MAX 1/NPC.")
    end
    if _cfg.use_time then
        wl("[opt] advance_time(minutes)       — PRIMA di azioni che richiedono tempo. MAX 1/turno.")
        wl("[opt] sleep_until(time)           — DORMIRE o saltare a un orario (HH:MM). NON advance_time. MAX 1/turno.")
    end
    wl("[opt] move_player(location)       — spostamento esplicito. MAX 1/turno.")
    wl("[opt] move_npc(id, location)      — spostamento NPC narrativo. MAX 1/NPC/turno.")
    wl("[opt] set_activity(id, activity)  — aggiorna attività NPC. MAX 1/NPC/turno.")
    if _cfg.use_inventory then
        wl("[opt] cambia_inventario(...)      — SOLO su azione esplicita giocatore. VIETATO per costi impliciti.")
    end
    if _cfg.use_notes then
        wl("[opt] remember(note, scope)       — nota. scope: player|public|npc. MAX 2/turno.")
    end
    if _cfg.use_memory then
        wl("[opt] memory_write/read           — fatti NPC cross-session. MAX 3/turno write.")
    end
    if extra_tools and extra_tools ~= "" then
        table.insert(lines, extra_tools)
    end
    table.insert(lines, "Poi: narrazione. STOP.")

    -- Feature-specific rules (only injected when feature is active)
    if _cfg.use_agents then
        table.insert(lines, [[
USO think_as_npc — CRITICO:
  "intent" = pensiero interno — descrivi in TERZA PERSONA, MAI virgolette.
  "speech" = parole esatte dette ad alta voce — copia VERBATIM tra «».
             Stringa vuota = l'NPC non parla questo turno.
  MAI parafrasare "speech". MAI mettere "intent" tra virgolette.]])
    end

    local rules = {}
    if _cfg.use_agents then
        table.insert(rules, "think_as_npc   → solo se NPC nella stessa location del protagonista. "
            .. "Cached: chiamate ripetute sulla stessa id restituiscono lo stesso risultato.")
    end
    table.insert(rules, "move_player    → SOLO su spostamento esplicito del giocatore. "
        .. "NON chiamare se il protagonista è già nella stanza.")
    table.insert(rules, "move_npc       → PRIMA di narrare un NPC in una nuova location. "
        .. "MAX 1 per NPC per turno.")
    if _cfg.use_time then
        table.insert(rules, "advance_time   → PRIMA di narrare qualsiasi azione che richiede tempo. "
            .. "Stima accurata dei minuti. MAX 1/turno.")
        table.insert(rules, "sleep_until    → se il giocatore DORME o aspetta fino a un'ora: "
            .. "sleep_until(time=\"HH:MM\") con l'ora di RISVEGLIO. NON usare advance_time "
            .. "per dormire (sbaglieresti i minuti). La narrazione del risveglio DEVE "
            .. "combaciare con l'ora restituita dal tool.")
    end
    if _cfg.use_inventory then
        table.insert(rules, "cambia_inventario → SOLO su azione esplicita del giocatore "
            .. "(raccoglie, cede, compra, vende, spende fisicamente). "
            .. "ASSOLUTAMENTE VIETATO per costi narrativi impliciti "
            .. "(caffè di sottofondo, taxi non richiesto, mance inventate ecc).")
    end
    if _cfg.use_notes then
        table.insert(rules, "remember       → solo per fatti esplicitamente notati o detti dal giocatore. "
            .. "scope 'public' RARO: solo annunci/eventi che l'intero mondo conoscerebbe "
            .. "davvero all'istante. Un fatto privato/intimo accaduto in una stanza NON è "
            .. "'public' solo perché drammatico — resta 'player' (default) o 'npc:id'. "
            .. "scope 'npc:id' per fatti condivisi solo con quell'NPC. MAX 2/turno.")
    end
    if _cfg.use_memory then
        table.insert(rules, "memory_write   → solo fatti avvenuti e fisicamente confermati. "
            .. "VIETATO: emozioni dedotte, intenzioni future, piani non agiti, interpretazioni. "
            .. "MAX 3/turno.")
    end

    if #rules > 0 then
        table.insert(lines, "\nREGOLE TOOL:")
        for _, r in ipairs(rules) do
            table.insert(lines, "  • " .. r)
        end
    end

    if extra_rules and extra_rules ~= "" then
        table.insert(lines, extra_rules)
    end
    return table.concat(lines, "\n")
end

-- ============================================================
-- HUD
-- ============================================================

-- Standard two-line HUD. adventure_stats: optional extra line string (adventure-specific).
function M.display_state(npc_data, locations, adventure_stats)
    local s = _state
    if not s then return "[ loading... ]" end
    if locations then _locations = locations end
    local loc_id = s.player.location
    local loc    = _loc_info(loc_id)
    local presenti = {}
    for id, l in pairs(s.npc_locations or {}) do
        if l == loc_id then
            local npc = npc_data and npc_data[id]
            table.insert(presenti, npc and (npc.name or id) or id)
        end
    end
    -- Generated NPCs (persona.generate/generate_npc tool) live in
    -- gen_npc_locations, a separate field from npc_locations — without this
    -- the player sees them narrated ("qui c'è Carlo") but the HUD says no one
    -- is here, because the HUD only ever looked at the static-NPC field.
    for id, l in pairs(s.gen_npc_locations or {}) do
        if l == loc_id then
            local p = _persona()
            local d = p and p.get and p.get(id)
            table.insert(presenti, (d and d.name) or id)
        end
    end
    table.sort(presenti)

    local time_part = ""
    if s.time then
        time_part = string.format(" %s %s G%d", s.time, s.day or "", s.giorno_index or 1)
    end

    local line1 = string.format("[ %s%s | 📍 %s | qui: %s ]",
        s.player.name, time_part,
        loc and loc.name or loc_id,
        #presenti > 0 and table.concat(presenti, ", ") or "—")

    local parts = {}
    if adventure_stats and adventure_stats ~= "" then
        table.insert(parts, adventure_stats)
    end
    if s.inventario then
        local inv = s.inventario or {}
        local inv_str = #inv > 0 and table.concat(inv, ", ") or "(vuoto)"
        table.insert(parts, string.format("💰€%d | 🎒 %s", s.soldi or 0, inv_str))
    end

    if #parts > 0 then
        return line1 .. "\n[ " .. table.concat(parts, " | ") .. " ]"
    end
    return line1
end

-- ============================================================
-- RESPONSE HELPERS
-- ============================================================

-- Parse LLM reply. Returns (r, nil) on success, (nil, error_result) on failure.
function M.parse_reply(reply)
    local r, _, err
    if safe_json_decode then
        r, _, err = safe_json_decode(reply)
    else
        local ok; ok, r = pcall(json.decode, reply)
        if not ok then err = r; r = nil end
    end
    if not r or type(r) ~= "table" then
        return nil, { success=false, error="JSON non valido: " .. tostring(err or reply) }
    end
    local narr = r.narration or r.narrative or ""
    if narr == "" then
        return nil, { success=false, error="Campo narration mancante" }
    end
    return r, nil
end

function M.response_ok(narration, game_over, reason)
    return {
        success          = true,
        narration        = narration,
        game_over        = game_over or false,
        game_over_reason = reason or "",
    }
end

-- ============================================================
-- SAVE / RESTORE
-- ============================================================

-- Serialize state + agent snapshots.
function M.snapshot()
    local s = _state
    if not s then return json.encode({}) end
    local snap = {}
    for k, v in pairs(s) do snap[k] = v end
    snap._agents        = M.snapshot_agents()
    snap._pending_events = nil   -- transient: consumed each turn, not persisted
    return json.encode(snap)
end

-- Deserialize snapshot. Returns (state_table, result_table).
-- init_agents_fn: called with _state already set to restored data — use adv.get_state() inside it.
function M.restore(snapshot_str, init_agents_fn)
    local ok, data = pcall(json.decode, snapshot_str)
    if not ok then
        return nil, { success=false, error="Snapshot non valido: " .. tostring(data) }
    end
    local agent_data = data._agents
    data._agents = nil
    -- Session-isolated persona: if the save recorded a persona path, switch to
    -- it before reloading NPC files so we load the correct session folder.
    if data._persona_path then
        local okp, p = pcall(require, "lib/persona")
        if okp and type(p) == "table" and p.use_path then
            p.use_path(data._persona_path)
            if p.reload_all then p.reload_all() end
        end
    end
    _state = data  -- set BEFORE init_agents_fn so it can read player.name via adv.get_state()
    if init_agents_fn then init_agents_fn() end
    -- Save contains agent snapshots but no agents exist: without this warning
    -- the histories would be dropped silently (e.g. load-save right after
    -- startup with no init_agents_fn passed, or stale agents from a previous script).
    if agent_data and next(agent_data) and not next(_agents) then
        wlog.warn("adventure", "snapshot has agent histories but no agents initialized"
            .. " — pass init_agents_fn to adv.restore() or call adv.init_agents() first.")
    end
    M.restore_agents(agent_data)
    -- Post-undo reconciliation: the save rewinds but file-backed data
    -- (persona life events, world event logs) does not. Prune entries
    -- stamped with a turn later than the restored one.
    local turn = tonumber(data.turn)
    if turn then
        local w = _world()
        if w and w.prune_future_events then
            local n = w.prune_future_events(turn)
            if n > 0 then
                wlog.warn("adventure", "pruned " .. n .. " world events after turn " .. turn)
            end
        end
        local okp, p = pcall(require, "lib/persona")
        if okp and type(p) == "table" and p.prune_future_events then
            local n = p.prune_future_events(turn)
            if n > 0 then
                wlog.warn("adventure", "pruned " .. n .. " persona entries after turn " .. turn)
            end
        end
    end
    return data, { success=true }
end

-- ============================================================
-- WORLD LINTER
-- ============================================================

-- Mechanical validation of the adventure's static data + live state.
-- Returns { errors={...}, warnings={...} }.
-- errors   = hard breaks (dangling travel_map edges, unknown locations)
-- warnings = likely mistakes (one-way passages, unplaced NPCs, routine gaps)
-- Run from /validate, after script load, or from CoderAI after edits.
function M.validate(npc_data, locations, travel_map)
    if npc_data   then _npc_data   = npc_data   end
    if locations  then _locations  = locations  end
    if travel_map then _travel_map = travel_map end
    npc_data   = _npc_data   or {}
    travel_map = _travel_map or {}

    local errors, warnings = {}, {}
    local function err(m)  table.insert(errors, m)   end
    local function warn(m) table.insert(warnings, m) end

    -- travel_map integrity
    for src, exits in pairs(travel_map) do
        if not _loc_exists(src) then
            err("travel_map: source '" .. src .. "' is not a known location")
        end
        local seen = {}
        for _, dst in ipairs(exits or {}) do
            if dst == src then warn("travel_map: '" .. src .. "' has a self-edge") end
            if seen[dst] then
                warn("travel_map: duplicate exit '" .. dst .. "' from '" .. src .. "'")
            end
            seen[dst] = true
            if not _loc_exists(dst) then
                err("travel_map: '" .. src .. "' → '" .. dst
                    .. "' — destination is not a known location")
            else
                local back = false
                for _, e in ipairs(travel_map[dst] or {}) do
                    if e == src then back = true; break end
                end
                if not back then
                    warn("travel_map: '" .. src .. "' → '" .. dst
                        .. "' is one-way (no return edge)")
                end
            end
        end
    end

    -- Connectivity: everything reachable from the player's location
    local s = _state
    local start = s and s.player and s.player.location
    if start and start ~= "" and _loc_exists(start) then
        local visited = { [start] = true }
        local q, head = { start }, 1
        while head <= #q do
            local n = q[head]; head = head + 1
            for _, nb in ipairs(_neighbors(n)) do
                if not visited[nb] then visited[nb] = true; table.insert(q, nb) end
            end
        end
        local unreachable = {}
        _each_location(function(id)
            if not visited[id] then table.insert(unreachable, id) end
        end)
        table.sort(unreachable)
        if #unreachable > 0 then
            warn("unreachable from '" .. start .. "': "
                .. table.concat(unreachable, ", "))
        end
    end

    -- NPC placement
    if s then
        for id, loc in pairs(s.npc_locations or {}) do
            if not npc_data[id] then
                warn("npc_locations: '" .. id .. "' has no NPC_DATA entry")
            end
            if loc and loc ~= "" and loc ~= "fuori" and not _loc_exists(loc) then
                warn("npc_locations: '" .. id .. "' is at unknown location '" .. loc .. "'")
            end
        end
        for id in pairs(npc_data) do
            if not (s.npc_locations and s.npc_locations[id]) then
                warn("NPC '" .. id .. "' has no entry in npc_locations (never placed)")
            end
        end
    end

    -- Agents vs NPC_DATA
    if _cfg.use_agents and next(_agents) then
        for id in pairs(_agents) do
            if not npc_data[id] then
                warn("agent '" .. id .. "' has no NPC_DATA entry")
            end
        end
        for id in pairs(npc_data) do
            if not _agents[id] then
                warn("NPC '" .. id .. "' has no agent (think_as_npc will fall back)")
            end
        end
    end

    -- Persona routine coverage (only if persona is in use)
    local okp, persona = pcall(require, "lib/persona")
    if okp and type(persona) == "table"
       and persona.known_ids and persona.validate_routine then
        for _, id in ipairs(persona.known_ids()) do
            for _, msg in ipairs(persona.validate_routine(id)) do
                warn("routine '" .. id .. "': " .. msg)
            end
        end
    end

    return { errors = errors, warnings = warnings }
end

-- Human-readable validation report.
function M.format_validation(report)
    local lines = { "=== VALIDAZIONE MONDO ===" }
    if #report.errors == 0 and #report.warnings == 0 then
        table.insert(lines, "  Nessun problema rilevato.")
    end
    for _, e in ipairs(report.errors)   do table.insert(lines, "  [ERRORE]  " .. e) end
    for _, w in ipairs(report.warnings) do table.insert(lines, "  [warning] " .. w) end
    return table.concat(lines, "\n")
end

-- ============================================================
-- STANDARD PLAYER COMMANDS
-- ============================================================

-- Handles /map, /time, /npcs, /inv, /notes. Returns result table or nil if not handled.
function M.handle_input(input, npc_data, locations, travel_map)
    local s = _state
    local trimmed = (input:match("^%s*(.-)%s*$") or input)
    local cmd, rest = trimmed:match("^(/[%w_]+)%s*(.*)")
    rest = rest or ""

    if cmd == "/map" or cmd == "/exits" then
        if travel_map then _travel_map = travel_map end
        if locations  then _locations  = locations  end
        local loc_id = s and s.player.location or ""
        local exits  = _neighbors(loc_id)
        local cur    = _loc_info(loc_id)
        local lines  = {
            "Posizione: " .. (cur and cur.name or loc_id) .. "  [" .. loc_id .. "]",
            "Uscite:"
        }
        for _, id in ipairs(exits) do
            local dest = _loc_info(id)
            table.insert(lines, "  • " .. (dest and dest.name or id) .. "  [" .. id .. "]")
        end
        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end

    if cmd == "/time" then
        if not (s and s.time) then
            return { success=true, handled=true, output="Tempo non attivo." }
        end
        return { success=true, handled=true,
            output=string.format("%s, %s G%d — turno %d",
                s.time, s.day or "", s.giorno_index or 1, s.turn) }
    end

    if cmd == "/npcs" then
        if not s then return { success=true, handled=true, output="Stato non inizializzato." } end
        local entries = {}
        for id, loc_id in pairs(s.npc_locations or {}) do
            local npc = npc_data and npc_data[id]
            local loc = locations and locations[loc_id]
            local act = (s.npc_activities or {})[id]
            table.insert(entries, string.format("  %-22s → %s [%s]%s",
                npc and npc.name or id,
                loc and loc.name or loc_id, loc_id,
                act and (" | " .. act) or ""))
        end
        -- Generated NPCs (separate field, see M.display_state comment)
        local p = _persona()
        for id, loc_id in pairs(s.gen_npc_locations or {}) do
            local d   = p and p.get and p.get(id)
            local loc = locations and locations[loc_id]
            table.insert(entries, string.format("  %-22s → %s [%s]  (generato)",
                (d and d.name) or id,
                loc and loc.name or loc_id, loc_id))
        end
        table.sort(entries)  -- sort entries only, keep the header first
        local lines = { "=== POSIZIONI NPC ===" }
        for _, l in ipairs(entries) do table.insert(lines, l) end
        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end

    if cmd == "/inv" or cmd == "/inventario" then
        if not (s and s.inventario) then
            return { success=true, handled=true, output="Inventario non attivo." }
        end
        local inv = s.inventario or {}
        return { success=true, handled=true, output=string.format(
            "Inventario: %s\nContanti: €%d",
            #inv > 0 and table.concat(inv, ", ") or "(vuoto)",
            s.soldi or 0) }
    end

    if cmd == "/notes" or cmd == "/note" then
        if not (s and s.notes) or #s.notes == 0 then
            return { success=true, handled=true, output="Nessuna nota." }
        end
        local lines = { "=== NOTE ===" }
        for _, n in ipairs(s.notes) do
            if type(n) == "string" then
                table.insert(lines, "  • " .. n)
            else
                table.insert(lines, string.format("  [%s|%s] %s",
                    n.date or "?", n.scope or "player", n.content or ""))
            end
        end
        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end

    if cmd == "/sim" then
        local steps = tonumber(rest:match("^(%d+)")) or 4
        return M._run_sim(steps, npc_data, locations)
    end

    if cmd == "/events" then
        if #_events_order == 0 then
            return { success=true, handled=true, output="Nessun evento registrato." }
        end
        local s = _state
        local fired = s and s._events_fired or {}
        local lines = { "=== EVENTI AVVENTURA ===" }
        for _, id in ipairs(_events_order) do
            local ev   = _events_registry[id]
            local last = fired[id]
            local status = last and ("✓ T" .. (type(last)=="table" and last.turn or last)) or "○"
            local when_s = ev.when and (ev.when.from .. "-" .. ev.when.to) or "qualsiasi ora"
            local day_s  = ev.day  and tostring(ev.day)  or "qualsiasi giorno"
            local rep    = ev.once == false and ("ripetibile cooldown=" .. (ev.cooldown or 0) .. "m") or "una volta"
            table.insert(lines, string.format("  [%-5s] %-28s %s | giorno:%s | %s",
                status, ev.label or id, when_s, day_s, rep))
        end
        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end

    if cmd == "/validate" then
        if travel_map then _travel_map = travel_map end
        if locations  then _locations  = locations  end
        if npc_data   then _npc_data   = npc_data   end
        local report = M.validate()
        return { success=true, handled=true, output=M.format_validation(report) }
    end

    if cmd == "/debug" then
        local s = _state
        if not s then return { success=true, handled=true, output="Stato non inizializzato." } end
        local lines = { "=== DEBUG ===" }

        -- Protagonista
        table.insert(lines, string.format("  Protagonista: %s @ %s | turno %d",
            s.player.name or "?", s.player.location or "?", s.turn or 0))

        -- Tempo
        if s.time then
            table.insert(lines, string.format("  Tempo: %s %s G%d",
                s.time, s.day or "", s.giorno_index or 1))
        end

        -- NPC locations + activities
        if next(s.npc_locations or {}) then
            local npc_lines = {}
            for id, loc in pairs(s.npc_locations) do
                local act = (s.npc_activities or {})[id]
                table.insert(npc_lines, string.format("    %-22s → %s%s",
                    id, loc, act and (" | " .. act) or ""))
            end
            table.sort(npc_lines)
            table.insert(lines, "  NPC:")
            for _, l in ipairs(npc_lines) do table.insert(lines, l) end
        end

        -- Inventario
        if s.inventario then
            local inv = s.inventario or {}
            table.insert(lines, string.format("  Inventario: %s | Soldi: €%d",
                #inv > 0 and table.concat(inv, ", ") or "(vuoto)", s.soldi or 0))
        end

        -- Note
        if s.notes then
            local by_scope = { player=0, public=0, npc=0 }
            for _, n in ipairs(s.notes) do
                if type(n) == "table" then
                    local sc = n.scope or "player"
                    if sc:sub(1, 3) == "npc" then by_scope.npc = by_scope.npc + 1
                    elseif sc == "public" then by_scope.public = by_scope.public + 1
                    else by_scope.player = by_scope.player + 1 end
                end
            end
            table.insert(lines, string.format("  Note: %d (player:%d public:%d npc:%d)",
                #s.notes, by_scope.player, by_scope.public, by_scope.npc))
        end

        -- Agenti
        local agent_ids = {}
        for id in pairs(_agents) do table.insert(agent_ids, id) end
        table.sort(agent_ids)
        if #agent_ids > 0 then
            table.insert(lines, "  Agenti: " .. table.concat(agent_ids, ", "))
        end

        -- Tool calls questo turno (se ne rimangono)
        local active_calls = {}
        for k, v in pairs(_tool_calls) do
            if v and k:sub(-7) ~= "_result" then
                table.insert(active_calls, k)
            end
        end
        if #active_calls > 0 then
            table.sort(active_calls)
            table.insert(lines, "  Tool chiamati: " .. table.concat(active_calls, ", "))
        end

        -- Debug tool log (se attivo)
        if _debug_log and #_dbg_tools > 0 then
            table.insert(lines, string.format("  Tool log: %d voci", #_dbg_tools))
            for i = math.max(1, #_dbg_tools - 4), #_dbg_tools do
                local t = _dbg_tools[i]
                table.insert(lines, string.format("    [%s] %s",
                    t.name or "?",
                    (t.result or ""):sub(1, 80)))
            end
        end

        -- Warning recenti dalle lib (log.lua) — in web mode la console è invisibile
        if wlog.count() > 0 then
            local recent = wlog.recent(5)
            table.insert(lines, string.format("  Warning: %d totali (ultimi %d)",
                wlog.count(), #recent))
            for _, w in ipairs(recent) do
                table.insert(lines, "    " .. w)
            end
        end

        -- Hook avventura-specifica
        if _debug_fn then
            local extra = _debug_fn()
            if extra and extra ~= "" then
                table.insert(lines, "  ── avventura ─────────────────────────────")
                table.insert(lines, extra)
            end
        end

        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end

    -- /setloc <location_id> [npc_id]   DEBUG: teleport player or NPC
    if cmd == "/setloc" then
        if not s then return { success=true, handled=true, output="Stato non inizializzato." } end
        local loc_id, npc_id = rest:match("^(%S+)%s*(%S*)$")
        if not loc_id or loc_id == "" then
            return { success=true, handled=true,
                output="Uso: /setloc <location_id> [npc_id]\nSenza npc_id sposta il player." }
        end
        npc_id = (npc_id ~= "" and npc_id) or nil
        if _loc_exists and not _loc_exists(loc_id) then
            -- gentle: list known locations
            local known = {}
            if _locations then for id in pairs(_locations) do table.insert(known, id) end end
            table.sort(known)
            return { success=true, handled=true,
                output="Location sconosciuta: " .. loc_id
                    .. (#known > 0 and ("\nNote: " .. table.concat(known, ", ")) or "") }
        end
        local loc_name = (_loc_info and _loc_info(loc_id) and _loc_info(loc_id).name) or loc_id
        if npc_id then
            if not s.npc_locations then s.npc_locations = {} end
            s.npc_locations[npc_id] = loc_id
            return { success=true, handled=true,
                output=string.format("NPC '%s' spostato in: %s [%s]", npc_id, loc_name, loc_id) }
        else
            s.player.location = loc_id
            return { success=true, handled=true,
                output=string.format("Player spostato in: %s [%s]", loc_name, loc_id) }
        end
    end

    return nil
end

-- ============================================================
-- /sim — headless NPC simulation (no LLM calls, no player input)
-- ============================================================
-- Runs N tick steps advancing the clock, showing NPC movements and events.
-- LLM beats are skipped (situation preview logged instead).
-- State IS modified (time + NPC positions advance). Reload save to reset.
--
-- Usage: /sim 8   → simulate 8 × tick_minutes (default 30 min) = 4 h
function M._run_sim(steps, npc_data, locations)
    local s = _state
    if not s then
        return { success=true, handled=true, output="[sim] Stato non inizializzato." }
    end
    if not _tick_fn then
        return { success=true, handled=true, output="[sim] tick_fn non registrato (set_tick_fn mancante)." }
    end

    local tmin     = (_cfg and _cfg.tick_minutes) or 30
    local buf      = {}
    local step_num = 0

    -- Enable agent sim mode (skips LLM, captures entries)
    local ok_ag, ag = pcall(require, "lib/agent")
    if ok_ag then
        ag._sim_mode        = true
        ag._sim_log_entries = {}
    end

    -- Wrap tick_fn to snapshot positions before each step and diff after.
    -- Use _tick_fn_raw (unwrapped) as base if gamelog has already wrapped _tick_fn,
    -- so sim entries are not double-logged.
    local orig_tick = _tick_fn
    local base_tick = _tick_fn_raw or _tick_fn
    _tick_fn = function(time_str, day_str, gidx, chunk)
        step_num = step_num + 1

        -- snapshot positions before tick
        local before = {}
        for k, v in pairs(s.npc_locations or {}) do before[k] = v end

        -- clear sim capture buffer for this step
        if ok_ag then ag._sim_log_entries = {} end

        local ok_t, terr = pcall(base_tick, time_str, day_str, gidx, chunk)
        if not ok_t then
            buf[#buf + 1] = string.format("[%s] !! tick error: %s", time_str, tostring(terr))
            return
        end

        -- collect activities/events from sim buffer
        local step_lines = {}

        if ok_ag and ag._sim_log_entries then
            for _, e in ipairs(ag._sim_log_entries) do
                local tag = e.kind == "llm_dry" and "[LLM]"
                         or e.kind == "event"   and "[EVT]"
                         or ""
                local txt = e.text or ""
                -- trim long situation previews
                if txt:len() > 70 then txt = txt:sub(1, 70) .. "…" end
                step_lines[#step_lines + 1] = string.format("  %-12s%s %s",
                    e.npc or "?", tag, txt)
            end
        end

        -- detect location changes
        for id, loc in pairs(s.npc_locations or {}) do
            if before[id] ~= loc then
                local nname = (npc_data and npc_data[id] and npc_data[id].name) or id
                local lname = (locations and locations[loc] and locations[loc].name) or loc
                local prev  = (locations and before[id] and
                               locations[before[id]] and locations[before[id]].name)
                              or before[id] or "?"
                step_lines[#step_lines + 1] = string.format("  %-12s→ %s (era: %s)",
                    "MOV:" .. id, lname, prev)
            end
        end

        if #step_lines == 0 then
            buf[#buf + 1] = string.format("[%s] —", time_str)
        else
            buf[#buf + 1] = string.format("[%s]", time_str)
            for _, l in ipairs(step_lines) do buf[#buf + 1] = l end
        end
    end

    M.advance_clock(steps * tmin)

    -- Restore tick_fn and agent flags
    _tick_fn = orig_tick
    if ok_ag then
        ag._sim_mode        = false
        ag._sim_log_entries = nil
    end

    local header = string.format(
        "=== /sim %d step ×%d min (ora: %s %s) ===",
        steps, tmin, s.time or "?", s.day or "")

    -- Write sim output to gamelog so CoderAI can analyze NPC behavior
    if _gamelog then
        _gamelog.separator("SIM " .. steps .. " steps → " .. (s.time or "?"))
        for _, line in ipairs(buf) do _gamelog.write("SIM", line) end
    end

    if #buf == 0 then
        return { success=true, handled=true, output=header .. "\n(nessun evento)" }
    end
    return { success=true, handled=true,
             output=header .. "\n" .. table.concat(buf, "\n") }
end

-- ============================================================
-- TOOL CALL GUARD HELPERS (for adventure-specific tools)
-- ============================================================

function M.tool_called(key)  _turn_guard(); return _tool_calls[key]                        end
function M.mark_tool(key)    _turn_guard(); _tool_calls[key] = true                        end
function M.count_tool(key)   _turn_guard(); return _tool_calls[key] or 0                   end
function M.inc_tool(key)     _turn_guard(); _tool_calls[key] = (_tool_calls[key] or 0) + 1 end

-- ============================================================
-- LOCATION SUGGESTION (no LLM)
-- ============================================================

-- Returns up to `max` plausible location IDs for an invalid attempt.
-- Scoring: word overlap (ID + name) + adjacency boost + same-zone boost.
-- Fallback chain: word matches → adjacent → same zone → all.
-- Score every known location against an attempted id. Returns a sorted array of
-- { id, s, name, desc, overlap } — `overlap` is the word-overlap component only
-- (id+name match), so a caller can tell a likely typo (overlap>0 → "did you
-- mean X") from a merely-adjacent suggestion (s from adjacency, overlap==0).
local function _score_locations(attempted, npc_current_loc)
    local scores = {}

    -- Tokens shorter than 2 chars are noise (floor markers p1/p2 → "p") and
    -- cause false "did you mean" matches across unrelated ids — skip them.
    local attempt_words = {}
    for w in attempted:lower():gmatch("[a-zA-Z]+") do
        if #w >= 2 then attempt_words[w] = true end
    end

    local adj_set = {}
    if npc_current_loc then
        for _, lid in ipairs(_neighbors(npc_current_loc)) do adj_set[lid] = true end
    end

    _each_location(function(loc_id, loc_data)
        local overlap = 0
        for w in loc_id:lower():gmatch("[a-zA-Z]+") do
            if attempt_words[w] then overlap = overlap + 2 end
        end
        if loc_data.name then
            for w in loc_data.name:lower():gmatch("[a-zA-Z]+") do
                if attempt_words[w] then overlap = overlap + 1 end
            end
        end
        local s = overlap + (adj_set[loc_id] and 3 or 0)
        -- Same zone removed: caused adjacent-but-wrong locations to outrank semantic matches
        table.insert(scores, { id=loc_id, s=s, overlap=overlap,
                               name=loc_data.name, desc=loc_data.description })
    end)

    table.sort(scores, function(a, b) return a.s > b.s end)
    return scores
end

local function _suggest_locations(attempted, npc_current_loc, locations, travel_map, max)
    max = max or 4
    local scores = _score_locations(attempted, npc_current_loc)
    local result = {}
    for i = 1, math.min(max, #scores) do
        table.insert(result, scores[i].id)
    end
    return result
end

-- ============================================================
-- STANDARD TOOL LIST
-- ============================================================

-- Returns the list of standard tools based on CFG.
-- memory_lib: the memory module (if use_memory=true).
-- extra: array of adventure-specific tool tables, appended at the end.
-- NPC_DATA, LOCATIONS, TRAVEL_MAP: stored at module level so closures always
-- read the latest values; the parameters here update the module state.
function M.get_tools(npc_data, locations, travel_map, memory_lib, extra)
    -- Store at module level so subsequent rebuild_npc_data calls are visible
    -- to the already-built closures (get_tools is called once per session).
    if npc_data   then _npc_data   = npc_data   end
    if locations  then _locations  = locations  end
    if travel_map then _travel_map = travel_map end
    local t = {}
    local cfg = _cfg

    -- ── think_as_npc ─────────────────────────────────────────────────────
    if cfg.use_agents then
        table.insert(t, {
            name = "think_as_npc",
            description = "Chiedi a un NPC come reagisce alla situazione. "
                       .. "Solo se è nella stessa location del protagonista. "
                       .. "Cached/turno — stessa id = stesso risultato. "
                       .. "Ritorna {intent, speech}. "
                       .. "'intent'=pensiero interno, TERZA PERSONA, MAI virgolette. "
                       .. "'speech'=parole esatte, VERBATIM tra «». Vuoto = silenzio.",
            params = [[{
                "type": "object", "required": ["id", "situation"],
                "properties": {
                    "id":        { "type": "string", "description": "NPC id" },
                    "situation": { "type": "string", "description": "Fatti osservabili. Neutro — NON descrivere la risposta attesa." }
                }
            }]],
            fn = function(args_json)
                _turn_guard()
                local a      = json.decode(args_json)
                local npc_id = a.id or ""
                local cache  = "think_as_npc_" .. npc_id
                if _tool_calls[cache .. "_result"] then
                    return _tool_calls[cache .. "_result"]
                end
                -- Lazy agent creation: a generated NPC may exist in persona (its
                -- .lua file is written) but its agent not be registered in _agents
                -- (wrapper race, restore gap, etc.). Before failing, try to build
                -- the agent on the fly from the persona file. Makes think_as_npc
                -- work for ANY persona-backed NPC, regardless of how it got there.
                if not _agents[npc_id] then
                    local okp, p = pcall(require, "lib/persona")
                    if okp and type(p) == "table" and p.get and p.agent_object
                       and p.get(npc_id) then
                        local ag = p.agent_object(npc_id, {})
                        if ag then M.add_agent(npc_id, ag) end
                    end
                end
                if not _agents[npc_id] then
                    local valid = {}
                    for k in pairs(_agents) do table.insert(valid, k) end
                    table.sort(valid)
                    -- Decision-tree, like move_player: existing ids first, create
                    -- as gated last resort with the LITERAL id (avoids the blind
                    -- generate_npc({}) loop seen in tests).
                    local msg = "Nessun agente per id '" .. npc_id .. "'. "
                        .. "ID personaggi esistenti: " .. table.concat(valid, ", ") .. "."
                    local okp, p = pcall(require, "lib/persona")
                    if okp and type(p) == "table" and p.generate then
                        msg = msg .. " SOLO se è una PERSONA NUOVA mai incontrata, creala con "
                            .. "QUESTI argomenti esatti: generate_npc(id=\"" .. npc_id .. "\", "
                            .. "context=\"<chi è: ruolo, età, dove vive, rapporto col mondo, "
                            .. "tratti>\"), POI richiama think_as_npc(\"" .. npc_id .. "\", ...)."
                    end
                    return json.encode({ error = msg })
                end
                -- Validate same location
                local s = _state
                if s then
                    local player_loc = s.player and s.player.location
                    local npc_loc    = (s.npc_locations and s.npc_locations[npc_id])
                                    or (s.gen_npc_locations and s.gen_npc_locations[npc_id])
                    if player_loc and npc_loc and npc_loc ~= player_loc then
                        return json.encode({
                            error = npc_id .. " è in " .. npc_loc
                                .. ", tu sei in " .. player_loc
                                .. " — think_as_npc solo se NPC nella stessa location. "
                                .. "Usa move_npc per avvicinarlo o move_player per raggiungerlo.",
                            npc_location    = npc_loc,
                            player_location = player_loc,
                        })
                    end
                end
                _tool_calls[cache] = true
                local s = _state
                local situation = a.situation or ""
                local shared = {}
                for _, n in ipairs(s and s.notes or {}) do
                    if type(n) == "table" then
                        -- Witness gate: a "public" note only reaches THIS npc if
                        -- she was actually present when it was written (n.witnesses,
                        -- set by the remember tool). A legacy note with no
                        -- witnesses field (written before this fix, or hand-authored)
                        -- reaches nobody directly here — it's not lost, the
                        -- narrator's own prompt (adv.prompt_notes) still sees ALL
                        -- public notes unconditionally, only per-NPC injection is gated.
                        local is_witnessed_public = n.scope == "public"
                            and type(n.witnesses) == "table" and n.witnesses[npc_id]
                        if n.scope == ("npc:" .. npc_id) or is_witnessed_public then
                            table.insert(shared, n.content or "")
                        end
                    end
                end
                if #shared > 0 then
                    situation = situation .. "\n[Notizie recenti: " .. table.concat(shared, "; ") .. "]"
                end
                -- Auto-inject current outfit (persona routine / override) so
                -- the agent knows what it is wearing right now.
                if s and s.time then
                    local okp, p = pcall(require, "lib/persona")
                    if okp and type(p) == "table" and p.current_outfit then
                        local outfit = p.current_outfit(npc_id, s.time, s.day)
                        if outfit then
                            situation = situation .. "\n[Stai indossando: " .. outfit .. "]"
                        end
                    end
                end
                local result = _agents[npc_id]:decide(situation, M.NPC_THINK_SCHEMA)
                _tool_calls[cache .. "_result"] = result
                _log("think_as_npc", { id=npc_id }, result)
                return result
            end,
        })
    end

    -- ── advance_time ──────────────────────────────────────────────────────
    if cfg.use_time then
        table.insert(t, {
            name = "advance_time",
            description = "Avanza il tempo. PRIMA di azioni che richiedono tempo. MAX 1/turno.",
            params = [[{
                "type": "object", "required": ["minutes"],
                "properties": { "minutes": { "type": "integer", "minimum": 1, "maximum": 480 } }
            }]],
            fn = function(args_json)
                _turn_guard()
                if _tool_calls["advance_time"] then
                    return json.encode({ error="advance_time già chiamato questo turno." })
                end
                _tool_calls["advance_time"] = true
                local a = json.decode(args_json)
                local mins = math.max(1, math.min(480, tonumber(a.minutes) or 30))
                local s = _state
                -- Stepped advance: the tick hook (set_tick_fn) simulates the
                -- world for every step, so NPCs live through the elapsed time
                -- instead of teleporting to the final slot.
                local ticks = M.advance_clock(mins)
                _log("advance_time", a, s and s.time or "?")
                return json.encode({ ok=true, time=s and s.time, day=s and s.day,
                                     ticks=ticks })
            end,
        })

        -- ── sleep_until ────────────────────────────────────────────────────
        -- Sleeping/waiting jumps to a TARGET time, not a duration. advance_time
        -- captures "how long an action takes" and the model both miscomputes the
        -- minutes-to-5am and can't exceed its per-turn cap — so it narrates
        -- waking at 05:00 while the clock stays at 00:10. This tool takes the
        -- target wake time and advances the clock to it (wrapping past midnight),
        -- running the same off-screen simulation as advance_time.
        table.insert(t, {
            name = "sleep_until",
            description = "Dormi o aspetta FINO a un orario. Usa quando il giocatore "
                .. "va a dormire o salta a un momento futuro (NON advance_time). "
                .. "Indica l'orario di RISVEGLIO 'HH:MM'; se è <= ora attuale è il "
                .. "giorno dopo. Esempio: sleep_until(time=\"05:00\"). MAX 1/turno.",
            params = [[{
                "type": "object", "required": ["time"],
                "properties": { "time": { "type": "string",
                    "description": "Orario di risveglio HH:MM (24h)." } }
            }]],
            fn = function(args_json)
                _turn_guard()
                if _tool_calls["advance_time"] then
                    return json.encode({ error="tempo già avanzato questo turno." })
                end
                local a = json.decode(args_json)
                local s = _state
                local th, tm = tostring(a.time or ""):match("(%d+):(%d+)")
                if not th or not (s and s.time) then
                    return json.encode({ error="time deve essere 'HH:MM'." })
                end
                local ch, cm = s.time:match("(%d+):(%d+)")
                local now    = tonumber(ch) * 60 + tonumber(cm)
                local target = tonumber(th) * 60 + tonumber(tm)
                local mins   = target - now
                if mins <= 0 then mins = mins + 1440 end   -- wrap to next day
                -- Safety: a sleep longer than 16h is almost certainly a mistake.
                if mins > 960 then mins = 960 end
                -- Did this sleep cross the deep-sleep window (01:00-05:00)? If so
                -- flag it so after_ai_turn can run the whole cast's dreams. Walk
                -- the interval [now, now+mins] in mod-1440 space.
                local crossed = false
                for t = now, now + mins do
                    local hh = math.floor((t % 1440) / 60)
                    if hh >= 1 and hh < 5 then crossed = true; break end
                end
                _tool_calls["advance_time"] = true   -- block a second advance this turn
                local ticks = M.advance_clock(mins)
                if crossed then s._dream_due_day = s.giorno_index or 1 end
                _log("sleep_until", a, s.time)
                return json.encode({ ok=true, time=s.time, day=s.day,
                                     minutes=mins, ticks=ticks, dreams_due=crossed })
            end,
        })
    end

    -- ── move_player ───────────────────────────────────────────────────────
    table.insert(t, {
        name = "move_player",
        description = "Sposta il protagonista in una nuova location. "
                   .. "PRIMA di narrare. SOLO su movimento esplicito. NON se già lì. MAX 1/turno. "
                   .. "Se ritorna errore: il protagonista NON si è mosso — "
                   .. "SCRIVI LA NARRAZIONE ORA restando nella location attuale.",
        params = [[{
            "type": "object", "required": ["location"],
            "properties": { "location": { "type": "string" } }
        }]],
        fn = function(args_json)
            _turn_guard()
            if _tool_calls["move_player"] then
                return json.encode({
                    error = "move_player già chiamato questo turno — SCRIVI LA NARRAZIONE ORA.",
                    current_location = _state and _state.player.location or "?"
                })
            end
            local a   = json.decode(args_json)
            local loc = a.location or ""
            local s   = _state

            -- No-op: already at destination — return ok without consuming cap
            if s and s.player.location == loc then
                local dest = _loc_info(loc)
                return json.encode({ ok=true, location=loc,
                    name=dest and dest.name or loc, already_here=true })
            end

            if _locations and not _loc_exists(loc) then
                local cur     = s and s.player.location or "?"
                local scored  = _score_locations(loc, cur)
                -- Graduated, decision-tree order: resolve to an existing place FIRST
                -- (strong name match, then nearby list), create as the gated last
                -- resort. Ordering matters for the LLM — the escape hatch goes last
                -- and stays explicitly conditional so typos don't spawn junk rooms.
                local suggestions = {}
                for i = 1, math.min(4, #scored) do suggestions[i] = scored[i].id end

                local parts = { "La location '" .. loc .. "' non esiste." }

                -- 1) Strong match: an existing place whose name/id overlaps → likely meant.
                local best = scored[1]
                if best and best.overlap > 0 then
                    local d = best.desc and (" — " .. best.desc) or ""
                    parts[#parts+1] = "Forse intendevi: " .. (best.name or best.id)
                        .. " [" .. best.id .. "]" .. d
                        .. ". Se sì: move_player('" .. best.id .. "')."
                end

                -- 2) Nearby existing places (ids only — descrizioni troppo lunghe per tutte).
                if #suggestions > 0 then
                    parts[#parts+1] = "Altre location esistenti vicine: "
                        .. table.concat(suggestions, ", ") .. "."
                end

                -- 3) Gated last resort: create a genuinely new place (procedural world
                --    only). Embed the LITERAL id so the LLM copies it instead of
                --    calling generate_location({}) with no args (observed failure mode).
                if _world() then
                    -- Remember the intended id so a blind generate_location({}) recovers it.
                    if _world().note_missing_location then _world().note_missing_location(loc) end
                    parts[#parts+1] = "SOLO se nessuna di queste è il posto giusto ed è un "
                        .. "luogo NUOVO mai visto, crea con QUESTI argomenti esatti: "
                        .. "generate_location(id=\"" .. loc .. "\", "
                        .. "context=\"<cosa è: tipo, atmosfera, stanze adiacenti>\", "
                        .. "from=\"" .. cur .. "\"), POI move_player(\"" .. loc .. "\")."
                else
                    parts[#parts+1] = "Se nessuna è quella giusta, SCRIVI LA NARRAZIONE "
                        .. "restando in " .. cur .. "."
                end

                return json.encode({
                    error = table.concat(parts, " "),
                    current_location = cur,
                    suggested_locations = suggestions,
                    did_you_mean = (best and best.overlap > 0) and best.id or nil,
                })
            end

            if (_travel_map or _world()) and s then
                local cur   = s.player.location or ""
                local exits = _neighbors(cur)
                local valid = false
                for _, e in ipairs(exits) do if e == loc then valid=true; break end end

                if not valid then
                    -- Full BFS pathfinding (unlimited depth, merged graph)
                    local visited = { [cur] = true }
                    local parent  = {}
                    local queue   = { cur }
                    local head    = 1
                    while head <= #queue do
                        local node = queue[head]; head = head + 1
                        for _, nb in ipairs(_neighbors(node)) do
                            if not visited[nb] then
                                visited[nb] = true
                                parent[nb]  = node
                                table.insert(queue, nb)
                                if nb == loc then
                                    head = #queue + 1  -- found — stop BFS
                                    break
                                end
                            end
                        end
                    end

                    if visited[loc] and loc ~= cur then
                        -- Reconstruct path cur → … → loc
                        local path_ids = { loc }
                        local node = loc
                        while parent[node] do
                            node = parent[node]
                            table.insert(path_ids, 1, node)
                        end
                        -- path_ids[1] == cur, drop it for display
                        local path_names = {}
                        for _, n in ipairs(path_ids) do
                            local ln = _loc_info(n)
                            table.insert(path_names, ln and ln.name or n)
                        end
                        _tool_calls["move_player"] = true
                        s.player.location = loc
                        local dest = _loc_info(loc)
                        local path_str = table.concat(path_ids, " → ")
                        _log("move_player", a, path_str)
                        return json.encode({
                            ok   = true, location = loc,
                            name = dest and dest.name or loc,
                            path = path_str,
                            narrative_hint = "Percorso: " .. table.concat(path_names, " → ") .. ".",
                        })
                    else
                        local exit_names = {}
                        for _, e in ipairs(exits) do table.insert(exit_names, e) end
                        return json.encode({
                            error = "Non raggiungibile: " .. loc
                                .. ". Posizione attuale: " .. cur
                                .. " — scegli una delle uscite o usa gli id da SPOSTAMENTI DISPONIBILI.",
                            current_location = cur,
                            available_exits  = exit_names,
                        })
                    end
                end
            end

            _tool_calls["move_player"] = true
            if s then s.player.location = loc end
            local dest = _loc_info(loc)
            _log("move_player", a, loc)
            return json.encode({ ok=true, location=loc, name=dest and dest.name or loc })
        end,
    })

    -- ── move_npc ──────────────────────────────────────────────────────────
    table.insert(t, {
        name = "move_npc",
        description = "Sposta un NPC in una location ESISTENTE. PRIMA di narrare. MAX 1/NPC/turno. "
                   .. "Se ritorna errore: l'NPC NON si è mosso — usa la location attuale nella narrazione.",
        params = [[{
            "type": "object", "required": ["id", "location"],
            "properties": {
                "id":       { "type": "string" },
                "location": { "type": "string", "description": "ID esatto da LOCATIONS — NON inventare location." },
                "activity": { "type": "string", "description": "Opzionale: attività nella nuova location." }
            }
        }]],
        fn = function(args_json)
            _turn_guard()
            local a   = json.decode(args_json)
            local id  = a.id or ""
            local loc = a.location or ""
            local key = "move_npc_" .. id

            if _tool_calls[key] then
                return json.encode({ error="move_npc già chiamato per " .. id .. " questo turno." })
            end

            -- Validate destination location (static + generated)
            if _locations and not _loc_exists(loc) then
                local npc_cur = _state and (
                    (_state.npc_locations and _state.npc_locations[id]) or
                    (_state.gen_npc_locations and _state.gen_npc_locations[id]))
                local suggestions = _suggest_locations(loc, npc_cur, _locations, _travel_map, 4)
                return json.encode({
                    error = "Location '" .. loc .. "' non esiste. "
                        .. "Location suggerite: " .. table.concat(suggestions, ", ") .. ".",
                    suggested_locations = suggestions,
                })
            end

            -- Accept static NPC (in _npc_data), persona-generated NPC already
            -- placed (in gen_npc_locations), OR an existing persona file
            -- never placed anywhere yet — a real bug observed live: an NPC
            -- created via CoderAI's editor (persona file on disk, full
            -- backstory) is never registered into gen_npc_locations, since
            -- that only happens through the in-game generate_npc tool flow.
            -- Rejecting her here left the narrator no way to place the REAL
            -- character, so it generated a DUPLICATE one instead. Resolve to
            -- the existing persona first (same "existing beats invented"
            -- principle as everywhere else) and register her into
            -- gen_npc_locations lazily, right now, on first successful move.
            local gen_locs = _state and _state.gen_npc_locations
            local is_gen   = gen_locs and gen_locs[id] ~= nil
            local is_unplaced_persona = false
            if not is_gen and not (_npc_data and _npc_data[id]) then
                local p = _persona()
                is_unplaced_persona = p and p.get and p.get(id) ~= nil
            end
            if _npc_data and not _npc_data[id] and not is_gen and not is_unplaced_persona then
                local valid_npcs = {}
                for k in pairs(_npc_data) do table.insert(valid_npcs, k) end
                if gen_locs then for k in pairs(gen_locs) do table.insert(valid_npcs, k) end end
                table.sort(valid_npcs)
                return json.encode({ error="NPC non trovato: '" .. id .. "'. "
                    .. "ID validi: " .. table.concat(valid_npcs, ", ") .. "." })
            end

            _tool_calls[key] = true
            if _state then
                if is_gen or is_unplaced_persona then
                    _state.gen_npc_locations = _state.gen_npc_locations or {}
                    _state.gen_npc_locations[id] = loc
                else
                    _state.npc_locations[id] = loc
                end
                if a.activity and a.activity ~= "" then
                    _state.npc_activities     = _state.npc_activities or {}
                    _state.npc_activities[id] = a.activity
                end
            end
            _log("move_npc", a, "ok")
            return json.encode({ ok=true, id=id, location=loc })
        end,
    })

    -- ── set_activity ──────────────────────────────────────────────────────
    table.insert(t, {
        name = "set_activity",
        description = "Aggiorna attività NPC senza spostarlo. MAX 1/NPC/turno.",
        params = [[{
            "type": "object", "required": ["id", "activity"],
            "properties": {
                "id":       { "type": "string" },
                "activity": { "type": "string" }
            }
        }]],
        fn = function(args_json)
            _turn_guard()
            local a   = json.decode(args_json)
            local id  = a.id or ""
            local key = "set_activity_" .. id
            if _tool_calls[key] then
                return json.encode({ error="set_activity già chiamato per " .. id })
            end
            _tool_calls[key] = true
            if _state then
                _state.npc_activities     = _state.npc_activities or {}
                _state.npc_activities[id] = a.activity
            end
            _log("set_activity", a, "ok")
            return json.encode({ ok=true })
        end,
    })

    -- ── cambia_inventario ─────────────────────────────────────────────────
    if cfg.use_inventory then
        table.insert(t, {
            name = "cambia_inventario",
            description = "Modifica inventario e/o soldi. "
                       .. "SOLO su azione esplicita del giocatore (raccoglie, cede, compra, vende, spende). "
                       .. "ASSOLUTAMENTE VIETATO per costi narrativi impliciti.",
            params = [[{
                "type": "object",
                "properties": {
                    "aggiungi": { "type": "array", "items": { "type": "string" },
                                  "description": "Oggetti ricevuti o raccolti." },
                    "rimuovi":  { "type": "array", "items": { "type": "string" },
                                  "description": "Oggetti ceduti, usati o persi (nome esatto)." },
                    "soldi":    { "type": "integer",
                                  "description": "Variazione euro: positivo=guadagno, negativo=spesa." }
                }
            }]],
            fn = function(args_json)
                local a = json.decode(args_json)
                local s = _state
                if not s then return json.encode({ error="Stato non inizializzato." }) end
                s.inventario = s.inventario or {}
                s.soldi      = s.soldi      or 0
                if type(a.rimuovi) == "table" then
                    for _, item in ipairs(a.rimuovi) do
                        for i, ex in ipairs(s.inventario) do
                            if ex:lower() == item:lower() then
                                table.remove(s.inventario, i); break
                            end
                        end
                    end
                end
                if type(a.aggiungi) == "table" then
                    for _, item in ipairs(a.aggiungi) do
                        table.insert(s.inventario, item)
                    end
                end
                if type(a.soldi) == "number" then
                    s.soldi = s.soldi + a.soldi
                end
                _log("cambia_inventario", a, "ok")
                return json.encode({ ok=true, inventario=s.inventario, soldi=s.soldi })
            end,
        })
    end

    -- ── remember ──────────────────────────────────────────────────────────
    if cfg.use_notes then
        table.insert(t, {
            name = "remember",
            description = "Salva una nota per i turni futuri. MAX 2/turno. "
                       .. "scope 'player'=solo tu (DEFAULT — usa questo per qualsiasi cosa "
                       .. "privata, intima o accaduta in una stanza con poche persone, anche "
                       .. "se drammatica: es. un ricatto, un'umiliazione, una lite in casa "
                       .. "NON sono mai 'public' solo perché importanti); "
                       .. "'public'=SOLO fatti che l'intero mondo di gioco conoscerebbe "
                       .. "davvero all'istante (un annuncio in piazza, un incendio, una "
                       .. "notizia sui giornali) — è un'eccezione rara, non l'opzione di "
                       .. "default per eventi 'importanti'. Anche marcata 'public', la nota "
                       .. "raggiunge solo gli NPC che erano fisicamente presenti in questo "
                       .. "momento — per farla sapere a qualcun altro deve arrivarci parlando "
                       .. "o con eventi/gossip, non un remember. "
                       .. "'npc'=condiviso solo con un NPC specifico (specifica npc_id).",
            params = [[{
                "type": "object", "required": ["note"],
                "properties": {
                    "note":   { "type": "string", "description": "Fatto breve (max 20 parole)." },
                    "scope":  { "type": "string", "enum": ["player", "public", "npc"] },
                    "npc_id": { "type": "string", "description": "Se scope='npc', l'id dell'NPC." }
                }
            }]],
            fn = function(args_json)
                M.inc_tool("rem_count")
                if M.count_tool("rem_count") > 2 then
                    return json.encode({ error="remember: max 2/turno." })
                end
                local a    = json.decode(args_json)
                local note = (a.note or ""):match("^%s*(.-)%s*$")
                if note == "" then return json.encode({ error="nota vuota" }) end
                local scope = a.scope or "player"
                if scope == "npc" and a.npc_id and a.npc_id ~= "" then
                    scope = "npc:" .. a.npc_id
                end
                local s = _state
                if s then
                    s.notes = s.notes or {}
                    local entry = {
                        date    = (s.time or "") .. " " .. (s.day or ""),
                        content = note,
                        scope   = scope,
                    }
                    -- Witness gate: even a "public" note can only ever reach an
                    -- NPC who was actually co-located with the player right now
                    -- — not the whole cast instantly. Structural safety net for
                    -- when the model marks something "public" that was really
                    -- just a scene a few people happened to witness (a real bug
                    -- observed live: private/intimate home events tagged public
                    -- reached an NPC across town who "remembered" witnessing it
                    -- herself). Genuine world-wide spread still has to travel —
                    -- through dialogue, events, or the gossip system — not an
                    -- instant broadcast from one remember() call.
                    if scope == "public" then
                        local witnesses = {}
                        for id, loc in pairs(s.npc_locations or {}) do
                            if loc == s.player.location then witnesses[id] = true end
                        end
                        for id, loc in pairs(s.gen_npc_locations or {}) do
                            if loc == s.player.location then witnesses[id] = true end
                        end
                        entry.witnesses = witnesses
                    end
                    table.insert(s.notes, entry)
                    if #s.notes > 25 then table.remove(s.notes, 1) end
                end
                _log("remember", { note=note, scope=scope }, "ok")
                return json.encode({ ok=true, note=note, scope=scope })
            end,
        })
    end

    -- ── memory_write / memory_read ────────────────────────────────────────
    if cfg.use_memory and memory_lib then
        table.insert(t, {
            name = "memory_write",
            description = "Salva fatto confermato su un NPC. "
                       .. "Solo ciò che è avvenuto — NON intenzioni, deduzioni o piani. MAX 3/turno.",
            params = [[{
                "type": "object", "required": ["entity", "category", "content"],
                "properties": {
                    "entity":   { "type": "string" },
                    "category": { "type": "string" },
                    "content":  { "type": "string", "description": "max 40 parole" }
                }
            }]],
            fn = function(args_json)
                M.inc_tool("memwrite_count")
                if M.count_tool("memwrite_count") > 3 then
                    return json.encode({ error="memory_write: max 3/turno." })
                end
                local a = json.decode(args_json)
                memory_lib.write(a.entity, a.category, a.content)
                return json.encode({ ok=true })
            end,
        })
        table.insert(t, {
            name = "memory_read",
            description = "Leggi un fatto persistente su un NPC.",
            params = [[{
                "type": "object", "required": ["entity", "category"],
                "properties": {
                    "entity":   { "type": "string" },
                    "category": { "type": "string" }
                }
            }]],
            fn = function(args_json)
                local a = json.decode(args_json)
                return json.encode({ value = memory_lib.read(a.entity, a.category) or "(nessun ricordo)" })
            end,
        })
    end

    -- ── adventure-specific extra tools ────────────────────────────────────
    if extra then
        for _, tool in ipairs(extra) do
            table.insert(t, tool)
        end
    end

    return t
end

-- ============================================================
-- GUI INSPECTOR SUPPORT (rpgai-gui: Cast window + NPC windows)
-- Wire in the adventure with three one-liners:
--   function get_npc_list()           return adv.npc_list()                end
--   function get_npc_info(id)         return adv.npc_info(id)              end
--   function debug_npc_action(n,a,j)  return adv.debug_npc_action(n,a,j)   end
-- ============================================================

-- Position table that owns this npc (static vs generated), or nil.
local function npc_pos_table(id)
    local s = _state
    if not s then return nil end
    if (s.npc_locations     or {})[id] ~= nil then return s.npc_locations     end
    if (s.gen_npc_locations or {})[id] ~= nil then return s.gen_npc_locations end
    return nil
end

function M.npc_list()
    local s = _state
    if not s then return "[]" end
    local out = {}
    local function add(id, loc)
        local npc = _npc_data and _npc_data[id]
        out[#out+1] = { id = id, name = (npc and npc.name) or id, location = loc }
    end
    for id, loc in pairs(s.npc_locations     or {}) do add(id, loc) end
    for id, loc in pairs(s.gen_npc_locations or {}) do add(id, loc) end
    table.sort(out, function(a, b) return a.id < b.id end)
    return json.encode(out)
end

function M.npc_info(id)
    local s = _state
    local pos = npc_pos_table(id)
    if not s or not pos then
        return json.encode({ error = "NPC sconosciuto: " .. tostring(id) })
    end
    local npc = _npc_data and _npc_data[id]
    local ag  = _agents and _agents[id]
    return json.encode({
        name        = (npc and npc.name) or id,
        age         = npc and npc.age,
        location    = pos[id],
        activity    = (s.npc_activities or {})[id] or "",
        personality = npc and npc.description or "",
        goals       = ag and ag.short_term_goals or nil,
        goals_lungo = ag and ag.long_term_goals or nil,
    })
end

-- Puppet control from the GUI (engine gates this behind --debug-gui).
function M.debug_npc_action(npc_id, action, args_json)
    local s = _state
    local pos = npc_pos_table(npc_id)
    if not s or not pos then
        return json.encode({ success = false,
                             error = "NPC sconosciuto: " .. tostring(npc_id) })
    end
    local ok, args = pcall(json.decode, args_json or "{}")
    if not ok or type(args) ~= "table" then args = {} end

    if action == "move" and args.location and args.location ~= "" then
        if _locations and next(_locations) and _locations[args.location] == nil then
            return json.encode({ success = false,
                                 error = "Location sconosciuta: " .. args.location })
        end
        pos[npc_id] = args.location
        if s.npc_activities then s.npc_activities[npc_id] = nil end
        return json.encode({ success = true,
                             output = "[possess] " .. npc_id .. " -> " .. args.location })
    elseif action == "activity" then
        s.npc_activities = s.npc_activities or {}
        local act = args.object or args.activity or ""
        s.npc_activities[npc_id] = (act ~= "" and act) or nil
        return json.encode({ success = true,
                             output = act ~= "" and ("[possess] " .. npc_id .. ": " .. act)
                                               or  ("[possess] " .. npc_id .. " libero") })
    end
    return json.encode({ success = false,
                         error = "Azione sconosciuta: " .. tostring(action) })
end

return M
