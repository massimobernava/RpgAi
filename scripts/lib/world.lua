-- scripts/lib/world.lua
-- Procedural world expansion for RpgAi.
-- Generates NPC, location, and object entities on-demand via LLM.
-- All generated entities persist through the save system via snapshot() / restore().
--
-- QUICK START:
--
--   local world = require("lib/world")
--
--   -- Call once at startup (before any ensure_* calls):
--   world.init("An apartment building in Messina, 1990s.")
--
--   -- Pre-register static entities (not generated, just tracked):
--   world.set_npc("cettina", { name="Cettina", age=28, job="student",
--       personality="curious, enterprising", relationships={}, routine={} })
--
--   -- Generate on demand (idempotent — safe to call every turn):
--   local npc = world.ensure_npc("marco_203", "Neighbour on the same floor, apartment 203, teacher")
--   local loc = world.ensure_location("apt_203_salotto", "Living room in Marco's apartment 203")
--   local obj = world.ensure_object("microonde_marco", "Samsung microwave in Marco's apartment")
--
--   -- In get_tools() — expose generation to the main LLM:
--   world.as_tool_generate_npc("Generate a new character the player is encountering.")
--   world.as_tool_generate_location("Generate a new room the player wants to explore.")
--   world.as_tool_generate_object("Generate a new object the player is interacting with.")
--   world.as_tool_object_action("Execute an action on an already-generated object.")
--
--   -- In get_state_snapshot() — include generated world:
--   local snap = json.decode(json.encode(state))
--   snap._world = world.snapshot()
--   return json.encode(snap)
--
--   -- In restore_state() — reload generated world:
--   local data = json.decode(snapshot)
--   world.restore(data._world)
--   data._world = nil
--   state = data
--
--   -- Dynamic location enum for get_json_schema():
--   world.all_location_ids_json()  -- returns '"id1", "id2", ...'

local json = require("json")

local M = {}

-- ── Storage ────────────────────────────────────────────────────────────────────

local _npcs      = {}   -- id -> NPC data table
local _locations = {}   -- id -> Location data table
local _objects   = {}   -- id -> Object data table
local _context   = ""   -- world description injected into all generation prompts
local _model     = nil  -- optional model override (nil = engine default)
local _provider  = nil  -- optional provider override (nil = engine default)

local _world_file_path = nil  -- path to the world .lua file (nil = no file persistence)
local _situations      = {}   -- key -> text: active narrative situations
local _events          = {}   -- location_id -> array of {time, text, npcs?}
local MAX_EVENTS_PER_LOC = 20

-- ── Initialisation ─────────────────────────────────────────────────────────────

--- Set the world description used as context for all LLM generation calls.
-- @param context  String: setting, period, tone (1-3 sentences max).
-- @param opts     Optional table: { model="...", provider="..." } for generation calls.
function M.init(context, opts)
    _context  = context or ""
    opts      = opts or {}
    _model    = opts.model    or nil
    _provider = opts.provider or nil
end

-- ── Lua serialiser (for world file writes) ────────────────────────────────────

local function _lua_val(v, depth)
    depth = depth or 0
    local ind  = string.rep("    ", depth)
    local ind2 = string.rep("    ", depth + 1)
    local t = type(v)
    if t == "string" then
        return string.format("%q", v)
    elseif t == "number" or t == "boolean" then
        return tostring(v)
    elseif t == "table" then
        -- Detect pure sequence (keys 1..n, no gaps)
        local maxn, total = 0, 0
        local is_seq = true
        for k in pairs(v) do
            total = total + 1
            if type(k) == "number" and k == math.floor(k) and k >= 1 then
                if k > maxn then maxn = k end
            else
                is_seq = false
            end
        end
        if is_seq and maxn > 0 and total == maxn then
            local parts = {}
            for i = 1, maxn do
                table.insert(parts, ind2 .. _lua_val(v[i], depth + 1))
            end
            return "{\n" .. table.concat(parts, ",\n") .. "\n" .. ind .. "}"
        else
            local keys = {}
            for k in pairs(v) do table.insert(keys, k) end
            table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
            if #keys == 0 then return "{}" end
            local parts = {}
            for _, k in ipairs(keys) do
                local ks
                if type(k) == "string" and k:match("^[%a_][%w_]*$") then
                    ks = k
                else
                    ks = "[" .. string.format("%q", tostring(k)) .. "]"
                end
                table.insert(parts, ind2 .. ks .. " = " .. _lua_val(v[k], depth + 1))
            end
            return "{\n" .. table.concat(parts, ",\n") .. "\n" .. ind .. "}"
        end
    end
    return "nil"
