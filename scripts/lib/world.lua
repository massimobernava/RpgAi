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
local wlog = require("lib/log")

local M = {}

-- ── Storage ────────────────────────────────────────────────────────────────────

local _npcs      = {}   -- id -> NPC data table
local _locations = {}   -- id -> Location data table
local _objects   = {}   -- id -> Object data table
local _context   = ""   -- world description injected into all generation prompts
local _model     = nil  -- optional model override (nil = engine default)
local _provider  = nil  -- optional provider override (nil = engine default)

local _world_file_path = nil  -- path to the world .lua file (nil = no file persistence)
local _save_warned     = false -- one-shot warning when the world file is unwritable
local _last_missing_obj = nil -- last object id an LLM tried to act on but didn't exist;
                              -- lets generate_object({}) recover the intended id
local _last_missing_loc = nil -- same, for locations (set by adventure.move_player)
local _current_turn    = nil  -- set by adventure.before_turn; stamped on events
local _world_ctx       = {}   -- {flags={}, time="HH:MM"} — set by set_world_state each turn
local _bible           = {}   -- key -> authoritative world fact (string)
local _claims          = {}   -- category -> key -> owner_id (exclusive assignments)
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
    -- Start fresh: clear all registries. Libs stay cached in package.loaded
    -- across script hot-swaps, so without this a new adventure would inherit
    -- NPCs/locations/objects (and the world file path) from the previous one.
    _npcs       = {}
    _locations  = {}
    _objects    = {}
    _situations = {}
    _events     = {}
    _bible      = {}
    _claims     = {}
    _world_file_path = nil
    _save_warned     = false
    if M.reset_colocation then M.reset_colocation() end
end

-- ── Lua serialiser (for world file writes) ────────────────────────────────────

local function _lua_val(v, depth)
    depth = depth or 0
    -- Depth cap: cyclic/deep tables must not stack-overflow the writer.
    if depth > 12 then return string.format("%q", "[max depth]") end
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
                        bible      = _bible,
                        claims     = _claims,
                    }, 0) .. "\n"
    -- Atomic: temp file + rename — a crash mid-write must never truncate
    -- the world file.
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then
        -- Don't fail silently: an unwritable path (typo in init_file, missing
        -- directory) would otherwise disable persistence with zero feedback.
        if not _save_warned then
            _save_warned = true
            wlog.warn("world", "cannot write world file '" .. path
                .. "' — world persistence is NOT working.")
        end
        return
    end
    f:write(content)
    f:close()
    if not os.rename(tmp, path) then os.remove(tmp) end
end

-- ── Initialisation ─────────────────────────────────────────────────────────────