end

local function _do_save_file()
    local path = _world_file_path
    if not path then return end
    local content = "-- " .. path .. "\n"
                 .. "-- World file per RpgAi. Editabile dal master.\n"
                 .. "-- Auto-aggiornato dal motore quando vengono generati nuovi luoghi/oggetti.\n"
                 .. "-- NON modificare la struttura delle sezioni — solo i valori interni.\n\n"
                 .. "return " .. _lua_val({
                        context    = _context,
                        situations = _situations,
                        events     = _events,
                        locations  = _locations,
                        objects    = _objects,
                        npcs       = _npcs,
                    }, 0) .. "\n"
    local f = io.open(path, "w")
    if not f then return end
    f:write(content)
    f:close()
end

-- ── Initialisation ─────────────────────────────────────────────────────────────

--- Load world state from a .lua file; generate that file if it does not yet exist.
-- The file is the authoritative source for locations/objects/situations/events.
-- Call instead of init() when you want file-backed persistence.
-- @param path     Path to the world .lua file (e.g. "./scripts/world_my_adventure.lua").
-- @param context  Fallback world description (used only when file does not exist or has none).
-- @param opts     Optional { model=..., provider=... }.
function M.init_file(path, context, opts)
    _world_file_path = path
    opts = opts or {}
    _model    = opts.model    or nil
    _provider = opts.provider or nil

    if path then
        local chunk, _ = loadfile(path)
        if chunk then
            local ok, data = pcall(chunk)
            if ok and type(data) == "table" then
                _context    = data.context    or context or ""
                _locations  = data.locations  or {}
                _objects    = data.objects    or {}
                _npcs       = data.npcs       or {}
                _situations = data.situations or {}
                _events     = data.events     or {}
                return
            end
        end
    end
    -- File missing or unreadable — start fresh
    M.init(context, opts)
end

--- Reload world state from the file set by init_file().
-- Call from restore_state() instead of world.restore(data._world).
function M.reload_file()
    if not _world_file_path then return end
    M.init_file(_world_file_path, _context, { model=_model, provider=_provider })
end

--- Persist current world state to the file set by init_file().
-- Called automatically on generation and mutations; also available to the script.
function M.save_file()
    _do_save_file()
end

-- ── Internal helpers ───────────────────────────────────────────────────────────

local function _call_llm(sys, user, schema)
    local ok, result = pcall(query_llm, sys, "[]", user, schema, _model, _provider)
    if not ok then return nil end
    local ok2, data = pcall(json.decode, result)
    return (ok2 and type(data) == "table") and data or nil
end

local function _list_names(registry)
    local parts = {}
    for id, ent in pairs(registry) do
        table.insert(parts, (ent.name or id) .. " [" .. id .. "]")
    end
    return #parts > 0 and table.concat(parts, ", ") or "none"
end

-- ── NPC generation ─────────────────────────────────────────────────────────────

local _NPC_SCHEMA = [[{
  "type": "object",
  "required": ["id","name","age","job","personality","relationships","routine"],
  "properties": {
    "id":           { "type": "string" },
    "name":         { "type": "string" },
    "age":          { "type": "integer", "minimum": 5, "maximum": 99 },
    "job":          { "type": "string" },
    "personality":  { "type": "string",
                      "description": "2-3 adjectives or short trait description" },
    "relationships":{ "type": "object",
                      "description": "key=role or person name, value=relationship description",
                      "additionalProperties": { "type": "string" } },
    "routine": {
      "type": "array",
      "minItems": 3,
      "items": {
        "type": "object",
        "required": ["time","location","action"],
        "properties": {
          "time":     { "type": "string", "description": "HH:MM format" },
          "location": { "type": "string" },
          "action":   { "type": "string" }
        }
      }
    }
  }
}]]

--- Get an NPC, generating via LLM if it does not yet exist.
-- Idempotent: safe to call multiple times with the same id.
-- @param id       Unique string (e.g. "marco_203").
-- @param context  Coherence hint: role, age range, relationship to world.
-- @return         NPC data table, or nil if generation failed.
function M.ensure_npc(id, context)
    if _npcs[id] then return _npcs[id] end

    local sys = "You are a character generator for a life-simulation RPG. "
             .. "Generate a character consistent with the described world. "
             .. "Reply ONLY with valid JSON according to the schema.\n\n"
             .. "World: " .. _context .. "\n"
             .. "Existing characters: " .. _list_names(_npcs)

    local user = "Generate the character with id='" .. id .. "'. "
              .. "Context: " .. (context or id)

    local data = _call_llm(sys, user, _NPC_SCHEMA)
    if data and data.name then
        data.id = id
        _npcs[id] = data
    end

    return _npcs[id]
end

-- ── Location generation ────────────────────────────────────────────────────────

local _LOCATION_SCHEMA = [[{
  "type": "object",
  "required": ["id","name","description","objects","connected_to"],
  "properties": {
    "id":           { "type": "string" },
    "name":         { "type": "string" },
    "description":  { "type": "string",
                      "description": "2-4 sentence atmospheric description" },
    "objects":      { "type": "array", "items": { "type": "string" },
                      "description": "list of object ids present here (may be empty)" },
    "connected_to": { "type": "array", "items": { "type": "string" },
                      "description": "list of location ids directly reachable from here" },
    "owner":        { "type": "string",
                      "description": "NPC id who controls this location; empty string if public" }
  }
}]]

--- Get a location, generating via LLM if it does not yet exist.
-- @param id       Unique string (e.g. "apt_203_salotto").
-- @param context  Coherence hint: type, owner, atmosphere, adjacent rooms.
-- @return         Location data table, or nil if generation failed.
function M.ensure_location(id, context)
    if _locations[id] then return _locations[id] end

    local sys = "You are a location generator for a life-simulation RPG. "
             .. "Generate a place consistent with the world and existing locations. "
             .. "Reply ONLY with valid JSON according to the schema.\n\n"
             .. "World: " .. _context .. "\n"
             .. "Existing locations: " .. _list_names(_locations)

    local user = "Generate the location with id='" .. id .. "'. "
              .. "Context: " .. (context or id)

    local data = _call_llm(sys, user, _LOCATION_SCHEMA)
    if data and data.name then
        data.id = id
        _locations[id] = data
        _do_save_file()
    end

    return _locations[id]
end

-- ── Object generation ──────────────────────────────────────────────────────────

local _OBJECT_SCHEMA = [[{
  "type": "object",
  "required": ["id","name","states","current_state","actions"],
  "properties": {
    "id":            { "type": "string" },
    "name":          { "type": "string" },
    "states":        { "type": "array", "minItems": 1,
                       "items": { "type": "string" } },
    "current_state": { "type": "string" },
    "actions": {
      "type": "object",
      "description": "key=action name, value=transition definition",
      "additionalProperties": {
        "type": "object",
        "required": ["from","to"],
        "properties": {
          "from":        { "type": "array", "items": { "type": "string" },
                          "description": "valid source states for this action" },
          "to":          { "type": "string",
                          "description": "target state after the action" },
          "duration_sec":{ "type": "integer", "minimum": 0 }
        }
      }
    },
    "data": {
      "type": "object",
      "description": "Structured content for this object. Use for: messages on a noticeboard, items in a container, entries in a register, etc. Keys are field names (e.g. 'messages', 'items'), values are arrays of entries. Each entry can be a string or an object with fields like {from, date, text}. Pre-populate with realistic initial content if the object logically has some.",
      "additionalProperties": true
    }
  }
}]]

--- Get an interactable object, generating via LLM if it does not yet exist.
-- @param id       Unique string (e.g. "microonde_marco").
-- @param context  Coherence hint: type, location, owner, expected states/actions.
-- @return         Object data table, or nil if generation failed.
function M.ensure_object(id, context)
    if _objects[id] then return _objects[id] end

    local sys = "You are an interactable object generator for an RPG. "
             .. "Generate an object with discrete states and actions consistent with the world. "
             .. "Reply ONLY with valid JSON according to the schema.\n\n"
             .. "World: " .. _context

    local user = "Generate the object with id='" .. id .. "'. "
              .. "Context: " .. (context or id)

    local data = _call_llm(sys, user, _OBJECT_SCHEMA)
    if data and data.name and data.current_state then
        data.id = id
        _objects[id] = data
        _do_save_file()
    end

    return _objects[id]
end

-- ── Object data mutation ──────────────────────────────────────────────────────

--- Merge a table into obj.data (shallow merge, creates data if missing).
-- @param id     Object id.
-- @param merge  Table of key→value to merge into obj.data.
-- @return       { ok=true } or { ok=false, error="..." }
function M.object_patch(id, merge)
    local obj = _objects[id]
    if not obj then return { ok=false, error="object not found: " .. tostring(id) } end
    obj.data = obj.data or {}
    for k, v in pairs(merge or {}) do obj.data[k] = v end
    _do_save_file()
    return { ok=true }
end