-- Rekey an id-keyed registry in place, merging any key that sanitizes to a
-- different (clean) id. Uses M.sanitize_id (resolved at call time, so it may be
-- defined later in the file). is_array_log=true concatenates event arrays;
-- otherwise the existing clean entry wins and the corrupt duplicate is dropped.
local function _heal_registry_keys(reg, is_array_log)
    if type(reg) ~= "table" then return false end
    local rename = {}
    for k in pairs(reg) do
        local clean = M.sanitize_id(k)
        if clean and clean ~= k then rename[k] = clean end
    end
    if not next(rename) then return false end
    for bad, clean in pairs(rename) do
        local v = reg[bad]; reg[bad] = nil
        if is_array_log then
            local dst = reg[clean] or {}
            for _, ev in ipairs(v) do dst[#dst + 1] = ev end
            table.sort(dst, function(a, b) return (a.turn or 0) < (b.turn or 0) end)
            reg[clean] = dst
        elseif reg[clean] == nil then
            if type(v) == "table" then v.id = clean end
            reg[clean] = v
        end
    end
    return true
end

--- Load world state from a .lua file; generate that file if it does not yet exist.
-- The file is the authoritative source for locations/objects/situations/events.
-- Call instead of init() when you want file-backed persistence.
-- @param path     Path to the world .lua file (e.g. "./scripts/world_my_adventure.lua").
-- @param context  Fallback world description (used only when file does not exist or has none).
-- @param opts     Optional { model=..., provider=... }.
function M.init_file(path, context, opts)
    opts = opts or {}

    if path then
        local chunk, lerr = loadfile(path)
        if chunk then
            local ok, data = pcall(chunk)
            if ok and type(data) == "table" then
                _world_file_path = path
                _save_warned = false
                _model    = opts.model    or nil
                _provider = opts.provider or nil
                _context    = data.context    or context or ""
                _locations  = data.locations  or {}
                _objects    = data.objects    or {}
                _npcs       = data.npcs       or {}
                _situations = data.situations or {}
                _events     = data.events     or {}
                _bible      = data.bible      or {}
                _claims     = data.claims     or {}
                -- Self-heal: legacy world files may carry id keys corrupted by an
                -- LLM before sanitize_id existed (e.g. "ingresso埋"). Rekey them so
                -- the phantom duplicate merges back into the real location.
                local healed = false
                healed = _heal_registry_keys(_locations, false) or healed
                healed = _heal_registry_keys(_objects,   false) or healed
                healed = _heal_registry_keys(_events,    true)   or healed  -- arrays → concat
                if healed then _do_save_file() end
                return
            end
            wlog.warn("world", "world file '" .. path
                .. "' exists but is invalid (" .. tostring(data) .. ") — starting fresh.")
        elseif lerr and not tostring(lerr):match("No such file") then
            wlog.warn("world", "world file '" .. path
                .. "' could not be loaded (" .. tostring(lerr) .. ") — starting fresh.")
        end
    end
    -- File missing or unreadable — start fresh.
    -- M.init clears registries AND _world_file_path; re-set the path after so
    -- future saves (re)create the file.
    M.init(context, opts)
    _world_file_path = path
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

local llm_util = require("lib/llm_util")

-- LLM call with structural validation + repair retries (see lib/llm_util.lua).
-- validate_fn optional: fn(data) -> true | false, "error".
local function _call_llm(sys, user, schema, validate_fn)
    local data, err = llm_util.validated_call(sys, user, schema, validate_fn,
        { retries = 2, model = _model, provider = _provider, tier = "gen" })
    if not data then
        wlog.warn("world", "generation failed: " .. tostring(err))
        return nil
    end
    return data
end

-- ── Structural validators for generated entities ──────────────────────────────
-- The JSON schema constrains the LLM, but providers don't all enforce it and
-- models drift. These checks (with normalization where unambiguous) are what
-- downstream code actually relies on.

local function _validate_npc(d)
    local ok, err = llm_util.is_str(d.name, "name"); if not ok then return ok, err end
    ok, err = llm_util.is_array(d.routine, "routine", function(slot)
        if type(slot) ~= "table" then return false, "must be an object" end
        if type(slot.time) ~= "string" or type(slot.location) ~= "string"
           or type(slot.action) ~= "string" then
            return false, "needs string fields time, location, action"
        end
        return true
    end)
    if not ok then return ok, err end
    if d.relationships ~= nil and type(d.relationships) ~= "table" then
        return false, "field 'relationships' must be an object"
    end
    return true
end

local function _validate_location(d)
    local ok, err = llm_util.is_str(d.name, "name"); if not ok then return ok, err end
    ok, err = llm_util.is_str(d.description, "description"); if not ok then return ok, err end
    if d.connected_to ~= nil then
        ok, err = llm_util.is_array(d.connected_to, "connected_to", function(c)
            if type(c) ~= "string" or c == "" then
                return false, "must be a non-empty location id string"
            end
            return true
        end)
        if not ok then return ok, err end
    end
    if d.objects ~= nil and type(d.objects) ~= "table" then
        return false, "field 'objects' must be an array of object id strings"
    end
    return true
end

local function _validate_object(d)
    local ok, err = llm_util.is_str(d.name, "name"); if not ok then return ok, err end
    ok, err = llm_util.is_array(d.states, "states", function(s)
        if type(s) ~= "string" or s == "" then return false, "must be a string" end
        return true
    end)
    if not ok then return ok, err end
    if #d.states == 0 then return false, "field 'states' must not be empty" end
    ok, err = llm_util.is_str(d.current_state, "current_state")
    if not ok then return ok, err end
    local cs_known = false
    for _, s in ipairs(d.states) do if s == d.current_state then cs_known = true end end
    if not cs_known then
        return false, "current_state '" .. d.current_state .. "' is not in states"
    end
    for act_name, act in pairs(d.actions or {}) do
        if type(act) ~= "table" then
            return false, "action '" .. tostring(act_name) .. "' must be an object"
        end
        -- Normalize: a single 'from' string becomes a one-element array.
        if type(act.from) == "string" then act.from = { act.from } end
        if type(act.from) ~= "table" then
            return false, "action '" .. tostring(act_name) .. "': 'from' must be an array of states"
        end
        if type(act.to) ~= "string" or act.to == "" then
            return false, "action '" .. tostring(act_name) .. "': 'to' must be a state string"
        end
        if act.requires ~= nil and type(act.requires) ~= "string" then
            return false, "action '" .. tostring(act_name) .. "': 'requires' must be an object id string"
        end
    end
    return true
end

local function _list_names(registry)
    local parts = {}
    for id, ent in pairs(registry) do
        table.insert(parts, (ent.name or id) .. " [" .. id .. "]")
    end
    return #parts > 0 and table.concat(parts, ", ") or "none"
end

--- Sanitize an entity id coming from an LLM. Ids are dictionary keys (locations,
-- objects, event logs) and MUST be plain snake_case ASCII — an LLM occasionally
-- appends stray Unicode (e.g. "ingresso埋"), creating a phantom duplicate key
-- that the engine treats as a distinct place. Strips anything outside
-- [a-z0-9_], lowercases, collapses repeats. Returns nil if nothing survives.
function M.sanitize_id(id)
    if type(id) ~= "string" then return nil end
    id = id:lower():gsub("[^%w_]", "_"):gsub("_+", "_"):gsub("^_", ""):gsub("_$", "")
    if id == "" then return nil end
    return id
end
local _sanitize_id = M.sanitize_id

-- ── World bible ────────────────────────────────────────────────────────────────
-- Authoritative shared facts. Every generation prompt (world AND persona)
-- injects this block, so independently generated entities respect the same
-- truth instead of contradicting each other (the #1 coherence failure mode:
-- two NPCs claiming the same apartment, inconsistent building layout, etc.).

--- Set an authoritative world fact. Overwrites by key.
-- @param key   Short stable key (e.g. "palazzo", "ascensore").
-- @param fact  One-sentence fact (e.g. "Il palazzo ha 3 piani, 2 appartamenti per piano.").
function M.bible_set(key, fact)
    if not key or key == "" then return false end
    _bible[key] = fact
    _do_save_file()
    return true
end

function M.bible_get(key)    return _bible[key] end
function M.bible_remove(key) _bible[key] = nil; _do_save_file() end
function M.bible_all()       return _bible end

--- Claim an assignment (e.g. apartment → NPC).
-- The claim answers "who lives/works here", not "who is the SOLE occupant":
-- with opts.share=true a new member is APPENDED to an existing claim
-- (families, flatmates, job shifts). Without share, a different owner is
-- rejected — the exclusive default catches accidental double assignment.
--
-- @param category  Namespace (e.g. "apartment", "job_slot").
-- @param key       The claimed resource (e.g. "203").
-- @param owner     Member id (e.g. "marco_203").
-- @param opts      Optional { share=true } — co-occupancy allowed.
-- @return { ok=true, owner=<full member list> } or { ok=false, owner=<existing> }
function M.bible_claim(category, key, owner, opts)
    if not (category and key and owner) then return { ok=false, owner=nil } end
    _claims[category] = _claims[category] or {}
    local existing = _claims[category][key]
    if existing and existing ~= owner then
        -- Already a listed member? (claims with share store "a, b, c")
        for member in existing:gmatch("[^,]+") do
            if member:match("^%s*(.-)%s*$") == owner then
                return { ok=true, owner=existing }
            end
        end
        if opts and opts.share then
            _claims[category][key] = existing .. ", " .. owner
            _do_save_file()
            return { ok=true, owner=_claims[category][key] }
        end
        return { ok=false, owner=existing }
    end
    _claims[category][key] = owner
    _do_save_file()
    return { ok=true, owner=owner }
end

function M.bible_release(category, key)
    if _claims[category] then _claims[category][key] = nil; _do_save_file() end
end

function M.claim_owner(category, key)
    return _claims[category] and _claims[category][key] or nil
end

--- Format bible facts + claims as a prompt block. "" if empty.
function M.format_bible()
    local lines = {}
    local keys = {}
    for k in pairs(_bible) do table.insert(keys, k) end
    table.sort(keys)
    for _, k in ipairs(keys) do
        table.insert(lines, "  [" .. k .. "] " .. _bible[k])
    end
    local cats = {}
    for c in pairs(_claims) do table.insert(cats, c) end
    table.sort(cats)
    for _, c in ipairs(cats) do
        local ks = {}
        for k in pairs(_claims[c]) do table.insert(ks, k) end
        table.sort(ks)
        for _, k in ipairs(ks) do
            table.insert(lines, "  [" .. c .. " " .. k .. "] assigned to: " .. _claims[c][k])
        end
    end
    if #lines == 0 then return "" end
    return "WORLD FACTS (authoritative — never contradict, never reassign):\n"
        .. table.concat(lines, "\n")
end

-- Internal: bible block for generation prompts ("" if empty).
local function _bible_block()
    local b = M.format_bible()
    return b ~= "" and ("\n" .. b) or ""
end

--- ToolDef: main LLM records a canonical world fact during narration.
-- Once recorded, the fact constrains every future generation.
function M.as_tool_bible(description)
    return {
        name        = "world_fact",
        description = description or
            "Registra un fatto CANONICO e permanente del mondo "
            .. "(struttura del palazzo, regole del luogo, assegnazioni). "
            .. "Usalo quando la narrazione stabilisce qualcosa di strutturale. "
            .. "Tutte le generazioni future lo rispetteranno. "
            .. "NON usarlo per eventi temporanei (usa world_event).",
        params      = [[{
            "type": "object",
            "required": ["key", "fact"],
            "properties": {
                "key":  { "type": "string", "description": "Chiave breve e stabile (snake_case), es. 'palazzo_piani'." },
                "fact": { "type": "string", "description": "Il fatto, una frase. Es. 'Il palazzo ha 3 piani, 2 appartamenti per piano.'" }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            if not a.key or a.key == "" or not a.fact or a.fact == "" then
                return json.encode({ error="key and fact are required" })
            end
            local prev = _bible[a.key]
            M.bible_set(a.key, a.fact)
            return json.encode({ ok=true, key=a.key,
                                 replaced=(prev ~= nil) and prev or nil })
        end,
    }
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
             .. _bible_block()

    local user = "Generate the character with id='" .. id .. "'. "
              .. "Context: " .. (context or id)

    local data = _call_llm(sys, user, _NPC_SCHEMA, _validate_npc)
    if data and data.name then
        data.id = id
        _npcs[id] = data
        _do_save_file()  -- same persistence as ensure_location / ensure_object
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

-- Graph repair after generation. The LLM invents connected_to freely:
-- self-edges, duplicates, one-way passages. This pass guarantees:
--   - no self-edges, no duplicates
--   - a way back to from_id (where the player came from), if given
--   - bidirectionality with every EXISTING location (both directions:
--     targets of this location get a reverse edge; earlier locations that
--     already referenced this id get linked back)
-- Unknown ids are KEPT — they are seeds for future expansion
-- (see M.pending_locations()).
local function _repair_location_graph(id, from_id)
    local loc = _locations[id]
    if not loc then return end
    local seen, cleaned = {}, {}
    for _, c in ipairs(loc.connected_to or {}) do
        if type(c) == "string" and c ~= "" and c ~= id and not seen[c] then
            seen[c] = true
            table.insert(cleaned, c)
        end
    end
    loc.connected_to = cleaned

    if from_id and from_id ~= id and not seen[from_id] then
        table.insert(loc.connected_to, from_id)
        seen[from_id] = true
    end

    -- Reverse edges into existing targets
    for _, c in ipairs(loc.connected_to) do
        local other = _locations[c]
        if other then
            other.connected_to = other.connected_to or {}
            local has = false
            for _, oc in ipairs(other.connected_to) do
                if oc == id then has = true; break end
            end
            if not has then table.insert(other.connected_to, id) end
        end
    end

    -- Earlier locations that already referenced this id → link back
    for oid, other in pairs(_locations) do
        if oid ~= id and not seen[oid] then
            for _, oc in ipairs(other.connected_to or {}) do
                if oc == id then
                    table.insert(loc.connected_to, oid)
                    seen[oid] = true
                    break
                end
            end
        end
    end
end

--- Get a location, generating via LLM if it does not yet exist.
-- @param id       Unique string (e.g. "apt_203_salotto").
-- @param context  Coherence hint: type, owner, atmosphere, adjacent rooms.
-- @param opts     Optional { from="loc_id" } — guarantees a way back to that
--                 location (use the player's current position).
-- @return         Location data table, or nil if generation failed.
function M.ensure_location(id, context, opts)
    id = _sanitize_id(id); if not id then return nil end
    if _locations[id] then return _locations[id] end

    local sys = "You are a location generator for a life-simulation RPG. "
             .. "Generate a place consistent with the world and existing locations. "
             .. "Reply ONLY with valid JSON according to the schema.\n\n"
             .. "World: " .. _context .. "\n"
             .. "Existing locations: " .. _list_names(_locations)
             .. _bible_block()

    local user = "Generate the location with id='" .. id .. "'. "
              .. "Context: " .. (context or id)

    local data = _call_llm(sys, user, _LOCATION_SCHEMA, _validate_location)
    if data and data.name then
        data.id = id
        _locations[id] = data
        _repair_location_graph(id, opts and opts.from)
        _do_save_file()
    end

    return _locations[id]
end

--- Direct neighbors of a location in the GENERATED graph, SYMMETRIC:
-- connected_to of id PLUS every generated location whose connected_to
-- references id. The symmetry matters at the static↔generated boundary —
-- a generated room links back to a static room via its own connected_to,
-- but the static room (not in this registry) can't carry the reverse edge,
-- so we derive it here. adventure.lua merges this with the static TRAVEL_MAP.
function M.neighbors(id)
    local out, seen = {}, {}
    local loc = _locations[id]
    if loc and type(loc.connected_to) == "table" then
        for _, c in ipairs(loc.connected_to) do
            if type(c) == "string" and c ~= id and not seen[c] then
                seen[c] = true
                table.insert(out, c)
            end
        end
    end
    for oid, other in pairs(_locations) do
        if oid ~= id and not seen[oid] then
            for _, oc in ipairs(other.connected_to or {}) do
                if oc == id then
                    seen[oid] = true
                    table.insert(out, oid)
                    break
                end
            end
        end
    end
    return out
end

--- Location ids referenced in some connected_to but not generated yet.
-- These are the world's expansion seeds: the LLM may generate them when the
-- player heads there.
-- @param extra_known  Optional table { id → anything } of locations known
--                     outside the world registry (e.g. the adventure's static
--                     LOCATIONS) — these are not pending.
function M.pending_locations(extra_known)
    local pending = {}
    for _, loc in pairs(_locations) do
        for _, c in ipairs(loc.connected_to or {}) do
            if type(c) == "string" and not _locations[c]
               and not (extra_known and extra_known[c]) then
                pending[c] = true
            end
        end
    end
    local out = {}
    for id in pairs(pending) do table.insert(out, id) end
    table.sort(out)
    return out
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
          "requires":    { "type": "string",
                          "description": "OPTIONAL object id the actor must be holding to perform this action (e.g. a key for 'unlock'). Omit for free actions." },
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
    id = _sanitize_id(id); if not id then return nil end
    if _objects[id] then return _objects[id] end

    local sys = "You are an interactable object generator for an RPG. "
             .. "Generate an object with discrete states and actions consistent with the world. "
             .. "Reply ONLY with valid JSON according to the schema.\n\n"
             .. "World: " .. _context
             .. _bible_block()

    local user = "Generate the object with id='" .. id .. "'. "
              .. "Context: " .. (context or id)

    local data = _call_llm(sys, user, _OBJECT_SCHEMA, _validate_object)
    if data and data.name and data.current_state then
        data.id = id
        _objects[id] = data
        _do_save_file()
    end

    return _objects[id]
end

-- ── Object possession (holder) ─────────────────────────────────────────────────
-- ONE canonical field answers "where is this object": obj.holder.
-- Formats: "loc:<location_id>" | "npc:<npc_id>" | "player" | "obj:<object_id>"
-- (obj: = inside a container). nil = scenery, not tracked.
-- Rule of thumb: only objects the main LLM has touched via tools get a
-- holder — do not simulate every spoon.

local function _valid_holder(holder)
    if holder == "player" then return true end
    local kind, ref = tostring(holder or ""):match("^(%w+):(.+)$")
    if not kind then return false, "invalid holder format (use loc:<id> | npc:<id> | player | obj:<id>)" end
    if kind == "loc" or kind == "npc" then return true end   -- ids may live outside this registry
    if kind == "obj" then
        if not _objects[ref] then return false, "container object not found: " .. ref end
        return true
    end
    return false, "unknown holder kind: " .. kind
end

-- Anti-cycle: walking the obj: chain from holder must not reach id.
local function _holder_chain_contains(start_holder, id)
    local cur, hops = start_holder, 0
    while cur and hops < 16 do
        local ref = tostring(cur):match("^obj:(.+)$")
        if not ref then return false end
        if ref == id then return true end
        cur = _objects[ref] and _objects[ref].holder or nil
        hops = hops + 1
    end
    return false
end

--- Set who/where holds an object. Returns {ok=true} or {ok=false, error}.
function M.set_holder(id, holder)
    local obj = _objects[id]
    if not obj then return { ok=false, error="object not found: " .. tostring(id) } end
    if holder == nil then
        obj.holder = nil; _do_save_file(); return { ok=true }
    end
    local ok, err = _valid_holder(holder)
    if not ok then return { ok=false, error=err } end
    if _holder_chain_contains(holder, id) then
        return { ok=false, error="cycle: '" .. id .. "' cannot end up inside itself" }
    end
    obj.holder = holder
    _do_save_file()
    return { ok=true, holder=holder }
end

function M.holder_of(id)
    return _objects[id] and _objects[id].holder or nil
end

--- All object ids held by a holder string (sorted).
function M.objects_held_by(holder)
    local out = {}
    for oid, obj in pairs(_objects) do
        if obj.holder == holder then table.insert(out, oid) end
    end
    table.sort(out)
    return out
end

--- One-line prompt block: what a holder carries. "" if nothing tracked.
-- label esempio: "Marco porta con sé" / "Nella cantina si trovano".
function M.format_held_by(holder, label)
    local ids = M.objects_held_by(holder)
    if #ids == 0 then return "" end
    local names = {}
    for _, oid in ipairs(ids) do
        local o = _objects[oid]
        table.insert(names, (o.name or oid) .. " [" .. oid .. "]")
    end
    return (label or ("Held by " .. holder)) .. ": " .. table.concat(names, ", ")
end

--- ToolDef: move an object between holders (give, take, drop, store).
function M.as_tool_move_object(description)
    return {
        name        = "move_object",
        description = description or
            "Sposta un oggetto tra detentori: il giocatore lo raccoglie, lo cede a "
            .. "un NPC, lo posa in una location o lo mette in un contenitore. Se "
            .. "l'oggetto non esiste viene creato automaticamente: NON serve "
            .. "generate_object prima. SOLO su azione esplicita e fisicamente "
            .. "avvenuta. to: 'player' | 'npc:<id>' | 'loc:<id>' | 'obj:<id contenitore>'.",
        params      = [[{
            "type": "object",
            "required": ["id", "to"],
            "properties": {
                "id": { "type": "string", "description": "Object id (same id used in generate_object)." },
                "to": { "type": "string", "description": "New holder: player | npc:<id> | loc:<id> | obj:<container id>." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local id = _sanitize_id(a.id)
            if not id or not a.to or a.to == "" then
                return json.encode({ error="id and to are required" })
            end
            -- Auto-create a bare object if missing (handing over a note/item that
            -- the narration just introduced): no LLM, no two-step, no loop.
            M.ensure_minimal_object(id)
            local prev = _objects[id].holder
            local res  = M.set_holder(id, a.to)
            if not res.ok then return json.encode(res) end
            return json.encode({ ok=true, id=id, from=prev, to=a.to })
        end,
    }
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

-- States that hide a container's contents from inspection.
local _CLOSED_STATES = { chiuso=true, chiusa=true, closed=true, locked=true,
                         bloccato=true, bloccata=true, sigillato=true, sigillata=true }

--- Apply a named action to an object, transitioning its current_state.
-- @param id      Object id.
-- @param action  Action name (must exist in object.actions).
-- @param actor   Optional holder string performing the action ("player"
--                default, or "npc:<id>") — checked against action.requires.
-- @return        { ok=true, state=new_state } or { ok=false, error="..." }
function M.object_action(id, action, actor)
    local obj = _objects[id]
    if not obj then
        return { ok=false, error="object not found: " .. tostring(id) }
    end
    actor = (actor and actor ~= "") and actor or "player"
    -- Universal read-only actions: return description + state without needing
    -- an actions entry. An object-defined action with the same name takes
    -- precedence (otherwise its state transition would be unreachable).
    local READ_ONLY = { esamina=true, guarda=true, leggi=true, ispeziona=true, osserva=true,
                        examine=true, look=true, read=true, inspect=true }
    local act = obj.actions and obj.actions[action]
    if not act and READ_ONLY[action] then
        local result = {
            ok          = true,
            id          = id,
            name        = obj.name or id,
            description = obj.description or "",
            state       = obj.current_state or "normale",
            data        = obj.data or nil,
        }
        -- Containers reveal their contents only when not closed/locked
        if not _CLOSED_STATES[tostring(obj.current_state or ""):lower()] then
            local inside = M.objects_held_by("obj:" .. id)
            if #inside > 0 then
                local names = {}
                for _, oid in ipairs(inside) do
                    table.insert(names, (_objects[oid].name or oid) .. " [" .. oid .. "]")
                end
                result.contains = names
            end
        end
        return result
    end

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
    -- State gate: action must be valid from the current state.
    local valid = false
    for _, s in ipairs(act.from or {}) do
        if s == obj.current_state then valid = true; break end
    end
    if not valid then
        return { ok=false,
                 error="action '" .. action .. "' not valid from state '" .. (obj.current_state or "?") .. "'" }
    end

    -- Condition gate: flat requires/flag/time (AND) OR alternatives array (OR between sets).
    -- Each condition set: { requires?, flag?, time_from?, time_to? } — all fields AND.
    local function _time_minutes(s)
        local h, m = tostring(s or ""):match("^(%d+):(%d+)$")
        return h and (tonumber(h) * 60 + tonumber(m)) or nil
    end
    local function _check_cond_set(c, world_state)
        if c.requires and c.requires ~= "" then
            if M.holder_of(c.requires) ~= actor then
                local req = _objects[c.requires]
                return false, "serve '" .. (req and req.name or c.requires) .. "' per questa azione."
            end
        end
        if c.flag and c.flag ~= "" then
            local flags = (type(world_state) == "table" and (world_state.flags or world_state.world_flags)) or {}
            if not flags[c.flag] then
                return false, "condizione non soddisfatta: " .. c.flag .. " non attivo."
            end
        end
        if c.time_from or c.time_to then
            local cur = _time_minutes(world_state and (world_state.time or world_state.ora)) or 720
            local from_m = _time_minutes(c.time_from)
            local to_m   = _time_minutes(c.time_to)
            if from_m and to_m then
                local ok_t = (from_m <= to_m) and (cur >= from_m and cur < to_m)
                                               or  (cur >= from_m or  cur < to_m)
                if not ok_t then
                    return false, "orario non valido per questa azione (" .. (c.time_from or "?") .. "-" .. (c.time_to or "?") .. ")."
                end
            end
        end
        return true
    end

    local world_state = _world_ctx

    local alts = act.alternatives
    if alts and type(alts) == "table" and #alts > 0 then
        -- OR: at least one alternative must pass
        local last_err = "nessuna condizione soddisfatta per questa azione."
        local any_ok = false
        for _, cset in ipairs(alts) do
            local ok_c, err_c = _check_cond_set(cset, world_state)
            if ok_c then any_ok = true; break end
            last_err = err_c or last_err
        end
        if not any_ok then
            return { ok=false, error=last_err }
        end
    else
        -- Flat single condition set (backward compat: requires at top level)
        local c = { requires=act.requires, flag=act.flag, time_from=act.time_from, time_to=act.time_to }
        local ok_c, err_c = _check_cond_set(c, world_state)
        if not ok_c then
            return { ok=false, error=err_c or "condizione non soddisfatta.", requires=act.requires }
        end
    end

    if act.to and act.to ~= "" then obj.current_state = act.to end
    _do_save_file()
    return { ok=true, state=obj.current_state }
end

-- ── Direct read/write (no generation) ─────────────────────────────────────────

function M.get_npc(id)            return _npcs[id]           end
function M.get_location(id)       return _locations[id]       end
function M.get_object(id)         return _objects[id]         end

--- Set the current game turn (stamped onto logged events for post-undo
-- reconciliation). Wired automatically by adventure.before_turn().
function M.set_turn(n) _current_turn = tonumber(n) end

--- Pass current world state for action condition evaluation (flags, time).
-- Call from before_ai_turn: world.set_world_state({flags=state.flags, time=state.time})
function M.set_world_state(ctx) _world_ctx = ctx or {} end

function M.set_npc(id, data)      _npcs[id] = data            end
function M.set_location(id, data) _locations[id] = data       end
function M.set_object(id, data)   _objects[id] = data         end

-- Merge-patch a GENERATED location (GUI editor). Whitelisted scalar fields +
-- array replacers; persists atomically. Static locations live in the adventure's
-- LOCATIONS table (not here) and are not patchable through world.
function M.location_patch(id, patch)
    local loc = _locations[id]
    if not loc then return { ok=false, error="location not found: " .. tostring(id) } end
    patch = patch or {}
    for _, k in ipairs({ "name", "description", "owner", "render" }) do
        if patch[k] ~= nil then loc[k] = patch[k] end
    end
    if type(patch.objects_replace)      == "table" then loc.objects      = patch.objects_replace end
    if type(patch.connected_to_replace) == "table" then loc.connected_to = patch.connected_to_replace end
    _do_save_file()
    return { ok=true }
end

-- Create a BARE generated location locally — NO LLM. For the GUI "new location".
function M.location_create(id, data)
    id = _sanitize_id(id); if not id then return { ok=false, error="id non valido" } end
    if _locations[id] then return { ok=false, error="esiste gia': " .. id } end
    data = data or {}
    _locations[id] = {
        name         = data.name        or id,
        description  = data.description or "",
        objects      = data.objects     or {},
        connected_to = data.connected_to or {},
    }
    if data.owner  then _locations[id].owner  = data.owner  end
    if data.render then _locations[id].render = data.render end
    _do_save_file()
    return { ok=true, id=id }
end

-- ── Object editor (GUI) ───────────────────────────────────────────────────────

--- List all world objects for the GUI editor.
-- Returns array of {id, name, location, current_state}.
function M.editor_list_objects()
    local out = {}
    for id, obj in pairs(_objects) do
        out[#out + 1] = {
            id            = id,
            name          = obj.name or id,
            location      = obj.holder or "?",
            current_state = obj.current_state or "normale",
        }
    end
    table.sort(out, function(a, b) return a.id < b.id end)
    return out
end

--- Full object record for GUI editor.
function M.editor_object_get(id)
    local obj = _objects[id]
    if not obj then return { ok=false, error="not found: " .. tostring(id) } end
    return { ok=true, id=id, data=obj }
end

--- Patch an object (GUI editor). Editable fields:
--   name, description, current_state, holder
--   actions_replace: full replacement of actions table
--   states_replace:  full replacement of states list
function M.object_patch(id, patch)
    local obj = _objects[id]
    if not obj then return { ok=false, error="object not found: " .. tostring(id) } end
    patch = patch or {}
    for _, k in ipairs({ "name", "description", "current_state", "holder" }) do
        if patch[k] ~= nil then obj[k] = patch[k] end
    end
    if type(patch.actions_replace) == "table" then obj.actions = patch.actions_replace end
    if type(patch.states_replace)  == "table" then obj.states  = patch.states_replace  end
    _do_save_file()
    return { ok=true }
end

--- Create a new bare object via GUI editor (no LLM).
function M.object_create(id, data)
    id = _sanitize_id(id); if not id then return { ok=false, error="id non valido" } end
    if _objects[id] then return { ok=false, error="esiste gia': " .. id } end
    data = data or {}
    local states = (type(data.states) == "table" and data.states) or { "normale" }
    _objects[id] = {
        id            = id,
        name          = data.name        or id:gsub("_", " "),
        description   = data.description or "",
        current_state = data.current_state or states[1] or "normale",
        states        = states,
        holder        = data.holder or nil,
        actions       = data.actions or {},
    }
    _do_save_file()
    return { ok=true, id=id }
end

-- Create a BARE object locally if missing — NO LLM call. For tools that only
-- need a container (object_write sets a data field, move_object sets a holder):
-- a note, a register, a handed item. Cheap and loop-proof, unlike object_action
-- which needs a real state machine and so must NOT be auto-created this way.
function M.ensure_minimal_object(id)
    id = _sanitize_id(id); if not id then return nil end
    if not _objects[id] then
        _objects[id] = { id = id, name = id:gsub("_", " "),
                         states = { "normale" }, current_state = "normale",
                         actions = {}, data = {} }
    end
    return _objects[id]
end

-- Flag the id the player tried to reach but doesn't exist, so a blind
-- generate_location({}) right after can recover it. Called by adventure.move_player.
function M.note_missing_location(id) _last_missing_loc = _sanitize_id(id) end

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
    if obj.holder then base = base .. " (held by: " .. obj.holder .. ")" end
    do
        local closed = { chiuso=true, chiusa=true, closed=true, locked=true,
                         bloccato=true, bloccata=true, sigillato=true, sigillata=true }
        if not closed[tostring(obj.current_state or ""):lower()] then
            local inside = M.objects_held_by("obj:" .. id)
            if #inside > 0 then
                local names = {}
                for _, oid in ipairs(inside) do
                    table.insert(names, (_objects[oid].name or oid) .. " [" .. oid .. "]")
                end
                base = base .. "\n  Contiene: " .. table.concat(names, ", ")
            end
        end
    end
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
                        "additionalProperties": { "type": "object", "additionalProperties": { "type": "number" } } },
    "relationship_updates": {
      "type": "array",
      "description": "How this interaction changed what one NPC thinks of the other. Use ONLY when the interaction meaningfully shifts the relationship.",
      "items": {
        "type": "object",
        "required": ["from", "to", "description"],
        "properties": {
          "from":        { "type": "string", "description": "npc_id whose view changed" },
          "to":          { "type": "string", "description": "npc_id the view is about" },
          "description": { "type": "string", "description": "New one-line relationship description from 'from' towards 'to'." }
        }
      }
    },
    "shared_fact": {
      "type": "object",
      "description": "OPTIONAL. A piece of information one NPC told the other during the interaction (gossip).",
      "properties": {
        "from": { "type": "string", "description": "npc_id who told it" },
        "to":   { "type": "string", "description": "npc_id who learned it" },
        "fact": { "type": "string", "description": "The fact, one sentence." }
      }
    }
  }
}]]

-- ── Colocation cooldown ────────────────────────────────────────────────────────

local _colo_last = {}   -- "loc|id1+id2" -> absolute minute of last ambient event

-- Called by M.init (defined earlier in the file, so it goes through M).
function M.reset_colocation()
    _colo_last = {}
end

local function _abs_minutes(day_index, time_str)
    local h, m = (time_str or "00:00"):match("(%d+):(%d+)")
    return ((tonumber(day_index) or 1) * 1440)
         + ((tonumber(h) or 0) * 60) + (tonumber(m) or 0)
end

--- Like check_colocation, but rate-limited: groups whose (location, npc set)
-- fired an ambient event within cooldown_min game-minutes are filtered out,
-- and the returned groups are immediately marked as fired. Without this,
-- the same pair in the same kitchen triggers an ambient event EVERY tick.
-- @param npc_location_map  { npc_id → location_id }.
-- @param time_str          Current in-game time "HH:MM".
-- @param day_index         Integer day counter.
-- @param opts              Optional { min_npcs=2, cooldown_min=180 }.
-- @return Array of { location_id, npc_ids[] } due for an ambient event.
function M.check_colocation_due(npc_location_map, time_str, day_index, opts)
    opts = opts or {}
    local cooldown = opts.cooldown_min or 180
    local groups   = M.check_colocation(npc_location_map, opts.min_npcs)
    local now      = _abs_minutes(day_index, time_str)
    local due = {}
    for _, g in ipairs(groups) do
        local ids = {}
        for _, id in ipairs(g.npc_ids) do table.insert(ids, id) end
        table.sort(ids)
        local key  = g.location_id .. "|" .. table.concat(ids, "+")
        local last = _colo_last[key]
        if not last or (now - last) >= cooldown or now < last then
            _colo_last[key] = now
            table.insert(due, g)
        end
    end
    return due
end

--- Apply the result of an ambient event LLM call (AMBIENT_SCHEMA) to the
-- world: life events + relationship updates + gossip into persona files,
-- stat deltas onto live npc objects, event into the location log.
-- All sub-applications are best-effort: missing personas/objects are skipped.
--
-- @param result   Decoded AMBIENT_SCHEMA table.
-- @param npc_ids  Array of participant npc_ids (order = a, b of the schema).
-- @param env      Optional {
--                   persona     = persona module (for life events/relationships/gossip),
--                   npc_objects = { npc_id → npc.lua object } (for stat deltas),
--                   location_id = where it happened (for the event log),
--                   time        = "HH:MM",
--                   date        = date label for life events (e.g. "day 3"),
--                 }
-- @return Table with applied counters.
function M.apply_ambient_result(result, npc_ids, env)
    if type(result) ~= "table" then return { applied=false } end
    env = env or {}
    local persona_lib = env.persona
    local date = env.date or "?"
    local applied = { life_events=0, relationships=0, stats=0, facts=0 }

    local function has_persona(id)
        return persona_lib and persona_lib.get and id and persona_lib.get(id)
    end

    local function add_life_event(id, ev)
        if not (ev and ev ~= "" and has_persona(id)) then return end
        persona_lib.patch(id, { date=date, event=ev })
        applied.life_events = applied.life_events + 1
    end
    add_life_event(npc_ids and npc_ids[1], result.life_event_a)
    add_life_event(npc_ids and npc_ids[2], result.life_event_b)

    for _, ru in ipairs(result.relationship_updates or {}) do
        if type(ru) == "table" and ru.from and ru.to
           and type(ru.description) == "string" and ru.description ~= ""
           and has_persona(ru.from) then
            persona_lib.patch(ru.from, {
                date = date,
                relationships_patch = { [ru.to] = ru.description },
            })
            applied.relationships = applied.relationships + 1
        end
    end

    local sf = result.shared_fact
    if type(sf) == "table" and sf.fact and sf.fact ~= ""
       and has_persona(sf.to)
       and persona_lib.add_known_fact then
        local src_name = (has_persona(sf.from)
            and (persona_lib.get(sf.from).name or sf.from)) or (sf.from or "?")
        if persona_lib.add_known_fact(sf.to, sf.fact,
                "sentito da " .. src_name, date) then
            applied.facts = applied.facts + 1
        end
    end

    for npc_id, deltas in pairs(result.stat_delta or {}) do
        local obj = env.npc_objects and env.npc_objects[npc_id]
        if obj and obj.stats and type(deltas) == "table" then
            for stat, dv in pairs(deltas) do
                if type(dv) == "number" and obj.stats[stat] ~= nil then
                    obj.stats[stat] = math.max(0, math.min(1, obj.stats[stat] + dv))
                    applied.stats = applied.stats + 1
                end
            end
        end
    end

    if env.location_id and type(result.event_summary) == "string"
       and result.event_summary ~= "" then
        M.log_event(env.location_id, env.time, result.event_summary, npc_ids)
    end

    return applied
end

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
             .. _bible_block()

    local npc_lines = {}
    for _, npc_id in ipairs(npc_ids) do
        if persona_lib and persona_lib.format then
            local block = "[" .. npc_id .. "]\n" .. persona_lib.format(npc_id)
            -- Ground the interaction in what each NPC actually knows
            if persona_lib.known_facts then
                local kf = persona_lib.known_facts(npc_id)
                if #kf > 0 then
                    local fl = {}
                    for i = math.max(1, #kf - 4), #kf do
                        table.insert(fl, kf[i].fact)
                    end
                    block = block .. "\nKnows: " .. table.concat(fl, "; ")
                end
            end
            table.insert(npc_lines, block)
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

--- Return a list of location ids DIRECTLY reachable from loc_id (1 hop only —
-- no transitive/BFS reachability; for multi-hop use the adventure.lua BFS).
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
        colo_last  = _colo_last,
        bible      = _bible,
        claims     = _claims,
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
    if s.colo_last  then _colo_last  = s.colo_last  end
    if s.bible      then _bible      = s.bible      end
    if s.claims     then _claims     = s.claims     end
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
            .. "OBBLIGATORI 'id' (snake_case) e 'context' — MAI chiamare senza argomenti. "
            .. "Esempio: generate_location(id=\"bagno_comune_p1\", "
            .. "context=\"bagno condiviso al primo piano, vasca con piedini, finestra alta\", "
            .. "from=\"corridoio_p1\"). "
            .. "Returns location data: name, description, objects, exits.",
        params      = [[{
            "type": "object",
            "required": ["id", "context"],
            "properties": {
                "id":      { "type": "string",
                             "description": "Unique snake_case location id (e.g. 'apt_203_salotto')." },
                "context": { "type": "string",
                             "description": "What this place should be: type, owner, atmosphere, "
                                         .. "adjacent rooms, objects expected inside." },
                "from":    { "type": "string",
                             "description": "Location id the player is coming from — a way back is guaranteed." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local id = _sanitize_id(a.id)
            -- Recover the intended id if the model called generate_location({})
            -- blind right after move_player flagged a missing location.
            if not id and _last_missing_loc then id = _last_missing_loc end
            if not id then
                return json.encode({ error="id e context OBBLIGATORI. Esempio: "
                    .. "generate_location(id=\"bagno_comune_p1\", context=\"bagno condiviso, "
                    .. "vasca, finestra alta\", from=\"corridoio_p1\"). Riprova completo." })
            end
            if _locations[id] then
                _last_missing_loc = nil
                return json.encode({ ok=true, already_exists=true,
                                     id=id, summary=M.format_location(id) })
            end
            local loc = M.ensure_location(id, a.context or id, { from = _sanitize_id(a.from) })
            if loc then _last_missing_loc = nil end
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
            "RARAMENTE necessario: per interagire con un oggetto usa direttamente "
            .. "object_action(id, action), che lo crea da solo se nuovo. Usa "
            .. "generate_object SOLO per pre-costruire un oggetto con stati/azioni "
            .. "specifici PRIMA di narrarlo. Se lo usi, 'id' (snake_case) e 'context' "
            .. "sono OBBLIGATORI — MAI senza argomenti. Esempio: "
            .. "generate_object(id=\"moppo_secchio\", context=\"set pulizie, "
            .. "stato bagnato/asciutto, azioni: usa, strizza\").",
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
            local id = _sanitize_id(a.id)
            -- Weak models often call generate_object({}) with no id. If a recent
            -- object_action named a missing object, recover that intended id so
            -- the call succeeds instead of looping on "id is required".
            if not id and _last_missing_obj then id = _last_missing_obj end
            if not id then
                return json.encode({ error="id e context OBBLIGATORI. Esempio: "
                    .. "generate_object(id=\"specchio_bagno\", context=\"specchio a parete, "
                    .. "stato pulito/appannato\"). Riprova con argomenti completi." })
            end
            if _objects[id] then
                _last_missing_obj = nil
                return json.encode({ ok=true, already_exists=true,
                                     id=id, summary=M.format_object(id) })
            end
            local obj = M.ensure_object(id, a.context or id)
            if obj then _last_missing_obj = nil end
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
            "Agisci su un oggetto STATEFUL già esistente (porta apri/chiudi, "
            .. "contenitore, interruttore, serratura). NON per scenografia o cose "
            .. "banali: quelle NARRALE direttamente senza tool. L'azione deve "
            .. "essere una transizione valida dallo stato attuale (NON una frase "
            .. "libera). Per spostare/possedere un oggetto usa move_object, non "
            .. "object_action. Esempio: object_action(id=\"porta_cucina\", action=\"chiudi\").",
        params      = [[{
            "type": "object",
            "required": ["id", "action"],
            "properties": {
                "id":     { "type": "string",
                            "description": "Object identifier (same id used in generate_object)." },
                "action": { "type": "string",
                            "description": "Action name. Must be valid from the object's current state." },
                "actor":  { "type": "string",
                            "description": "Who performs it: 'player' (default) or 'npc:<id>'. Matters for actions that require holding an object (e.g. a key)." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local id = _sanitize_id(a.id)
            if not id then return json.encode({ error="id is required" }) end
            -- NON auto-generare: farlo a ogni id inventato è lento (una chiamata
            -- LLM per id) e riempie il mondo di oggetti-spazzatura. object_action
            -- è SOLO per oggetti STATEFUL (porte, contenitori, serrature). Per la
            -- scenografia (ghiaccio, furgone, un mucchio di sacchi) NON serve un
            -- oggetto: il narratore la descrive direttamente.
            if not _objects[id] then
                return json.encode({
                    error = "Oggetto '" .. id .. "' non tracciato. NON creare un oggetto "
                        .. "per scenografia o cose banali: NARRA l'azione direttamente, "
                        .. "senza tool. Usa generate_object SOLO se è un oggetto con STATI "
                        .. "che cambiano e contano (porta che si apre/chiude, contenitore, "
                        .. "serratura): generate_object(id=\"" .. id .. "\", context=\"...\").",
                    not_tracked = id,
                })
            end
            return json.encode(M.object_action(id, a.action or "", a.actor))
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
    local ev = { time=time_str or "?", text=text or "", turn=_current_turn }
    if npcs and #npcs > 0 then ev.npcs = npcs end
    table.insert(_events[location_id], ev)
    local arr = _events[location_id]
    while #arr > MAX_EVENTS_PER_LOC do table.remove(arr, 1) end
    _do_save_file()
end

--- Post-undo reconciliation: drop events recorded at a turn later than
-- current_turn (undo rewinds the save, not the world file/registry).
-- Entries without a turn stamp are kept.
-- @return number of events removed.
function M.prune_future_events(current_turn)
    current_turn = tonumber(current_turn)
    if not current_turn then return 0 end
    local removed = 0
    for _, arr in pairs(_events) do
        for i = #arr, 1, -1 do
            local t = tonumber(arr[i].turn)
            if t and t > current_turn then
                table.remove(arr, i)
                removed = removed + 1
            end
        end
    end
    if removed > 0 then _do_save_file() end
    return removed
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
            local loc = _sanitize_id(a.location_id)
            if not loc then
                return json.encode({ error="location_id required" })
            end
            M.log_event(loc, a.time, a.text, a.npcs)
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
            "Scrivi/aggiungi contenuto al campo dati di un oggetto (biglietto, "
            .. "registro, bacheca, lista, contenuto). Se l'oggetto non esiste viene "
            .. "creato automaticamente: NON serve generate_object prima. Esempio: "
            .. "object_write(id=\"biglietto_lucia\", field=\"testo\", set=\"...\"). "
            .. "Usa 'append' per aggiungere voci, 'remove_index' per toglierne una.",
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
            local id    = _sanitize_id(a.id)
            local field = a.field or ""
            if not id or field == "" then
                return json.encode({ error="id and field are required" })
            end
            -- Auto-create a bare container if missing (a note, a register, a list
            -- the narration just introduced): no LLM, no two-step, no loop.
            local obj = M.ensure_minimal_object(id)
            obj.data = obj.data or {}

            if a.set ~= nil then
                obj.data[field] = a.set
                _do_save_file()
                return json.encode({ ok=true, action="set", field=field })
            end

            if a.remove_index then
                local arr = obj.data[field]
                if type(arr) ~= "table" or a.remove_index > #arr then
                    return json.encode({ ok=false, error="index out of range" })
                end
                table.remove(arr, a.remove_index)
                _do_save_file()
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