--- Append an entry to an array field inside obj.data.
-- Creates obj.data and the field array if they don't exist.
-- @param id     Object id.
-- @param field  Field name inside data (e.g. "messages", "items").
-- @param entry  Value to append (string or table).
-- @return       { ok=true, count=N } or { ok=false, error="..." }
function M.object_append(id, field, entry)
    local obj = _objects[id]
    if not obj then return { ok=false, error="object not found: " .. tostring(id) } end
    obj.data = obj.data or {}
    if type(obj.data[field]) ~= "table" then obj.data[field] = {} end
    table.insert(obj.data[field], entry)
    _do_save_file()
    return { ok=true, count=#obj.data[field] }
end

-- ── Object state machine ───────────────────────────────────────────────────────

--- Apply a named action to an object, transitioning its current_state.
-- @param id      Object id.
-- @param action  Action name (must exist in object.actions).
-- @return        { ok=true, state=new_state } or { ok=false, error="..." }
function M.object_action(id, action)
    local obj = _objects[id]
    if not obj then
        return { ok=false, error="object not found: " .. tostring(id) }
    end
    -- Universal read-only actions: return description + state without needing an actions entry
    local READ_ONLY = { esamina=true, guarda=true, leggi=true, ispeziona=true, osserva=true,
                        examine=true, look=true, read=true, inspect=true }
    if READ_ONLY[action] then
        return {
            ok          = true,
            id          = id,
            name        = obj.name or id,
            description = obj.description or "",
            state       = obj.current_state or "normale",
            data        = obj.data or nil,
        }
    end

    local act = obj.actions and obj.actions[action]
    if not act then
        local valid_actions = {}
        if obj.actions then
            for k in pairs(obj.actions) do table.insert(valid_actions, k) end
        end
        local hint = #valid_actions > 0
            and (". Azioni valide: " .. table.concat(valid_actions, ", "))
            or ". Questo oggetto non ha azioni definite — usa object_write per modificarne il contenuto."
        return { ok=false, error="unknown action: " .. tostring(action) .. hint }
    end
    local valid = false
    for _, s in ipairs(act.from or {}) do
        if s == obj.current_state then valid = true; break end
    end
    if not valid then
        return { ok=false,
                 error="action '" .. action .. "' not valid from state '" .. (obj.current_state or "?") .. "'" }
    end
    obj.current_state = act.to
    _do_save_file()
    return { ok=true, state=obj.current_state }
end

-- ── Direct read/write (no generation) ─────────────────────────────────────────

function M.get_npc(id)            return _npcs[id]           end
function M.get_location(id)       return _locations[id]       end
function M.get_object(id)         return _objects[id]         end

function M.set_npc(id, data)      _npcs[id] = data            end
function M.set_location(id, data) _locations[id] = data       end
function M.set_object(id, data)   _objects[id] = data         end

function M.all_npcs()             return _npcs               end
function M.all_locations()        return _locations           end
function M.all_objects()          return _objects             end

--- Return a JSON enum fragment with all known location ids.
-- Useful for building dynamic get_json_schema() movement enums:
--   '"" , ' .. world.all_location_ids_json()
function M.all_location_ids_json()
    local ids = {}
    for id in pairs(_locations) do table.insert(ids, '"' .. id .. '"') end
    table.sort(ids)
    return table.concat(ids, ", ")
end

-- ── Formatting helpers ─────────────────────────────────────────────────────────

--- Format an NPC's core profile as a readable multi-line string.
function M.format_npc(id)
    local npc = _npcs[id]
    if not npc then return "(unknown character: " .. id .. ")" end
    local lines = {
        "Name: "        .. (npc.name        or id),
        "Age: "         .. tostring(npc.age or "?"),
        "Job: "         .. (npc.job         or "?"),
        "Personality: " .. (npc.personality or "?"),
    }
    if npc.relationships and next(npc.relationships) then
        local rels = {}
        for k, v in pairs(npc.relationships) do table.insert(rels, k .. ": " .. v) end
        table.sort(rels)
        table.insert(lines, "Relationships: " .. table.concat(rels, "; "))
    end
    return table.concat(lines, "\n")
end

--- Format an NPC's daily routine as a readable multi-line string.
function M.format_routine(id)
    local npc = _npcs[id]
    if not npc or not npc.routine then return "" end
    local lines = {}
    for _, slot in ipairs(npc.routine) do
        table.insert(lines, string.format("  %s – %s (%s)", slot.time, slot.action, slot.location))
    end
    return table.concat(lines, "\n")
end

--- Format a location's data as a readable multi-line string.
function M.format_location(id)
    local loc = _locations[id]
    if not loc then return "(unknown location: " .. id .. ")" end
    local lines = { "Name: " .. (loc.name or id) }
    if loc.description then table.insert(lines, loc.description) end
    if loc.objects and #loc.objects > 0 then
        table.insert(lines, "Objects: " .. table.concat(loc.objects, ", "))
    end
    if loc.connected_to and #loc.connected_to > 0 then
        table.insert(lines, "Connected to: " .. table.concat(loc.connected_to, ", "))
    end
    return table.concat(lines, "\n")
end

--- Format an object's current state, available actions, and data content.
function M.format_object(id)
    local obj = _objects[id]
    if not obj then return "(unknown object: " .. id .. ")" end
    local available = {}
    for name, act in pairs(obj.actions or {}) do
        for _, s in ipairs(act.from or {}) do
            if s == obj.current_state then table.insert(available, name); break end
        end
    end
    table.sort(available)
    local base = string.format("%s [state: %s] — actions: %s",
        obj.name or id,
        obj.current_state or "?",
        #available > 0 and table.concat(available, ", ") or "none"
    )
    if not obj.data or not next(obj.data) then return base end
    -- Format data fields
    local data_lines = {}
    local fields = {}
    for k in pairs(obj.data) do table.insert(fields, k) end
    table.sort(fields)
    for _, field in ipairs(fields) do
        local val = obj.data[field]
        if type(val) == "table" then
            table.insert(data_lines, "  [" .. field .. "]")
            for i, entry in ipairs(val) do
                if type(entry) == "table" then
                    -- Render structured entry: join non-empty string fields
                    local parts = {}
                    for _, k in ipairs({"from","autore","data","date","text","testo","note"}) do
                        if entry[k] and entry[k] ~= "" then
                            table.insert(parts, tostring(entry[k]))
                        end
                    end
                    table.insert(data_lines, "    " .. i .. ". " .. table.concat(parts, " — "))
                else
                    table.insert(data_lines, "    " .. i .. ". " .. tostring(entry))
                end
            end
        else
            table.insert(data_lines, "  " .. field .. ": " .. tostring(val))
        end
    end
    return base .. "\n" .. table.concat(data_lines, "\n")
end

-- ── Ambient co-location events ─────────────────────────────────────────────────
-- Detects when 2+ NPCs share a location and helps scripts generate ambient interactions.
-- The script owns the async LLM call (query_llm_async / query_llm_poll);
-- world.lua only provides detection and formatting helpers.

--- Detect which locations have 2 or more NPCs present simultaneously.
-- @param npc_location_map  Table { npc_id → location_id } — current NPC positions.
-- @param min_npcs          Minimum co-located NPC count to trigger (default: 2).
-- @return Array of { location_id, npc_ids[] } for each qualifying location.
function M.check_colocation(npc_location_map, min_npcs)
    min_npcs = min_npcs or 2
    local by_loc = {}
    for npc_id, loc_id in pairs(npc_location_map or {}) do
        if loc_id and loc_id ~= "" then
            by_loc[loc_id] = by_loc[loc_id] or {}
            table.insert(by_loc[loc_id], npc_id)
        end
    end
    local result = {}
    for loc_id, npc_ids in pairs(by_loc) do
        if #npc_ids >= min_npcs then
            table.insert(result, { location_id=loc_id, npc_ids=npc_ids })
        end
    end
    return result
end

-- JSON schema for the ambient event LLM call.
-- The script passes this to query_llm_async.
M.AMBIENT_SCHEMA = [[{
  "type": "object",
  "required": ["event_summary", "life_event_a", "life_event_b"],
  "properties": {
    "event_summary":  { "type": "string",
                        "description": "1-2 sentences describing what happened between the NPCs. Natural, vivid, specific." },
    "life_event_a":   { "type": "string",
                        "description": "One-line life event entry for the first NPC (from their POV)." },
    "life_event_b":   { "type": "string",
                        "description": "One-line life event entry for the second NPC (from their POV). Omit if more than 2 NPCs." },
    "mood_shift_a":   { "type": "string", "enum": ["none","positive","negative","neutral"],
                        "description": "Emotional impact on first NPC." },
    "mood_shift_b":   { "type": "string", "enum": ["none","positive","negative","neutral"],
                        "description": "Emotional impact on second NPC." },
    "stat_delta":     { "type": "object",
                        "description": "Optional stat changes keyed by npc_id. E.g. { 'rosangela_201': { stress: -0.1 } }",
                        "additionalProperties": { "type": "object", "additionalProperties": { "type": "number" } } }
  }
}]]

--- Build the system + user prompt for an ambient event LLM call.
-- The script calls query_llm_async(sys, "[]", user, world.AMBIENT_SCHEMA).
-- @param location_id   Location where co-location happened.
-- @param npc_ids       Array of NPC ids present.
-- @param time_str      Current in-game time (e.g. "08:30").
-- @param date_str      Current in-game date label (e.g. "day 3").
-- @param persona_lib   Optional: loaded persona module; used to inject NPC profiles.
-- @return sys, user    Two strings ready for query_llm_async.
function M.ambient_prompt(location_id, npc_ids, time_str, date_str, persona_lib)
    local loc = _locations[location_id]
    local loc_desc = loc and (loc.name .. ": " .. (loc.description or "")) or location_id

    local sys = "You are an ambient event generator for a life-simulation RPG. "
             .. "Two or more NPCs are in the same location at the same time. "
             .. "Generate a brief, believable ambient interaction between them. "
             .. "Keep it grounded — everyday life, not drama (unless the characters' profiles suggest tension). "
             .. "World: " .. _context

    local npc_lines = {}
    for _, npc_id in ipairs(npc_ids) do
        if persona_lib and persona_lib.format then
            table.insert(npc_lines, "[" .. npc_id .. "]\n" .. persona_lib.format(npc_id))
        else
            local npc = _npcs[npc_id]
            if npc then
                table.insert(npc_lines, "[" .. npc_id .. "] " .. (npc.name or npc_id)
                    .. ", " .. tostring(npc.age or "?") .. ", " .. (npc.job or "?"))
            else
                table.insert(npc_lines, "[" .. npc_id .. "] (unknown)")
            end
        end
    end

    local user = string.format(
        "Location: %s\nTime: %s, %s\n\nNPCs present:\n%s\n\nGenerate the ambient interaction.",
        loc_desc, time_str or "?", date_str or "?",
        table.concat(npc_lines, "\n\n")
    )

    return sys, user
end

--- Return a list of location ids reachable from loc_id.
-- Reads connected_to from the world registry.
-- @param loc_id           Source location id.
-- @param extra_connections Optional table { loc_id → {connected_id, ...} } for
--                          locations not in the world registry (e.g. hardcoded rooms
--                          in the script). Merged with registry data; script decides
--                          whether to use this as a movement filter or prompt hint.
-- @return Array of reachable location ids (may be empty). Never includes loc_id itself.
function M.reachable_from(loc_id, extra_connections)
    local seen = {}

    -- From world registry
    local loc = _locations[loc_id]
    if loc and loc.connected_to then
        for _, id in ipairs(loc.connected_to) do
            if id ~= loc_id then seen[id] = true end
        end
    end

    -- From extra_connections provided by the script
    if extra_connections and extra_connections[loc_id] then
        for _, id in ipairs(extra_connections[loc_id]) do
            if id ~= loc_id then seen[id] = true end
        end
    end

    local result = {}
    for id in pairs(seen) do table.insert(result, id) end
    table.sort(result)
    return result
end

-- ── Snapshot / restore ─────────────────────────────────────────────────────────

--- Serialize all generated entities to a JSON string for the save system.
-- Store in get_state_snapshot() under any key (e.g. state._world = world.snapshot()).
function M.snapshot()
    return json.encode({
        npcs       = _npcs,
        locations  = _locations,
        objects    = _objects,
        situations = _situations,
        events     = _events,
    })
end

--- Restore generated entities from a snapshot string (output of M.snapshot()).
-- Call from restore_state() before re-assigning state.
-- For file-backed worlds, prefer world.reload_file() instead.
function M.restore(data)
    if not data or data == "" then return end
    local ok, s = pcall(json.decode, data)
    if not ok or type(s) ~= "table" then return end
    if s.npcs       then _npcs       = s.npcs       end
    if s.locations  then _locations  = s.locations  end
    if s.objects    then _objects    = s.objects    end
    if s.situations then _situations = s.situations end
    if s.events     then _events     = s.events     end
end

-- ── Tool factories ─────────────────────────────────────────────────────────────
-- Each returns a ToolDef compatible with tools.build() / get_tools().
-- The main LLM calls these tools when it encounters an entity it hasn't seen before.

--- ToolDef: generate a new NPC.
function M.as_tool_generate_npc(description)
    return {
        name        = "generate_npc",
        description = description or
            "Generate a new NPC the player is meeting for the first time. "
            .. "Call ONCE per new character before narrating their appearance. "
            .. "Returns the NPC profile for use in narration.",
        params      = [[{
            "type": "object",
            "required": ["id", "context"],
            "properties": {
                "id":      { "type": "string",
                             "description": "Unique snake_case id (e.g. 'marco_203'). "
                                         .. "Use a stable, descriptive key." },
                "context": { "type": "string",
                             "description": "Who this person should be: role, rough age, "
                                         .. "relationship to the world, tone." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            if not a.id or a.id == "" then
                return json.encode({ error="id is required" })
            end
            if _npcs[a.id] then
                return json.encode({ ok=true, already_exists=true,
                                     id=a.id, summary=M.format_npc(a.id) })
            end
            local npc = M.ensure_npc(a.id, a.context)
            if not npc then
                return json.encode({ error="generation failed — LLM returned invalid data" })
            end
            return json.encode({
                ok           = true,
                id           = npc.id,
                name         = npc.name,
                age          = npc.age,
                job          = npc.job,
                personality  = npc.personality,
                relationships = npc.relationships,
            })
        end,
    }
end

--- ToolDef: generate a new location.
function M.as_tool_generate_location(description)
    return {
        name        = "generate_location",
        description = description or
            "Generate a new location the player is entering for the first time. "
            .. "Call ONCE per new room or place before narrating it. "
            .. "Returns location data: name, description, objects, exits.",
        params      = [[{
            "type": "object",
            "required": ["id", "context"],
            "properties": {
                "id":      { "type": "string",
                             "description": "Unique snake_case location id (e.g. 'apt_203_salotto')." },
                "context": { "type": "string",
                             "description": "What this place should be: type, owner, atmosphere, "
                                         .. "adjacent rooms, objects expected inside." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            if not a.id or a.id == "" then
                return json.encode({ error="id is required" })
            end
            if _locations[a.id] then
                return json.encode({ ok=true, already_exists=true,
                                     id=a.id, summary=M.format_location(a.id) })
            end
            local loc = M.ensure_location(a.id, a.context)
            if not loc then
                return json.encode({ error="generation failed" })
            end
            return json.encode({
                ok           = true,
                id           = loc.id,
                name         = loc.name,
                description  = loc.description,
                objects      = loc.objects,
                connected_to = loc.connected_to,
                owner        = loc.owner,
            })
        end,
    }
end

--- ToolDef: generate a new interactable object.
function M.as_tool_generate_object(description)
    return {
        name        = "generate_object",
        description = description or
            "Generate a new interactable object the player is examining for the first time. "
            .. "Call ONCE per new object before narrating an interaction. "
            .. "Returns the object's states and available actions.",
        params      = [[{
            "type": "object",
            "required": ["id", "context"],
            "properties": {
                "id":      { "type": "string",
                             "description": "Unique snake_case object id (e.g. 'microonde_marco')." },
                "context": { "type": "string",
                             "description": "What this object is: type, location, owner, "
                                         .. "expected states and actions." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            if not a.id or a.id == "" then
                return json.encode({ error="id is required" })
            end
            if _objects[a.id] then
                return json.encode({ ok=true, already_exists=true,
                                     id=a.id, summary=M.format_object(a.id) })
            end
            local obj = M.ensure_object(a.id, a.context)
            if not obj then
                return json.encode({ error="generation failed" })
            end
            return json.encode({
                ok            = true,
                id            = obj.id,
                name          = obj.name,
                states        = obj.states,
                current_state = obj.current_state,
                actions       = obj.actions,
            })
        end,
    }
end

--- ToolDef: apply an action to an existing interactable object.
function M.as_tool_object_action(description)
    return {
        name        = "object_action",
        description = description or
            "Apply an action to an interactable object (open, turn on, fill, etc.). "
            .. "Call AFTER generate_object if the object is new. "
            .. "Returns new state, or error if action not valid from current state.",
        params      = [[{
            "type": "object",
            "required": ["id", "action"],
            "properties": {
                "id":     { "type": "string",
                            "description": "Object identifier (same id used in generate_object)." },
                "action": { "type": "string",
                            "description": "Action name. Must be valid from the object's current state." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            return json.encode(M.object_action(a.id or "", a.action or ""))
        end,
    }
end

-- ── Situations & events ────────────────────────────────────────────────────────

--- Set a named active situation (overwrites if key exists).
-- @param key   Short identifier (e.g. "infiltrazione_tetto").
-- @param text  One-sentence description injected into system prompt.
function M.set_situation(key, text)
    _situations[key] = text
    _do_save_file()
end

--- Remove a named situation.
function M.clear_situation(key)
    _situations[key] = nil
    _do_save_file()
end

--- Format all active situations as a prompt-ready block.
-- Returns "" if no situations are active.
function M.format_situations()
    local parts = {}
    for k, v in pairs(_situations) do
        table.insert(parts, "  [" .. k .. "] " .. v)
    end
    if #parts == 0 then return "" end
    table.sort(parts)
    return "SITUAZIONI ATTIVE NEL MONDO:\n" .. table.concat(parts, "\n")
end

--- Append a timestamped event to a location's event log.
-- Auto-saves the world file. Prunes to MAX_EVENTS_PER_LOC oldest events.
-- @param location_id  Where this happened.
-- @param time_str     In-game time string (e.g. "08:30").
-- @param text         One-sentence event description.
-- @param npcs         Optional array of NPC ids involved.
function M.log_event(location_id, time_str, text, npcs)
    _events[location_id] = _events[location_id] or {}
    local ev = { time=time_str or "?", text=text or "" }
    if npcs and #npcs > 0 then ev.npcs = npcs end
    table.insert(_events[location_id], ev)
    local arr = _events[location_id]
    while #arr > MAX_EVENTS_PER_LOC do table.remove(arr, 1) end
    _do_save_file()
end

--- Return a formatted string of the last n events at a location.
-- Returns "" if no events recorded.
function M.format_recent_events(location_id, n)
    local arr = _events[location_id]
    if not arr or #arr == 0 then return "" end
    n = n or 5
    local start = math.max(1, #arr - n + 1)
    local lines = {}
    for i = start, #arr do
        local ev = arr[i]
        local s = ev.time .. ": " .. ev.text
        if ev.npcs and #ev.npcs > 0 then
            s = s .. " [" .. table.concat(ev.npcs, ", ") .. "]"
        end
        table.insert(lines, s)
    end
    return table.concat(lines, "\n")
end

--- ToolDef: main LLM can log a world event (e.g. a fight, a discovery, a change).
function M.as_tool_log_event(description)
    return {
        name        = "world_event",
        description = description or
            "Registra un evento significativo accaduto in una location. "
            .. "Usa per: confronti, scoperte, cambiamenti ambientali importanti. "
            .. "Gli eventi persistono e possono emergere in scene future.",
        params      = [[{
            "type": "object",
            "required": ["location_id", "text"],
            "properties": {
                "location_id": { "type": "string", "description": "Dove è accaduto." },
                "time":        { "type": "string", "description": "Ora in gioco (HH:MM)." },
                "text":        { "type": "string", "description": "Descrizione dell'evento in una frase." },
                "npcs":        { "type": "array", "items": { "type": "string" },
                                 "description": "ID degli NPC coinvolti (opzionale)." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            if not a.location_id or a.location_id == "" then
                return json.encode({ error="location_id required" })
            end
            M.log_event(a.location_id, a.time, a.text, a.npcs)
            return json.encode({ ok=true })
        end,
    }
end

--- ToolDef: write structured content into an object's data field.
-- Use for: posting on a noticeboard, adding an item to a container,
-- updating a register, removing an entry, etc.
function M.as_tool_object_write(description)
    return {
        name        = "object_write",
        description = description or
            "Write or append structured content to an interactable object's data. "
            .. "Use for: posting a notice on a noticeboard, adding an item to a container, "
            .. "updating a list. The object must already exist (call generate_object first). "
            .. "Use 'append' to add entries, 'remove_index' to delete one by position.",
        params      = [[{
            "type": "object",
            "required": ["id", "field"],
            "properties": {
                "id":           { "type": "string", "description": "Object identifier." },
                "field":        { "type": "string", "description": "Data field name (e.g. 'messages', 'items')." },
                "append":       { "description": "Entry to append to the field array. Can be a string or an object with fields like {from, date, text}." },
                "remove_index": { "type": "integer", "minimum": 1,
                                  "description": "1-based index of an existing entry to remove." },
                "set":          { "description": "Replace the entire field with this value." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local id    = a.id    or ""
            local field = a.field or ""
            if id == "" or field == "" then
                return json.encode({ error="id and field are required" })
            end
            local obj = _objects[id]
            if not obj then
                return json.encode({ error="object not found: " .. id .. " — call generate_object first" })
            end
            obj.data = obj.data or {}

            if a.set ~= nil then
                obj.data[field] = a.set
                return json.encode({ ok=true, action="set", field=field })
            end

            if a.remove_index then
                local arr = obj.data[field]
                if type(arr) ~= "table" or a.remove_index > #arr then
                    return json.encode({ ok=false, error="index out of range" })
                end
                table.remove(arr, a.remove_index)
                return json.encode({ ok=true, action="removed", field=field, count=#arr })
            end

            if a.append ~= nil then
                local res = M.object_append(id, field, a.append)
                res.action = "appended"
                res.field  = field
                return json.encode(res)
            end

            return json.encode({ error="specify 'append', 'remove_index', or 'set'" })
        end,
    }
end

return M
