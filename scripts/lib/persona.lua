-- scripts/lib/persona.lua
-- File-backed NPC personas for RpgAi.
--
-- Each NPC is a .lua file in a dedicated directory — their complete life record:
-- identity, history, current conditions, daily routine, npc.lua config,
-- agent.lua system prompt. The file IS the ground truth. No NPC data lives in
-- the save JSON; on restore, files are reloaded from disk.
--
-- QUICK START:
--
--   local persona = require("lib/persona")
--
--   -- Once at startup (before restore or NPC creation):
--   persona.init("./scripts/npcs/", "An apartment building in Messina, 1990s.")
--
--   -- In restore_state() — reload all NPC files from disk:
--   persona.reload_all()
--
--   -- In get_tools() — expose generation and life-event tools to main LLM:
--   return tools.build({
--       persona.as_tool_generate("Generate a new character the player is encountering."),
--       persona.as_tool_life_event("Record an important life event for an NPC."),
--   })
--
--   -- After get_tools() generated an NPC, create npc.lua + agent.lua objects:
--   local npc_obj   = persona.npc_object("marco_203", world_adapter)
--   local agent_obj = persona.agent_object("marco_203", { npc=npc_obj, turn_counter=tc })
--
--   -- Formatting for system prompt injection:
--   persona.format("marco_203")          -- identity + conditions + goals
--   persona.format_routine("marco_203")  -- daily schedule
--   persona.format_history("marco_203")  -- last N life events
--
--   -- Direct data access:
--   persona.get("marco_203")   → data table
--   persona.known_ids()        → sorted array of known ids
--
-- NPC PERSONA FILE FORMAT  (npcs/marco_203.lua):
--
--   return {
--     id          = "marco_203",
--     name        = "Marco Ferretti",
--     age         = 43,
--     job         = "history teacher",
--     personality = "reserved, thoughtful, kind...",
--
--     life_events = { { date="day 1", event="met by the player" }, ... },
--
--     relationships = { wife="Sofia Ferretti", daughter="Emma (8 years old)" },
--     conditions    = {},    -- e.g. { "broken leg", "hospitalised" }
--
--     routine = {
--       { time_from="07:00", time_to="08:15", location="cucina_203",  activity="has breakfast" },
--       { time_from="08:15", time_to="14:00", location="fuori",       activity="teaches" },
--       { time_from="22:00", time_to="07:00", location="camera_203",  activity="sleeps" },
--     },
--     stats_defaults  = { stress=0.3, loneliness=0.2 },
--     event_reactions = {
--       rumore_forte = { activity="looks up", narrative_hint="Marco glances upward." },
--     },
--
--     agent_system = "You are Marco Ferretti, 43 years old...",
--
--     short_term_goals = {},
--     long_term_goals  = { "protect the family" },
--   }

local json     = require("json")
local llm_util = require("lib/llm_util")
local wlog     = require("lib/log")

local M = {}

-- ── Registry ───────────────────────────────────────────────────────────────────

local _npcs      = {}    -- id -> data table (in-memory mirror of .lua files)
local _base_path = "./scripts/npcs/"
local _context   = ""
local _model     = nil
local _provider  = nil

-- Growth caps (configurable via persona.init opts). Since the dream
-- redesign (see memory project_dream_system_redesign) M.dream() no longer
-- generates any of this — structure is earned from real occurrences during
-- play instead: _max_needs and _max_event_reactions are read by
-- M.check_pending_needs / M._track_unhandled_event (stage 2a/2b).
-- _max_sequences is still INERT — sequences are bounded indirectly (always
-- paired with a need or event_reaction). _max_stats now caps PERSONAL stats
-- per NPC (stage 4, see _common_stats below).
local _max_sequences       = 5
local _max_needs           = 3
local _max_event_reactions = 6
local _max_stats           = 4

-- Stage 4: stats every NPC gets, deterministically, regardless of
-- generation quality — a fixed, genre-agnostic baseline (override via
-- persona.init opts.common_stats). Distinct from PERSONAL stats
-- (character-specific — "gelosia" for a jealous character, "avidità" for a
-- merchant), which are still LLM-generated per NPC and get their own
-- reliability net (_generation_looks_valid + M._backfill_personal_stats):
-- generation is NOT assumed infallible — a mid-tier model has been observed
-- this session skipping/emptying fields it was technically allowed to
-- (sparse routines, empty dicts satisfy a bare "required" JSON Schema key).
local _common_stats = { energy = 0.7, mood = 0.6, stress = 0.3 }

local _default_template    = nil   -- set via opts.default_template in M.init()
local _critique            = false -- semantic review pass on generation
local _critique_model      = nil
local _critique_provider   = nil
local _variety_pool        = nil   -- template rotation pool

-- Session-level guard used by dream_tick when the caller passes last_dream=nil.
local _last_dream_fallback = {}

-- Current game turn (set by adventure.before_turn). Stamped onto life_events
-- and known_facts so that, after an undo, stale "future" entries can be
-- detected and pruned (file-backed data does not rewind with the save state).
local _current_turn = nil

function M.set_turn(n) _current_turn = tonumber(n) end

-- ── Init ───────────────────────────────────────────────────────────────────────

--- Initialize the persona system.
-- @param base_path  Directory where NPC .lua files are stored.
-- @param context    World description for coherent LLM generation (1-3 sentences).
-- @param opts       Optional { model="...", provider="..." } for generation calls.
function M.init(base_path, context, opts)
    _base_path = base_path or "./scripts/npcs/"
    -- Ensure trailing slash
    if _base_path:sub(-1) ~= "/" then _base_path = _base_path .. "/" end
    _context  = context or ""
    opts      = opts or {}
    _model    = opts.model    or nil
    _provider = opts.provider or nil
    _max_sequences       = opts.max_sequences       or _max_sequences
    _max_needs           = opts.max_needs           or _max_needs
    _max_event_reactions = opts.max_event_reactions or _max_event_reactions
    _max_stats           = opts.max_stats           or _max_stats
    _default_template    = opts.default_template    or nil
    -- Semantic critique: extra LLM review pass on generated NPCs. Shape is
    -- already validated structurally; this judges SOUL — variety, narrative
    -- tension of the secret, distinctive voice. One call per generated
    -- entity, amortized over its whole life. A failed review re-prompts the
    -- generator with the reviewer's reason (via validated_call retries).
    _critique            = opts.critique or false
    _critique_model      = opts.critique_model    or nil
    _critique_provider   = opts.critique_provider or nil
    -- Variety quota: when generate() gets no explicit template, rotate
    -- through this pool picking the LEAST represented template so the cast
    -- stays varied by data, not by prompt exhortation.
    -- E.g. variety_pool = { "worker", "student", "pensioner" }
    _variety_pool        = opts.variety_pool or nil
    -- Stage 4: stats every NPC gets regardless of generation quality (see
    -- _backfill_common_stats) — distinct from PERSONAL stats (character-
    -- specific, still LLM-generated, see _generation_looks_valid +
    -- M._backfill_personal_stats). Override per-adventure, e.g. for a genre
    -- where "energy/mood/stress" doesn't fit.
    if opts.common_stats then _common_stats = opts.common_stats end
    os.execute('mkdir -p "' .. _base_path .. '"')
    -- Expose as a Lua global so eval_lua and get_game_state can discover it
    -- without having to read the adventure script or parse persona module locals.
    _PERSONA_BASE_PATH = _base_path
end

-- ── Lua table serializer ───────────────────────────────────────────────────────
-- Converts a data table to a Lua source string.
-- Supports: nil, boolean, number, string, nested tables.
-- Output is deterministic (keys sorted alphabetically).

local function _serialize(val, indent)
    indent = indent or ""
    -- Depth cap: a cyclic or absurdly deep table (LLM-injected or accidental
    -- self-reference) must not stack-overflow the writer.
    if #indent > 48 then return string.format("%q", "[max depth]") end
    local t = type(val)
    if     t == "nil"     then return "nil"
    elseif t == "boolean" then return tostring(val)
    elseif t == "number"  then return tostring(val)
    elseif t == "string"  then return string.format("%q", val)
    elseif t == "table"   then
        -- Detect array: contiguous integer keys starting at 1
        local count = 0
        local is_array = true
        for k in pairs(val) do
            count = count + 1
            if type(k) ~= "number" or k ~= math.floor(k) or k < 1 then
                is_array = false; break
            end
        end
        if is_array and count > 0 then
            for i = 1, count do
                if val[i] == nil then is_array = false; break end
            end
        end
        if count == 0 then return "{}" end

        local inner = indent .. "    "
        local parts = {}

        if is_array then
            for _, v in ipairs(val) do
                table.insert(parts, inner .. _serialize(v, inner))
            end
        else
            local keys = {}
            for k in pairs(val) do table.insert(keys, k) end
            table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
            for _, k in ipairs(keys) do
                local key_str = (type(k) == "string" and k:match("^[%a_][%w_]*$"))
                    and k or ("[" .. string.format("%q", tostring(k)) .. "]")
                table.insert(parts, inner .. key_str .. " = " .. _serialize(val[k], inner))
            end
        end
        return "{\n" .. table.concat(parts, ",\n") .. ",\n" .. indent .. "}"
    else
        return string.format("%q", "[unserializable:" .. t .. "]")
    end
end

-- ── File I/O ───────────────────────────────────────────────────────────────────

-- Logical field order for human-readable output
local _FIELD_ORDER = {
    "id", "template", "name", "age", "job", "home", "workplace",
    "appearance", "personality", "secret",
    "life_events", "known_facts",
    "relationships", "family", "conditions", "carrying",
    "vn_verbs", "topics",
    "routine", "stats_defaults", "event_reactions",
    "agent_system", "npc_summary", "outfit_override", "short_term_goals", "long_term_goals",
    -- Template-provided base (set at generation time, stable across sessions):
    "state_phrases",
    -- Dream-grown behavioral additions (accumulated nightly):
    "dream_count", "npc_stats", "npc_sequences", "npc_needs", "npc_event_reactions",
    "dream_log",
}

local function _write(data)
    local path = _base_path .. data.id .. ".lua"
    -- Atomic: temp file + rename. A crash mid-write must never truncate the
    -- persona file — it is the NPC's entire life record.
    local tmp  = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false end

    f:write("-- " .. data.id .. ".lua — NPC persona file (auto-generated by RpgAi)\n")
    f:write("-- Last updated: " .. os.date("%Y-%m-%d %H:%M") .. "\n")
    f:write("-- Use the npc_life_event tool to apply changes. Do not edit manually.\n")
    -- Dream log as human-readable comments
    if data.dream_log and #data.dream_log > 0 then
        f:write("--\n-- DREAM EVOLUTION:\n")
        for _, entry in ipairs(data.dream_log) do
            local line = "--   [" .. (entry.date or "?") .. "] " .. (entry.aspect or "dream")
            if entry.addition_type and entry.addition_name then
                line = line .. " → " .. entry.addition_type .. ":" .. entry.addition_name
            end
            if entry.narrative then
                line = line .. "\n--     \"" .. entry.narrative:gsub("\n", " ") .. "\""
            end
            f:write(line .. "\n")
        end
        f:write("--\n")
    end
    f:write("\nreturn {\n\n")

    local written = {}
    for _, k in ipairs(_FIELD_ORDER) do
        if data[k] ~= nil then
            written[k] = true
            f:write("    " .. k .. " = " .. _serialize(data[k], "    ") .. ",\n\n")
        end
    end
    -- Any extra fields not in the order list
    local extra = {}
    for k in pairs(data) do
        if not written[k] then table.insert(extra, k) end
    end
    table.sort(extra)
    for _, k in ipairs(extra) do
        f:write("    " .. k .. " = " .. _serialize(data[k], "    ") .. ",\n\n")
    end

    f:write("}\n")
    f:close()
    -- Syntax-check before replacing the live file: load the temp file and
    -- verify it parses as valid Lua. An LLM-injected bad string (e.g. s*"...")
    -- gets caught here instead of corrupting the NPC record.
    local chunk, syn_err = loadfile(tmp)
    if not chunk then
        os.remove(tmp)
        _log_err("[" .. (data.id or "?") .. "] _write aborted — generated Lua is invalid: " .. tostring(syn_err))
        return false
    end
    local ok = os.rename(tmp, path)
    if not ok then os.remove(tmp); return false end
    return true
end

local function _log_err(msg)
    pcall(wlog.warn, "persona", msg)
    pcall(function()
        local f = io.open("/tmp/persona_load_error.log", "a")
        if f then
            f:write(os.date("%Y-%m-%d %H:%M:%S") .. " " .. msg .. "\n")
            f:close()
        end
    end)
end

local function _file_exists(path)
    local f = io.open(path, "r")
    if f then f:close(); return true end
    return false
end

-- Sanitize an LLM-provided NPC id: ids are filenames (npcs/<id>.lua) AND table
-- keys, so they must be plain snake_case ASCII. Strips stray Unicode an LLM may
-- append (the same failure that produced "ingresso埋" in world.lua). nil if empty.
local function _sanitize_id(id)
    if type(id) ~= "string" then return nil end
    id = id:lower():gsub("[^%w_]", "_"):gsub("_+", "_"):gsub("^_", ""):gsub("_$", "")
    if id == "" then return nil end
    return id
end
M.sanitize_id = _sanitize_id

-- Returns (data, err). err ~= nil means the file EXISTS but is unreadable
-- (syntax error, runtime error, invalid structure) — callers must not treat
-- this like "file missing" or they will overwrite accumulated history.
local function _load_file(id)
    local path = _base_path .. id .. ".lua"
    if not _file_exists(path) then return nil, nil end
    local chunk, lerr = loadfile(path)
    if not chunk then
        _log_err("[" .. id .. "] syntax error: " .. tostring(lerr))
        return nil, "syntax error: " .. tostring(lerr)
    end
    local ok2, data = pcall(chunk)
    if not ok2 then
        _log_err("[" .. id .. "] runtime error: " .. tostring(data))
        return nil, "runtime error: " .. tostring(data)
    end
    if type(data) ~= "table" or not data.id then
        _log_err("[" .. id .. "] invalid structure (not a table with id)")
        return nil, "invalid structure"
    end
    return data, nil
end

-- Move a corrupt persona file aside (timestamped .broken backup) so it can be
-- recovered manually instead of being silently overwritten.
local function _backup_corrupt(id, reason)
    local path   = _base_path .. id .. ".lua"
    local backup = path .. ".broken-" .. os.date("%Y%m%d%H%M%S")
    pcall(os.rename, path, backup)
    _log_err("[" .. id .. "] corrupt file (" .. tostring(reason)
        .. ") backed up to " .. backup)
end

-- Deterministic, no LLM call — applied at every point persona data enters
-- _npcs (M.load, register_static's both branches, M.generate) so a baseline
-- always exists no matter how the file got there (hand-authored, generated,
-- old file predating this feature). Never overwrites an existing value —
-- an author or the generator may have deliberately set a different number
-- for a common name (e.g. an exhausted NPC starting at low energy).
local function _backfill_common_stats(data)
    if not data then return end
    data.stats_defaults = data.stats_defaults or {}
    for k, v in pairs(_common_stats) do
        if data.stats_defaults[k] == nil then data.stats_defaults[k] = v end
    end
end

-- ── File loading ───────────────────────────────────────────────────────────────

--- Load a single NPC file into the registry.
-- @param id  NPC identifier (= filename without .lua).
-- @return    Data table or nil.
function M.load(id)
    local data = _load_file(id)
    if data then
        _backfill_common_stats(data)
        _npcs[data.id] = data
    end
    return data
end

--- Reload all .lua files from the base directory into the registry.
-- Call from restore_state() — persona files are the authoritative source,
-- not the save JSON.
function M.reload_all()
    _npcs = {}
    local ok, handle = pcall(io.popen,
        'ls -1 "' .. _base_path .. '" 2>/dev/null')
    if not ok or not handle then return end
    for line in handle:lines() do
        local id = line:match("^(.+)%.lua$")
        if id then M.load(id) end
    end
    handle:close()
end

--- Register a master-defined NPC without calling the LLM.
--
-- Behaviour:
--   1. If already in memory (_npcs[id]) → return as-is (reload_all ran first).
--   2. If .lua file exists on disk        → load from disk, ignore config.
--   3. Otherwise                          → fill defaults from config, write file.
--
-- This makes the function idempotent: calling it every set_initial_state is safe.
-- The .lua file on disk is the authoritative source once written — life_events
-- and dream_log accumulated in-game are preserved across sessions.
--
-- @param id      NPC identifier (snake_case, stable).
-- @param config  Table with NPC fields. Required: name, personality, agent_system.
--                All other fields optional — sensible defaults applied.
-- @return        Data table (same as persona.get(id)).
function M.register_static(id, config)
    -- 1. Already in memory
    if _npcs[id] then return _npcs[id] end

    -- 2. File exists on disk
    local existing, load_err = _load_file(id)
    if existing then
        _backfill_common_stats(existing)
        _npcs[id] = existing
        return existing
    end
    -- File exists but is corrupt: back it up before rebuilding from config,
    -- so accumulated life_events/dream_log can be recovered manually.
    if load_err then
        _backup_corrupt(id, load_err)
    end

    -- 3. Build from config and write
    local data = {
        id              = id,
        name            = config.name            or id,
        age             = config.age             or 30,
        job             = config.job             or "sconosciuto",
        home            = config.home            or "sconosciuto",
        workplace       = config.workplace       or "sconosciuto",
        appearance      = config.appearance      or "",
        outfit_override = config.outfit_override,
        personality     = config.personality     or "",
        secret          = config.secret          or "",
        life_events     = config.life_events     or {{ date="start", event="character defined by master" }},
        relationships   = config.relationships   or {},
        family          = config.family          or {},
        conditions      = config.conditions      or {},
        routine         = config.routine         or {},
        stats_defaults  = config.stats_defaults  or {},
        event_reactions = config.event_reactions or {},
        agent_system    = config.agent_system    or "",
        npc_summary     = config.npc_summary     or config.personality or "",
        short_term_goals = config.short_term_goals or {},
        long_term_goals  = config.long_term_goals  or {},
        state_phrases    = config.state_phrases    or {},
        is_static        = true,   -- marks master-defined NPCs
        dream_count      = 0,
        npc_stats        = {},
        npc_sequences    = config.npc_sequences  or {},
        npc_needs        = {},
        npc_event_reactions = {},
        dream_log        = {},
    }
    _backfill_common_stats(data)
    if not _write(data) then
        _log_err("[" .. id .. "] could not write persona file — "
            .. "NPC lives in memory only this session")
    end
    _npcs[id] = data
    return data
end

-- ── LLM generation ─────────────────────────────────────────────────────────────

local _SCHEMA = [[{
  "type": "object",
  "required": ["id","name","age","job","home","workplace","appearance","personality","secret","relationships","family","routine",
               "stats_defaults","event_reactions","agent_system",
               "short_term_goals","long_term_goals"],
  "properties": {
    "id":           { "type": "string" },
    "name":         { "type": "string" },
    "age":          { "type": "integer", "minimum": 5, "maximum": 99 },
    "job":          { "type": "string" },
    "home":         { "type": "string", "description": "Apartment identifier and floor, e.g. 'Appartamento 202, secondo piano'. Used to locate the NPC in the building." },
    "workplace":    { "type": "string", "description": "Where this NPC works or spends most daytime hours outside the building. E.g. 'Manzoni middle school, via Roma 12' or 'retired, does not work'." },
    "appearance":   { "type": "string", "description": "Physical description (the BODY, not clothes): build, height, hair, face, distinctive features. 1-2 sentences, concrete and specific. E.g. 'Minuto e nervoso, capelli grigi radi, occhiali spessi, mani sempre in movimento.' Used for narration and image generation; the outfit (clothes) is separate per routine slot." },
    "personality":  { "type": "string", "description": "2-4 sentences: traits, speech style, notable quirks" },
    "secret":       { "type": "string", "description": "A private fact the NPC never reveals spontaneously. Must have some spice: a clandestine affair, an obsession, a vice, an unconfessable desire, a structural lie. Prefer secrets that create romantic or social tension. E.g. 'Has had an affair with the fourth-floor neighbour's husband for two years.' or 'Pretends to sleep when she hears night noises from 203 because she likes them.' or 'Filed a false report against their ex.'" },
    "relationships":{ "type": "object", "additionalProperties": { "type": "string" } },
    "family": {
      "type": "array",
      "description": "Stable list of family members. Used to keep the LLM consistent across sessions when the player asks about family.",
      "items": {
        "type": "object",
        "required": ["name","relation"],
        "properties": {
          "name":     { "type": "string", "description": "Full name of the family member." },
          "relation": { "type": "string", "description": "Relationship label, e.g. 'marito', 'figlia', 'fratello', 'nipote'." },
          "notes":    { "type": "string", "description": "1-2 sentences: where they live, status, current relationship quality, key facts." }
        }
      }
    },
    "routine": {
      "type": "array", "minItems": 4,
      "description": "Must cover all 24 hours with no gaps. Each location_id must be unique per physical room.",
      "items": {
        "type": "object",
        "required": ["time_from","time_to","location_id","location_label","activity"],
        "properties": {
          "time_from":      { "type": "string", "description": "HH:MM" },
          "time_to":        { "type": "string", "description": "HH:MM" },
          "location_id":    { "type": "string", "description": "Snake_case world id: {npc_id}_{room}, e.g. 'elena_302_bagno', 'elena_302_camera'. Same physical room = same id always." },
          "location_label": { "type": "string", "description": "Human-readable room name, e.g. 'Bathroom', 'Bedroom'." },
          "activity":       { "type": "string", "description": "Default activity description. Be specific and characterful." },
          "outfit":         { "type": "string", "description": "What they wear during this slot, 3-8 words (e.g. 'tuta da ginnastica grigia', 'completo blu con cravatta', 'pigiama di flanella'). Consistent wardrobe: same person, same closet." },
          "day":            { "type": "array", "items": { "type": "string" },
                              "description": "Days when this slot is active. Use Italian day names: lunedì martedì mercoledì giovedì venerdì sabato domenica. Empty array or omit = every day. Example: [\"lunedì\",\"martedì\",\"mercoledì\",\"giovedì\",\"venerdì\"] for weekdays only." },
          "stats":          { "type": "object", "description": "Stat deltas per tick while in this slot, e.g. {energy:-0.05, hunger:0.03}. Negative = costs, positive = recovery.", "additionalProperties": { "type": "number" } },
          "narrative_hint": { "type": "string", "description": "Hint shown to main LLM only when protagonist is in the same location. Keep short (1 sentence)." },
          "variations": {
            "type": "array",
            "description": "Alternate activities tried in order before the default. First one whose prob roll succeeds is used.",
            "items": {
              "type": "object",
              "properties": {
                "activity":       { "type": "string" },
                "prob":           { "type": "number", "minimum": 0, "maximum": 1, "description": "0.0-1.0 probability this variation fires on any given tick." },
                "stats":          { "type": "object", "additionalProperties": { "type": "number" } },
                "narrative_hint": { "type": "string" },
                "event":          { "type": "string", "description": "Event id emitted when this variation fires. Other NPCs with a matching event_reactions entry will react automatically." },
                "info":           { "type": "object", "description": "Event routing info. Required fields: type ('broadcast'|'location'|'direct'|'area') and optionally target_location (for location type), target (npc_id, for direct type), detail (human-readable description of what happened).", "additionalProperties": true }
              }
            }
          }
        }
      }
    },
    "stats_defaults": {
      "type": "object",
      "description": "2-4 personality-relevant float stats, values 0.0-1.0",
      "additionalProperties": { "type": "number" }
    },
    "event_reactions": {
      "type": "object",
      "description": "key = event_name, value = { activity, narrative_hint }",
      "additionalProperties": {
        "type": "object",
        "properties": {
          "activity":       { "type": "string" },
          "narrative_hint": { "type": "string" }
        }
      }
    },
    "agent_system": {
      "type": "string",
      "description": "First-person LLM system prompt for this NPC as an agent. Include: name, age, job, personality, speech style, key relationships, current situation. 4-8 sentences."
    },
    "short_term_goals": { "type": "array", "items": { "type": "string" } },
    "long_term_goals":  { "type": "array", "items": { "type": "string" } }
  }
}]]

local function _known_names()
    local parts = {}
    for id, d in pairs(_npcs) do
        table.insert(parts, (d.name or id) .. " [" .. id .. "]")
    end
    return #parts > 0 and table.concat(parts, ", ") or "none"
end

-- World bible block (authoritative shared facts from world.lua, if loaded).
-- Keeps independently generated NPCs consistent with established world truth.
local function _world_bible_block()
    local okw, w = pcall(require, "lib/world")
    if okw and type(w) == "table" and type(w.format_bible) == "function" then
        local b = w.format_bible()
        if b ~= "" then return "\n" .. b end
    end
    return ""
end

-- Data-driven variety: histogram of the existing cast (age bands + jobs)
-- injected into the generation prompt. Beats generic "be varied" exhortation
-- because the model sees exactly what is over-represented.
local function _variety_block()
    local ages, jobs, n = {}, {}, 0
    for _, d in pairs(_npcs) do
        n = n + 1
        local a = tonumber(d.age)
        if a then
            local band = (a < 30 and "18-29") or (a < 45 and "30-44")
                      or (a < 65 and "45-64") or "65+"
            ages[band] = (ages[band] or 0) + 1
        end
        if d.job and d.job ~= "" then table.insert(jobs, d.job) end
    end
    if n == 0 then return "" end
    local parts = {}
    for band, c in pairs(ages) do table.insert(parts, band .. ": " .. c) end
    table.sort(parts)
    return "\nCAST ALREADY PRESENT — steer AWAY from over-represented bands and duplicate niches:"
        .. "\n  age bands: " .. table.concat(parts, ", ")
        .. (#jobs > 0 and ("\n  jobs taken: " .. table.concat(jobs, "; ")) or "")
end

-- Pick the least represented template from the variety pool (counts only
-- generated NPCs that recorded a template).
local function _pick_variety_template()
    if type(_variety_pool) ~= "table" or #_variety_pool == 0 then return nil end
    local counts = {}
    for _, t in ipairs(_variety_pool) do counts[t] = 0 end
    for _, d in pairs(_npcs) do
        if d.template and counts[d.template] then
            counts[d.template] = counts[d.template] + 1
        end
    end
    local best, best_c = nil, math.huge
    for _, t in ipairs(_variety_pool) do
        if counts[t] < best_c then best, best_c = t, counts[t] end
    end
    return best
end

-- Semantic critique: an LLM reviewer judges the generated character for
-- soul, not shape. Returns true | false, reason — usable as validate_fn.
-- Reviewer unavailable/broken → pass (never block generation on critic infra).
local function _critic_review(d)
    local sys = "You are a strict quality reviewer for generated RPG characters. "
        .. "Judge the character against the world and the existing cast. "
        .. "FAIL it when: the secret is banal or creates no narrative/social tension; "
        .. "the personality or agent_system is generic (could describe anyone); "
        .. "the routine has no characterful detail; "
        .. "it duplicates an existing character's niche (same job + age band + vibe); "
        .. "it contradicts the established world facts. "
        .. "PASS otherwise — judge substance, do not nitpick style. "
        .. "Reply ONLY with valid JSON.\n\n"
        .. "World: " .. _context
        .. _world_bible_block()
        .. "\nExisting NPCs: " .. _known_names()
    local user = "Review this generated character:\n" .. json.encode({
        name = d.name, age = d.age, job = d.job,
        personality = d.personality, secret = d.secret,
        agent_system = d.agent_system,
    })
    local schema = [[{
        "type": "object", "required": ["pass"],
        "properties": {
            "pass":   { "type": "boolean" },
            "reason": { "type": "string",
                        "description": "If pass=false: what is weak and how to fix it. Specific and actionable." }
        }
    }]]
    local cm, cp = _critique_model or _model, _critique_provider or _provider
    if not cm or not cp then
        local tm, tp = llm_util.tier("gen")
        cm = cm or tm
        cp = cp or tp
    end
    local ok, raw = pcall(query_llm, sys, "[]", user, schema, cm, cp)
    if not ok then return true end
    local ok2, verdict = pcall(json.decode, raw)
    if not ok2 or type(verdict) ~= "table" then return true end
    if verdict.pass == false then
        return false, "QUALITY REVIEW FAILED: "
            .. (verdict.reason or "generic, flat character — add specificity and tension")
    end
    return true
end

--- Generate a new NPC persona via LLM, write the .lua file, add to registry.
-- Idempotent: if id already exists in registry, returns existing data without LLM call.
-- @param id             Unique snake_case identifier (e.g. "marco_203").
-- @param context        Coherence hint: role, age range, location, relationship to world.
-- @param template_name  Optional: "base" | "worker" | "student" | "pensioner".
--                       Falls back to _default_template set in M.init().
-- @param opts           Optional table: { location_prefix="apt_201" } — canonical
--                       location prefix for household members. When set, the LLM
--                       generates location_ids as {prefix}_{room} (e.g. "apt_201_cucina")
--                       instead of {id}_{room}. Use for NPCs sharing a living space.
-- @return               Data table or nil on failure.
function M.generate(id, context, template_name, opts)
    if _npcs[id] then return _npcs[id] end

    -- Try loading from disk first (file may exist from a previous session)
    local existing, load_err = _load_file(id)
    if existing then
        _npcs[id] = existing
        return existing
    end
    -- Corrupt file: back it up before regenerating over it
    if load_err then
        _backup_corrupt(id, load_err)
    end

    -- A "specific" context names WHO this character is (role/age/traits), as
    -- opposed to a thin hint like the id or "un vicino". When specific, the
    -- caller's intent is authoritative and the variety/critique machinery (which
    -- optimizes for novelty/spice) must NOT override it — that drift turned an
    -- explicit "Vito, ice seller, 55" into "Stefano, law student, 24".
    local specific = type(context) == "string" and context ~= id and #context >= 12

    -- Load template (explicit > variety rotation > default > nil). Skip rotation
    -- for specific contexts so a rotated archetype can't fight the request.
    local tmpl_name = template_name
    if not tmpl_name and not specific then
        tmpl_name = _pick_variety_template() or _default_template
    end
    local tmpl = nil
    if tmpl_name then
        local ok, t = pcall(require, "lib/npc_template_" .. tmpl_name)
        if ok and type(t) == "table" then
            tmpl = t
        else
            -- Unknown template name — log and continue without template
            pcall(function()
                local f = io.open("/tmp/persona_generate_reject.log", "a")
                if f then
                    f:write(os.date("%Y-%m-%d %H:%M:%S")
                        .. " [" .. id .. "] template '" .. tostring(tmpl_name) .. "' not found, proceeding without.\n")
                    f:close()
                end
            end)
        end
    end

    local loc_pfx = (opts and opts.location_prefix) or id

    -- Authority directive (primacy position) — only when the context is specific.
    local authority = specific and (
        "ABSOLUTE PRIORITY: the user Context below specifies WHO this character is "
        .. "(name, role, age, traits). Honor it EXACTLY — keep the stated name, job, "
        .. "age and personality. Do NOT invent a different person. The variety and "
        .. "novelty guidance that follows applies ONLY to details the Context leaves "
        .. "open (routine, secret, family, voice) — never to override the Context.\n\n"
    ) or ""

    local sys = authority
             .. "You are a character generator for a life-simulation RPG in the Italian comedy drama style. "
             .. "Generate a believable, specific character with something interesting beneath the surface. "
             .. "VARIETY: do not generate yet another lonely pensioner. Consider: couples in crisis, separated individuals, "
             .. "workers with double lives, enterprising singles, widows who have rebuilt their lives, "
             .. "broke young people, forty-somethings searching for meaning. Age range 25-80, do not cluster around the elderly. "
             .. "The routine MUST cover all 24 hours with no gaps. "
             .. "For location_id use the format {prefix}_{room} in snake_case "
             .. "where prefix='" .. loc_pfx .. "' "
             .. "(e.g. '" .. loc_pfx .. "_cucina', '" .. loc_pfx .. "_camera'). "
             .. "The same physical room must always have the same location_id. "
             .. "In the night sleep slot (e.g. 23:00-06:00) use activity='sleeps and dreams'. "
             .. "Give every routine slot an 'outfit' (3-8 words): nightwear for sleep, work clothes for work, "
             .. "home clothes for evenings. Wardrobe must be coherent across slots and with the character's budget and style. "
             .. "Use the 'day' field to differentiate weekday vs weekend behaviour: "
             .. "work/school slots get day=[\"lunedì\",\"martedì\",\"mercoledì\",\"giovedì\",\"venerdì\"], "
             .. "weekend leisure slots get day=[\"sabato\",\"domenica\"]. "
             .. "Slots valid every day (sleep, meals at home) omit 'day' entirely. "
             .. "Cover both weekday AND weekend versions of the daytime block. "
             .. "appearance: a concrete physical description of the BODY (build, height, hair, face, "
             .. "distinctive features) — NOT clothes. 1-2 sentences, specific. "
             .. "home: specify the exact apartment and floor (e.g. 'Apartment 202, second floor'). "
             .. "workplace: precise place of work or 'retired, does not work'. "
             .. "secret: ONE private fact the character never reveals spontaneously. "
             .. "Must have some spice: a secret affair, an obsession, something racy or embarrassing, "
             .. "a hidden vice, an unconfessable desire, a lie that has gone on for years. "
             .. "Avoid banal or purely financial secrets. Prefer secrets that create social or romantic tension. "
             .. "family: stable list of family members with name, relation, notes. At least 1-3 plausible entries. "
             .. "agent_system: system prompt in FIRST PERSON, minimum 4 sentences. Distinctive voice, not generic. "
             .. "Reply ONLY with valid JSON according to the schema.\n\n"
             .. "World: " .. _context .. "\n"
             .. "Existing NPCs: " .. _known_names()
             .. _world_bible_block()
             .. _variety_block()
             .. (tmpl and tmpl.prompt_hint and ("\n\nTEMPLATE GUIDANCE: " .. tmpl.prompt_hint) or "")

    local user = "Generate the character id='" .. id .. "'. Context: " .. (context or id)

    local schema = _SCHEMA:gsub(
        "Snake_case world id: {npc_id}_{room}, e%.g%. 'elena_302_bagno', 'elena_302_camera'%. Same physical room = same id always%.",
        "Snake_case world id: {prefix}_{room} where prefix='" .. loc_pfx .. "', e.g. '"
            .. loc_pfx .. "_cucina', '" .. loc_pfx .. "_camera'. "
            .. "NEVER use the NPC id as prefix — always use '" .. loc_pfx .. "'. Same physical room = same id always.")
    -- Sanity-check against RAG contamination: reject if the output looks like
    -- it mixed in content from RAG examples (English boilerplate, wrong IDs, etc.)
    local function _generation_looks_valid(d)
        if type(d.name) ~= "string" or d.name == "" then
            return false, "name missing"
        end
        -- agent_system must be a real character voice, not a placeholder
        if type(d.agent_system) ~= "string" or #d.agent_system < 80 then
            return false, "agent_system too short or missing"
        end
        -- routine must exist and location_ids must follow the {id}_{room} convention.
        -- 1-3 slots is SPARSE, not invalid: _pad_routine repairs it to 24h
        -- coverage after the call — mid-tier gen models rarely emit 4+ slots
        -- and rejecting made the narrator retry identically until the loop cap.
        local rt = d.routine or {}
        if #rt < 1 then
            return false, "routine missing (0 slots) — provide at least one "
                .. "{time_from,time_to,location_id,activity} slot"
        end
        local bad_ids = 0
        for _, r in ipairs(rt) do
            local lid = r.location_id or ""
            -- Accept: {id}_ prefix, canonical household prefix, known external prefixes
            if lid ~= "" and not lid:match("^" .. id .. "_")
                         and not lid:match("^" .. loc_pfx .. "_")
                         and not lid:match("^esterno")
                         and not lid:match("^fuori")
                         and not lid:match("^liceo")
                         and not lid:match("^chiesa")
                         and not lid:match("^scuola") then
                bad_ids = bad_ids + 1
            end
        end
        -- more than half bad → contaminated
        if bad_ids > math.floor(#rt / 2) then
            return false, bad_ids .. "/" .. #rt .. " location_ids do not follow the {id}_room format"
        end
        -- stats_defaults empty is NOT a hard-reject (regression fixed
        -- 2026-07-18): a real live failure showed a model getting stuck
        -- rejecting-and-retrying on this field FOREVER — even with a rich,
        -- explicit context ("Stats: libido 0.8, autorità 0.7, segretezza
        -- 0.9") — until M.generate gave up and the NPC never got created at
        -- all. That's strictly worse than an empty stats_defaults: this
        -- exact gap is what M._backfill_personal_stats (fired from
        -- M.npc_object, and from M.generate's own tail below) exists to fix
        -- gracefully AFTER birth, non-blocking. Let her be born; the async
        -- safety net covers the rest. Common stats (_backfill_common_stats)
        -- apply unconditionally regardless either way.
        return true, "ok"
    end

    -- Validated call: rejection reasons are fed back to the LLM for repair
    -- retries inside the call, instead of failing the whole generation.
    -- Validation = structure first, then (if enabled) semantic critique:
    -- a reviewer LLM judges variety/tension/voice and its reason becomes
    -- the retry feedback.
    local function _full_validate(d)
        local ok, verr = _generation_looks_valid(d)
        if not ok then return ok, verr end
        -- Skip the variety/spice critic for specific contexts: it optimizes for
        -- novelty and would reject a faithful-but-plain rendering of the request.
        if _critique and not specific then return _critic_review(d) end
        return true
    end
    local data, gen_err = llm_util.validated_call(sys, user, schema,
        _full_validate, { retries = 2, model = _model, provider = _provider, tier = "gen" })
    if not data then
        _log_err("[" .. id .. "] generation REJECTED after retries: " .. tostring(gen_err))
        return nil, gen_err
    end

    -- Repair: pad a sparse routine to full 24h coverage instead of rejecting.
    -- Gaps are filled with a home/rest slot; dreams enrich the routine later.
    do
        local function tomin(t)
            local h, m = tostring(t or ""):match("^(%d+):(%d+)$")
            if not h then return nil end
            return tonumber(h) * 60 + tonumber(m)
        end
        local slots = {}
        for _, r in ipairs(data.routine or {}) do
            if tomin(r.time_from) and tomin(r.time_to) and r.location_id then
                table.insert(slots, r)
            end
        end
        if #slots > 0 then
            table.sort(slots, function(a, b)
                return tomin(a.time_from) < tomin(b.time_from)
            end)
            local home = nil
            for _, r in ipairs(slots) do
                local lid = tostring(r.location_id)
                if lid:match("casa") or lid:match("camera") or lid:match("home") then
                    home = r.location_id; break
                end
            end
            home = home or slots[1].location_id
            local padded = {}
            for i, r in ipairs(slots) do
                table.insert(padded, r)
                local nxt      = slots[i + 1]
                local gap_from = r.time_to
                local gap_to   = nxt and nxt.time_from or slots[1].time_from
                local gf, gt   = tomin(gap_from), tomin(gap_to)
                -- intermediate gap only when forward (skip overlaps);
                -- final gap wraps midnight back to the first slot
                local insert_gap = nxt and (gt > gf) or (not nxt and gt ~= gf)
                if insert_gap then
                    local night = gf >= 20 * 60 or gf < 6 * 60
                    table.insert(padded, {
                        time_from   = gap_from,
                        time_to     = gap_to,
                        location_id = home,
                        activity    = night and "dorme"
                                      or "in casa, si occupa delle sue cose",
                        outfit      = night and "abbigliamento da notte" or nil,
                    })
                end
            end
            data.routine = padded
        end
    end

    data.id         = id
    data.template   = tmpl_name  -- recorded for variety rotation counts
    data.secret     = data.secret  or ""
    data.family     = data.family  or {}
    data.conditions = data.conditions  or {}
    data.life_events = data.life_events or {
        { date="start", event="character generated by the engine" },
    }
    -- Dream-grown behavioral additions — start empty, grow nightly
    data.dream_count         = 0
    data.npc_stats           = {}
    data.npc_sequences       = {}
    data.npc_needs           = {}
    data.npc_event_reactions = {}
    data.npc_summary         = ""
    data.dream_log           = {}

    -- Merge template base values (template provides defaults; generated values take precedence)
    if tmpl then
        -- stats_defaults: template fills in any stat the LLM didn't generate
        local base_stats = {}
        for k, v in pairs(tmpl.stats_defaults or {}) do base_stats[k] = v end
        for k, v in pairs(data.stats_defaults or {}) do base_stats[k] = v end
        data.stats_defaults = base_stats
        -- state_phrases: authoritative from template (not generated by LLM)
        data.state_phrases = tmpl.state_phrases or {}
    else
        data.state_phrases = data.state_phrases or {}
    end

    -- Cap PERSONAL stats (i.e. not one of _common_stats) at _max_stats —
    -- generation is asked for 2-4, this is just a safety ceiling against an
    -- unusually verbose response.
    do
        local personal = {}
        for k in pairs(data.stats_defaults or {}) do
            if _common_stats[k] == nil then table.insert(personal, k) end
        end
        if #personal > _max_stats then
            table.sort(personal)
            for i = _max_stats + 1, #personal do data.stats_defaults[personal[i]] = nil end
        end
    end
    -- Deterministic baseline every NPC gets regardless of what the LLM
    -- produced above — never overwrites a value the template/generation
    -- already set for the same name.
    _backfill_common_stats(data)

    _npcs[id] = data
    _write(data)

    -- Personal-stat safety net, fired unconditionally here (not only from
    -- M.npc_object) — in an adventure with no use_npc_tick, npc_object may
    -- never be built for a generated NPC, and this is the only chance for
    -- her to ever get a personality-specific stat instead of common-only.
    do
        local ok, err = pcall(M._backfill_personal_stats, id, nil)
        if not ok then _log_err("[" .. id .. "] backfill_personal_stats: " .. tostring(err)) end
    end

    -- Auto-create world locations for each unique location_id in the routine.
    -- Uses pcall so persona.lua works even if world.lua is not loaded in the script.
    local ok_w, world_lib = pcall(require, "lib/world")
    if ok_w and world_lib and type(world_lib.ensure_location) == "function" then
        local seen = {}
        for _, r in ipairs(data.routine or {}) do
            local lid = r.location_id
            if lid and not seen[lid] then
                seen[lid] = true
                local label = r.location_label or lid
                world_lib.ensure_location(lid, label .. " of " .. (data.name or id))
            end
        end
    end

    return data
end

-- ── Dream growth ───────────────────────────────────────────────────────────────

-- Stage 1 of the dream redesign (see memory project_dream_system_redesign):
-- dreams touch ONLY the soft/psychological layer that's ALWAYS consumed
-- (npc_summary → agent prompt, relationships/goals → agent + narrator
-- prompts) — never mechanical structure (sequence/need/event_reaction/stat).
-- Those are earned from real occurrences during play instead (stage 2/3),
-- never free-invented at night disconnected from anything real.
local _DREAM_SCHEMA = [[{
  "type": "object",
  "required": ["dream_narrative","aspect_developed","life_event_summary"],
  "properties": {
    "dream_narrative":    { "type": "string", "description": "2-3 sentences describing the dream." },
    "aspect_developed":   { "type": "string", "description": "Psychological aspect that emerged (e.g. 'fear of loneliness', 'desire for freedom')." },
    "life_event_summary": { "type": "string", "description": "One-line summary for life_events, prefixed with [dream]." },
    "npc_summary_update": { "type": "string", "description": "OPTIONAL. Update who this character is TODAY in 2-3 sentences, including recent developments. Must reflect evolution beyond the initial profile. Omit if nothing meaningfully changed." },
    "relationships_patch": {
      "type": "object",
      "description": "OPTIONAL. New or updated relationship descriptions — key=person's id/role, value=short description from this NPC's point of view. Only if recent events genuinely justify a shift.",
      "additionalProperties": { "type": "string" }
    },
    "short_term_goals_replace": {
      "type": "array", "items": { "type": "string" },
      "description": "OPTIONAL. Full replacement of short-term goals, only if they have genuinely shifted."
    },
    "long_term_goals_add": {
      "type": "array", "items": { "type": "string" },
      "description": "OPTIONAL. New long-term goals to append. Do not repeat existing ones."
    }
  }
}]]

-- Small schema for the event-compaction pre-step (see _compact_events_since_last_dream).
local _COMPACT_SCHEMA = [[{
  "type": "object",
  "required": ["summary"],
  "properties": {
    "summary": { "type": "string", "description": "ONE sentence capturing what mattered most across these events." }
  }
}]]

-- Jaccard similarity on word tokens (case-insensitive).
-- Returns true if two event strings share > 70% of their words.
local function _event_is_duplicate(events, new_event)
    local function tokenize(s)
        local t = {}
        for w in (s or ""):lower():gmatch("%a+") do
            if #w > 3 then t[w] = true end  -- skip short words
        end
        return t
    end
    local new_tok = tokenize(new_event)
    local new_count = 0
    for _ in pairs(new_tok) do new_count = new_count + 1 end
    if new_count == 0 then return false end
    -- Check against last 10 events
    local start = math.max(1, #events - 9)
    for i = start, #events do
        local old_tok = tokenize(events[i].event)
        local intersection, union = 0, 0
        for w in pairs(new_tok) do
            union = union + 1
            if old_tok[w] then intersection = intersection + 1 end
        end
        for w in pairs(old_tok) do
            if not new_tok[w] then union = union + 1 end
        end
        if union > 0 and (intersection / union) >= 0.7 then return true end
    end
    return false
end

-- Compact all life_events accumulated SINCE THE LAST DREAM (not just the
-- previous calendar day) into one [compressed] entry, run BEFORE dream()
-- reads recent events as context.
--
-- Replaces the old mechanism, which only compacted the PREVIOUS calendar
-- day, fused into the dream's OWN output (dream.day_summary), applied
-- AFTER generation — so a dream firing at night read a hardcoded last-5
-- window of raw events and could miss everything from earlier that same
-- day (e.g. a significant morning interaction). This runs first, reads
-- everything since the last [dream] marker, and only calls the LLM when
-- there's enough to actually compact (few events → dream reads them raw,
-- no extra call needed).
local function _compact_events_since_last_dream(data, date_str)
    local evs = data.life_events or {}
    if #evs == 0 then return end

    local boundary = 0
    for i, ev in ipairs(evs) do
        if (ev.event or ""):match("^%[dream%]") then boundary = i end
    end
    local pending = {}
    for i = boundary + 1, #evs do
        if not (evs[i].event or ""):match("^%[compressed%]") then
            table.insert(pending, evs[i])
        end
    end
    if #pending < 5 then return end  -- few enough — dream reads them directly

    local sys = "Compress these life events for an NPC into ONE sentence "
             .. "capturing what mattered most. Output only the sentence, "
             .. "no prefix."
    local lines = {}
    for _, ev in ipairs(pending) do
        table.insert(lines, "  - " .. (ev.date or "?") .. ": " .. (ev.event or "?"))
    end
    local user = "Events:\n" .. table.concat(lines, "\n")

    local result, err = llm_util.validated_call(sys, user, _COMPACT_SCHEMA,
        function(d)
            if type(d.summary) ~= "string" or d.summary == "" then
                return false, "summary missing"
            end
            return true
        end,
        { retries = 1, model = _model, provider = _provider, tier = "gen" })
    if not result then
        _log_err("[" .. (data.id or "?") .. "] event compaction failed: " .. tostring(err))
        return  -- leave events uncompacted; dream still reads them, just uncompressed
    end

    local rebuilt = {}
    for i = 1, boundary do table.insert(rebuilt, evs[i]) end
    table.insert(rebuilt, { date = date_str, event = "[compressed] " .. result.summary,
                            turn = _current_turn })
    data.life_events = rebuilt
end

--- Run a nightly dream for an NPC: deepens psychology — personality summary,
-- relationships, goals. Mechanical structure (routine/needs/sequences/
-- event_reactions) is NOT generated here — see stage 2/3 in memory
-- project_dream_system_redesign (earned from real occurrences during play,
-- not free-invented at night disconnected from anything real).
-- Call from after_ai_turn when NPC is in sleep slot and hasn't dreamed today.
-- Returns { narrative, aspect, life_event } or nil on failure.
-- @param id        NPC identifier.
-- @param date_str  In-game date label (e.g. "day 3").
function M.dream(id, date_str)
    local data = _npcs[id]
    if not data then return nil end

    -- Compact BEFORE reading context — fixes same-day-earlier events being
    -- lost to a hardcoded window (see helper doc above).
    _compact_events_since_last_dream(data, date_str)

    local recent_evs = {}
    local evs = data.life_events or {}
    local boundary = 0
    for i, ev in ipairs(evs) do
        if (ev.event or ""):match("^%[dream%]") then boundary = i end
    end
    for i = boundary + 1, #evs do
        table.insert(recent_evs, "  - " .. (evs[i].date or "?") .. ": " .. (evs[i].event or "?"))
    end

    local consolidation = ((data.dream_count or 0) + 1) % 7 == 0
    local sys = "You are a dream system for an NPC in a realistic life-simulation RPG. "
             .. "Generate a meaningful dream that reveals or deepens the NPC's psychology. "
             .. "npc_summary_update must STAY ANCHORED to the ORIGINAL personality: evolve it, "
             .. "never contradict or replace the core traits. "
             .. (consolidation and
                 ("CONSOLIDATION NIGHT: rewrite npc_summary_update FROM the original personality "
                  .. "plus the recent events, ignoring the previous summary (this resets drift). ")
                 or "")
             .. "You may ALSO update how this character relates to a specific person they know "
             .. "(relationships_patch) or shift their goals — ONLY if the recent events genuinely "
             .. "justify it. Leave npc_summary_update/relationships_patch/goals fields out "
             .. "entirely on nights where nothing has really changed; a purely narrative dream "
             .. "with no structural update is a normal, valid outcome. "
             .. "World: " .. _context

    local rel_lines = {}
    for k, v in pairs(data.relationships or {}) do table.insert(rel_lines, k .. ": " .. v) end

    local user = string.format(
        "NPC: %s (%d years old, %s).\nORIGINAL personality (anchor — never lose these traits): %s\n"
     .. "Current summary: %s\nConditions: %s\nExisting relationships: %s\n"
     .. "Short-term goal: %s\nLong-term goals: %s\nRecent events:\n%s\n\nGenerate the dream.",
        data.name or id,
        data.age or 0,
        data.job or "unknown",
        data.personality or "—",
        (data.npc_summary and data.npc_summary ~= "") and data.npc_summary or "(none yet)",
        table.concat(data.conditions or {}, ", "),
        #rel_lines > 0 and table.concat(rel_lines, "; ") or "(none noted)",
        table.concat(data.short_term_goals or {}, "; "),
        table.concat(data.long_term_goals or {}, "; "),
        #recent_evs > 0 and table.concat(recent_evs, "\n") or "  (none)"
    )

    local dream, dream_err = llm_util.validated_call(sys, user, _DREAM_SCHEMA,
        function(d)
            if type(d.dream_narrative) ~= "string" or d.dream_narrative == "" then
                return false, "dream_narrative missing"
            end
            return true
        end,
        { retries = 1, model = _model, provider = _provider, tier = "gen" })
    if not dream then
        _log_err("[" .. id .. "] dream REJECTED: " .. tostring(dream_err))
        return nil
    end

    -- Update npc_summary if provided
    if type(dream.npc_summary_update) == "string" and dream.npc_summary_update ~= "" then
        data.npc_summary = dream.npc_summary_update
    end

    -- Relationships: merge (add/update keys), never wipe the table.
    local relationships_updated = false
    if type(dream.relationships_patch) == "table" and next(dream.relationships_patch) then
        data.relationships = data.relationships or {}
        for k, v in pairs(dream.relationships_patch) do
            if type(v) == "string" and v ~= "" then
                data.relationships[k] = v
                relationships_updated = true
            end
        end
    end

    -- Goals
    local goals_updated = false
    if type(dream.short_term_goals_replace) == "table" and #dream.short_term_goals_replace > 0 then
        data.short_term_goals = dream.short_term_goals_replace
        goals_updated = true
    end
    if type(dream.long_term_goals_add) == "table" and #dream.long_term_goals_add > 0 then
        data.long_term_goals = data.long_term_goals or {}
        for _, g in ipairs(dream.long_term_goals_add) do
            local dup = false
            for _, existing in ipairs(data.long_term_goals) do
                if existing == g then dup = true; break end
            end
            if not dup then table.insert(data.long_term_goals, g) end
        end
        goals_updated = true
    end

    -- Record dream as life event (with dedup)
    local ev_summary = dream.life_event_summary or ("[dream] " .. (dream.aspect_developed or "—"))
    data.life_events = data.life_events or {}
    if not _event_is_duplicate(data.life_events, ev_summary) then
        table.insert(data.life_events, { date=date_str, event=ev_summary, turn=_current_turn })
    end
    data.dream_count = (data.dream_count or 0) + 1

    -- Dream log entry (persisted + written as file header comments)
    data.dream_log = data.dream_log or {}
    table.insert(data.dream_log, {
        date                   = date_str,
        aspect                 = dream.aspect_developed,
        narrative              = dream.dream_narrative,
        relationships_updated  = relationships_updated or nil,
        goals_updated          = goals_updated or nil,
    })

    _write(data)

    return {
        narrative  = dream.dream_narrative,
        aspect     = dream.aspect_developed,
        life_event = ev_summary,
    }
end

--- Nightly dream tick — call from after_ai_turn.
-- Picks one known persona that hasn't dreamed today and runs M.dream() for it.
-- Mutates last_dream[id] = day_index on success.
-- Returns { id, result } if a dream happened, nil otherwise.
--
-- @param time_str   Current in-game time, e.g. "02:30".
-- @param day_index  Integer day counter (used as guard key and date label).
-- @param last_dream Mutable table { npc_id → day_index_of_last_dream }.
-- @param npc_objects Unused since the stage-1 dream redesign (dreams no
--                    longer touch mechanical structure, so there is nothing
--                    to mirror onto a live npc.lua object — relationships/
--                    goals live in the persona file and are read fresh by
--                    narrator/agent code every turn). Kept for call-site
--                    compatibility; pass nil.
function M.dream_tick(time_str, day_index, last_dream, npc_objects)
    -- Only run in deep-sleep window 01:00-05:00
    local h = tonumber((time_str or "12:00"):match("^(%d+)")) or 12
    if h < 1 or h > 5 then return nil end

    -- If the caller passes nil (first run, old save without state.last_dream),
    -- fall back to a module-level table so the "already dreamed today" guard
    -- still persists for the session. A throwaway local table here would make
    -- the same NPC re-dream every turn in the 01-05 window.
    last_dream  = last_dream or _last_dream_fallback

    for id in pairs(_npcs) do
        if last_dream[id] ~= day_index then
            local date_str = "day " .. tostring(day_index)
            local result   = M.dream(id, date_str)
            if result then
                last_dream[id] = day_index
                return { id=id, result=result }
            end
        end
    end
    return nil
end

--- Run dreams for EVERY NPC that hasn't dreamed yet today, in one pass.
-- For when the player sleeps THROUGH the night (sleep_until): the whole cast
-- dreams at once, except those who already dreamed today (last_dream guard).
-- No time-window gate — the CALLER decides the night was slept. One LLM call
-- per dreaming NPC. Returns a list of { id, result }.
-- @param day_index   Integer day counter (guard key + date label).
-- @param last_dream  Mutable { npc_id → day_index_of_last_dream }.
-- @param npc_objects Unused since the stage-1 dream redesign — see M.dream_tick doc. Pass nil.
function M.dream_tick_all(day_index, last_dream, npc_objects)
    last_dream  = last_dream or _last_dream_fallback
    local out = {}
    for id in pairs(_npcs) do
        if last_dream[id] ~= day_index then
            local result = M.dream(id, "day " .. tostring(day_index))
            if result then
                last_dream[id] = day_index
                out[#out + 1] = { id = id, result = result }
            end
        end
    end
    return out
end

-- ── Need/sequence: earned from real stat crossings (stage 2a) ───────────────────
--
-- See memory project_dream_system_redesign. Unlike the old dream system,
-- this generates need+sequence ATOMICALLY — one LLM call produces both, so
-- need.sequence_name is guaranteed to match a real sequence (the old bug:
-- a dream-invented need could silently fail to link to any sequence). The
-- trigger is a REAL live stat value crossing a threshold, not free invention
-- at night disconnected from anything that actually happened.
--
-- First crossing for a stat → a brand new need+sequence pair.
-- Later crossings (need already exists) → ENRICH: one more option (a
-- different way of acting on the same need) appended to the existing need,
-- capped at _MAX_NEED_OPTIONS — keeps growth bounded by stat count instead
-- of needs proliferating.

local _NEED_SEQ_SCHEMA = [[{
  "type": "object",
  "required": ["sequence_name","sequence_steps","option_description"],
  "properties": {
    "sequence_name": { "type": "string", "description": "Snake_case name for the behavior sequence, e.g. 'seeks_comfort'." },
    "sequence_steps": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["location_id","activity"],
        "properties": {
          "location_id":    { "type": "string", "description": "Location id in snake_case ({npc_id}_{room})." },
          "activity":       { "type": "string" },
          "narrative_hint": { "type": ["string","null"] },
          "stats":          { "type": "object", "additionalProperties": { "type": "number" } }
        }
      }
    },
    "option_description": { "type": "string", "description": "What the NPC does / how they act when this need strikes and this sequence fires." }
  }
}]]

local _DEFAULT_NEED_THRESHOLD = 0.75
local _MAX_NEED_OPTIONS       = 3

-- In-flight guard, purely in-memory: { npc_id -> { stat -> true } }.
-- Not persisted — jobs don't survive an engine restart anyway (see
-- lib/jobs.lua), so a stale "pending" flag across restarts isn't a risk.
local _pending_need_gen = {}

--- Threshold at which `stat` should trigger need generation for this NPC.
-- data.stat_thresholds (optional, author-set: { statname = 0.0-1.0 })
-- overrides the default.
local function _need_threshold(data, stat)
    local t = data.stat_thresholds and data.stat_thresholds[stat]
    return (type(t) == "number") and t or _DEFAULT_NEED_THRESHOLD
end

local function _find_need(data, stat)
    for _, n in ipairs(data.npc_needs or {}) do
        if n.stat == stat then return n end
    end
    return nil
end

-- Handler registered at module load (not a closure captured at submit time)
-- so a script reload/restore always dispatches against current persona
-- state — matches lib/jobs.lua's own contract.
local _ok_jobs, _jobs = pcall(require, "lib/jobs")
if _ok_jobs and type(_jobs) == "table" and _jobs.on then
    _jobs.on("persona_need_gen", function(result, meta, err)
        local id = meta and meta.npc_id
        if id then
            _pending_need_gen[id] = _pending_need_gen[id] or {}
            _pending_need_gen[id][meta.stat] = nil   -- clear in-flight guard either way
        end
        if not result then
            _log_err("[" .. tostring(id) .. "] need generation failed: " .. tostring(err))
            return
        end
        local data = id and _npcs[id]
        if not data then return end
        if type(result.sequence_name) ~= "string" or result.sequence_name == ""
           or type(result.sequence_steps) ~= "table" then
            _log_err("[" .. id .. "] need generation: malformed result")
            return
        end

        data.npc_sequences = data.npc_sequences or {}
        data.npc_sequences[result.sequence_name] = result.sequence_steps

        local new_option = { condition = {}, sequence = result.sequence_name,
                              description = result.option_description or "" }
        local existing = _find_need(data, meta.stat)
        if existing then
            existing.options = existing.options or {}
            if #existing.options < _MAX_NEED_OPTIONS then
                table.insert(existing.options, new_option)
            end
        else
            data.npc_needs = data.npc_needs or {}
            table.insert(data.npc_needs, {
                stat      = meta.stat,
                threshold = _need_threshold(data, meta.stat),
                options   = { new_option },
            })
        end
        _write(data)

        -- Mirror onto the LIVE npc.lua tick object if one was supplied.
        -- Unlike stage-1 dream fields (narrator/agent-prompt only, safe to
        -- lag until next reload), this affects tick behaviour immediately —
        -- the NPC should be able to act on it THIS session, not after a
        -- restart.
        local npc_obj = meta.npc_obj
        if npc_obj and npc_obj.config then
            npc_obj.config.sequences = npc_obj.config.sequences or {}
            npc_obj.config.sequences[result.sequence_name] = result.sequence_steps
            npc_obj.config.needs = npc_obj.config.needs or {}
            local live_existing = nil
            for _, n in ipairs(npc_obj.config.needs) do
                if n.stat == meta.stat then live_existing = n; break end
            end
            if live_existing then
                live_existing.options = live_existing.options or {}
                table.insert(live_existing.options, new_option)
            else
                table.insert(npc_obj.config.needs, {
                    stat = meta.stat, threshold = _need_threshold(data, meta.stat),
                    options = { new_option },
                })
            end
        end
    end)
end

--- Scan live NPC stats for threshold crossings and fire (at most one per
-- stat, rate-limited) generation jobs. Call from the adventure's tick path
-- right after NPC.tick (e.g. quickstart's run_npc_tick — wired
-- automatically there when use_npc_tick is on). Submission is async and
-- non-blocking (falls back to sync only in engines/tests without
-- query_llm_async) — safe to call from a hot tick path. Results land later
-- via jobs.poll_all() (already called by adv.before_turn()).
-- @param npc_objects { npc_id -> npc.lua object } — only NPCs present here
--                     are checked (stat values live on the tick object).
function M.check_pending_needs(npc_objects)
    if not (_ok_jobs and _jobs) then return end
    for id, npc_obj in pairs(npc_objects or {}) do
        local data = _npcs[id]
        if data then
            for stat, value in pairs(npc_obj.stats or {}) do
                local threshold = _need_threshold(data, stat)
                if type(value) == "number" and value >= threshold then
                    _pending_need_gen[id] = _pending_need_gen[id] or {}
                    local existing = _find_need(data, stat)
                    local at_cap
                    if existing then
                        at_cap = existing.options and #existing.options >= _MAX_NEED_OPTIONS
                    else
                        at_cap = #(data.npc_needs or {}) >= _max_needs   -- total-needs safety ceiling
                    end
                    if not _pending_need_gen[id][stat] and not at_cap then
                        _pending_need_gen[id][stat] = true
                        local mode = existing and "enrich (add ONE more, DIFFERENT, way of acting on this need)"
                                              or "first (this need doesn't exist yet for this NPC)"
                        local existing_desc = ""
                        if existing and existing.options then
                            local d = {}
                            for _, o in ipairs(existing.options) do
                                table.insert(d, o.description or "?")
                            end
                            existing_desc = "\nExisting ways this need is already satisfied: "
                                         .. table.concat(d, "; ") .. ". Propose a DIFFERENT one."
                        end
                        local sys = "You are generating a behavioral pattern for an NPC "
                                 .. "in a realistic life-simulation RPG, triggered by a "
                                 .. "real elevated stat (not invented). Produce ONE short "
                                 .. "sequence (1-3 steps) the NPC follows and a description "
                                 .. "of when/how they act on it. "
                                 .. "For location_id use the format {npc_id}_{room}. "
                                 .. "World: " .. _context
                        local user = string.format(
                            "NPC: %s (%s). Stat '%s' is elevated (%.2f, threshold %.2f).\n"
                         .. "Personality: %s\nMode: %s%s\n\nGenerate the sequence.",
                            data.name or id, data.job or "?", stat, value, threshold,
                            data.personality or "—", mode, existing_desc)
                        _jobs.submit("persona_need_gen", sys, user, {
                            schema = _NEED_SEQ_SCHEMA, tier = "gen",
                            meta = { npc_id = id, stat = stat, npc_obj = npc_obj },
                        })
                    end
                end
            end
        end
    end
end

-- ── Event reactions: earned from real, repeated event delivery (stage 2b) ───
--
-- NPC.dispatch/onEvent (npc.lua) silently no-ops on a real event with no
-- matching reaction. Rather than a dream free-inventing an event_name that
-- may never fire again (the old, near-guaranteed-orphan bug), this tracks
-- occurrences of REAL events per (npc, event_name) — via npc.lua's generic
-- on_unhandled_event hook, wired in M.npc_object — and generates a reaction
-- on the 2nd real delivery. The connection is guaranteed by construction:
-- the event_name is copied verbatim from something that actually happened,
-- twice (not a one-off — skip the 1st occurrence so a rare event that won't
-- repeat doesn't burn a generation call for nothing).

local _EVENT_REACTION_SCHEMA = [[{
  "type": "object",
  "required": ["activity"],
  "properties": {
    "activity":       { "type": "string", "description": "What the NPC does in reaction to this event." },
    "narrative_hint": { "type": ["string","null"], "description": "Shown to the narrator only if the protagonist witnesses it." },
    "stats":          { "type": "object", "additionalProperties": { "type": "number" }, "description": "Stat deltas applied when this reaction fires." },
    "sequence_name":  { "type": ["string","null"], "description": "OPTIONAL snake_case name for a new multi-step sequence this reaction triggers, if a one-line reaction isn't enough." },
    "sequence_steps": {
      "type": ["array","null"],
      "description": "REQUIRED together with sequence_name — the steps of that new sequence.",
      "items": {
        "type": "object",
        "required": ["location_id","activity"],
        "properties": {
          "location_id":    { "type": "string", "description": "Location id in snake_case ({npc_id}_{room})." },
          "activity":       { "type": "string" },
          "narrative_hint": { "type": ["string","null"] },
          "stats":          { "type": "object", "additionalProperties": { "type": "number" } }
        }
      }
    }
  }
}]]

-- In-flight guard, mirrors _pending_need_gen: { npc_id -> { event_name -> true } }.
local _pending_event_gen = {}

if _ok_jobs and type(_jobs) == "table" and _jobs.on then
    _jobs.on("persona_event_reaction_gen", function(result, meta, err)
        local id = meta and meta.npc_id
        if id then
            _pending_event_gen[id] = _pending_event_gen[id] or {}
            _pending_event_gen[id][meta.event_name] = nil   -- clear in-flight guard either way
        end
        if not result then
            _log_err("[" .. tostring(id) .. "] event_reaction generation failed: " .. tostring(err))
            return
        end
        local data = id and _npcs[id]
        if not data then return end
        if type(result.activity) ~= "string" or result.activity == "" then
            _log_err("[" .. id .. "] event_reaction generation: malformed result")
            return
        end

        local reaction = {
            activity       = result.activity,
            narrative_hint = result.narrative_hint,
            stats          = result.stats or {},
        }
        -- Atomic pairing (same call, guaranteed match) — closes the gap the
        -- OLD dream schema had: it could never produce this at all, even
        -- though npc.lua's onEvent has always supported reaction.sequence.
        if type(result.sequence_name) == "string" and result.sequence_name ~= ""
           and type(result.sequence_steps) == "table" then
            data.npc_sequences = data.npc_sequences or {}
            data.npc_sequences[result.sequence_name] = result.sequence_steps
            reaction.sequence = result.sequence_name
        end

        data.npc_event_reactions = data.npc_event_reactions or {}
        data.npc_event_reactions[meta.event_name] = reaction
        _write(data)

        -- Mirror onto the LIVE npc.lua tick object, same reasoning as
        -- check_pending_needs: this affects tick behaviour, must not lag
        -- until a reload.
        local npc_obj = meta.npc_obj
        if npc_obj and npc_obj.config then
            npc_obj.config.event_reactions = npc_obj.config.event_reactions or {}
            npc_obj.config.event_reactions[meta.event_name] = reaction
            if reaction.sequence then
                npc_obj.config.sequences = npc_obj.config.sequences or {}
                npc_obj.config.sequences[reaction.sequence] = result.sequence_steps
            end
        end
    end)
end

--- Called via npc.lua's on_unhandled_event hook (wired in M.npc_object) when
-- a REAL event is delivered to this NPC with no matching reaction. Counts
-- occurrences per (npc, event_name), persisted so it survives save/load; on
-- the 2nd real occurrence, fires ONE async generation job (see schema above)
-- if under the event_reactions cap and none already in flight for this pair.
-- @param id          NPC identifier.
-- @param event_name  The REAL event name that was dispatched (verbatim —
--                    never invented, that's the whole point of stage 2b).
-- @param info        Event info table (unused today; kept for future context).
-- @param npc_obj     The live npc.lua object (for immediate tick mirroring).
function M._track_unhandled_event(id, event_name, info, npc_obj)
    if not (_ok_jobs and _jobs and event_name and event_name ~= "") then return end
    local data = _npcs[id]
    if not data then return end

    data.event_occurrence_count = data.event_occurrence_count or {}
    local n = (data.event_occurrence_count[event_name] or 0) + 1
    data.event_occurrence_count[event_name] = n
    _write(data)   -- cheap single-field change; keeps the count truthful across save/load/undo

    if n ~= 2 then return end   -- only the 2nd real occurrence triggers generation

    local count = 0
    for _ in pairs(data.npc_event_reactions or {}) do count = count + 1 end
    if count >= _max_event_reactions then return end   -- cap reached

    _pending_event_gen[id] = _pending_event_gen[id] or {}
    if _pending_event_gen[id][event_name] then return end   -- already in flight
    _pending_event_gen[id][event_name] = true

    local sys = "You are generating an NPC's reaction to a REAL recurring event "
             .. "in a realistic life-simulation RPG — this event has genuinely "
             .. "happened twice to this character, it is not invented. Produce "
             .. "a short, in-character reaction. Optionally, if it fits a "
             .. "recurring habit better than a one-line reaction, also propose "
             .. "a short new sequence (1-3 steps). "
             .. "For location_id use the format {npc_id}_{room}. "
             .. "World: " .. _context
    local user = string.format(
        "NPC: %s (%s). Personality: %s\nRecurring event: '%s' (has happened twice).\n\n"
     .. "Generate the reaction.",
        data.name or id, data.job or "?", data.personality or "—", event_name)

    _jobs.submit("persona_event_reaction_gen", sys, user, {
        schema = _EVENT_REACTION_SCHEMA, tier = "gen",
        meta = { npc_id = id, event_name = event_name, npc_obj = npc_obj },
    })
end

-- ── Routine crystallization: earned from repeated narrative interaction ────
-- (stage 3) ──────────────────────────────────────────────────────────────────
--
-- The mechanism that gives a DIALOGUE-ONLY NPC (no routine, purely reactive
-- via think_as_npc — "alive only while you're looking at her") a path to
-- becoming autonomous. Every real interaction (a think_as_npc dialogue turn,
-- or an npc_life_event) is tracked; every _CRYSTALLIZE_EVERY interactions an
-- LLM is shown the recent interaction summaries and asked whether a
-- recurring PATTERN has emerged worth turning into a permanent routine slot
-- — not asked to invent one from nothing. A rejected/no-pattern outcome is
-- common and expected; the counter simply resets and tries again after the
-- next batch of interactions.
--
-- Deliberately NOT built yet (see memory project_dream_system_redesign):
-- auto-adding `variations` to a crystallized slot the more it fires. That
-- needs its own tracking of "did this exact slot run" inside the tick loop,
-- which is a separate, fuzzier design question — left for a stage 3b pass.

local _CRYSTALLIZE_EVERY     = 5
local _MAX_INTERACTION_LOG   = 10

local _ROUTINE_CRYSTALLIZE_SCHEMA = [[{
  "type": "object",
  "required": ["pattern_found"],
  "properties": {
    "pattern_found":  { "type": "boolean", "description": "true ONLY if a genuine recurring pattern is visible across the interactions — not invented." },
    "time_from":      { "type": ["string","null"], "description": "HH:MM start of the routine slot." },
    "time_to":        { "type": ["string","null"], "description": "HH:MM end of the routine slot." },
    "location_id":    { "type": ["string","null"], "description": "Location id in snake_case ({npc_id}_{room})." },
    "activity":       { "type": ["string","null"] },
    "narrative_hint": { "type": ["string","null"] }
  }
}]]

-- In-flight guard, mirrors _pending_need_gen/_pending_event_gen.
local _pending_crystallize = {}

local function _tomin(hhmm)
    local h, m = tostring(hhmm or ""):match("^(%d+):(%d+)$")
    if not h then return nil end
    return tonumber(h) * 60 + tonumber(m)
end

-- True if [af,at) and [bf,bt) overlap, both possibly wrapping midnight.
-- Conservative: any overlap → reject the crystallized slot rather than risk
-- corrupting an already-coherent, possibly hand-authored, routine.
local function _time_ranges_overlap(af, at, bf, bt)
    if not (af and at and bf and bt) then return true end   -- unparsable → treat as conflicting, refuse
    local function expand(f, t)
        if t > f then return { { f, t } } end
        return { { f, 1440 }, { 0, t } }   -- wraps midnight → two segments
    end
    for _, a in ipairs(expand(af, at)) do
        for _, b in ipairs(expand(bf, bt)) do
            if a[1] < b[2] and b[1] < a[2] then return true end
        end
    end
    return false
end

local function _routine_slot_conflicts(routine, time_from, time_to)
    local nf, nt = _tomin(time_from), _tomin(time_to)
    if not (nf and nt) then return true end   -- malformed → refuse
    for _, r in ipairs(routine or {}) do
        local ef, et = _tomin(r.time_from), _tomin(r.time_to)
        if _time_ranges_overlap(nf, nt, ef, et) then return true end
    end
    return false
end

if _ok_jobs and type(_jobs) == "table" and _jobs.on then
    _jobs.on("persona_crystallize_routine", function(result, meta, err)
        local id = meta and meta.npc_id
        if id then _pending_crystallize[id] = nil end   -- clear in-flight guard either way
        if not result then
            _log_err("[" .. tostring(id) .. "] crystallization failed: " .. tostring(err))
            return
        end
        local data = id and _npcs[id]
        if not data then return end
        if not result.pattern_found then return end   -- no pattern this round — normal outcome, not an error

        if type(result.time_from) ~= "string" or type(result.time_to) ~= "string"
           or type(result.location_id) ~= "string" or type(result.activity) ~= "string"
           or result.location_id == "" or result.activity == "" then
            _log_err("[" .. id .. "] crystallization: pattern_found=true but slot incomplete")
            return
        end
        data.routine = data.routine or {}
        if _routine_slot_conflicts(data.routine, result.time_from, result.time_to) then
            _log_err("[" .. id .. "] crystallization: proposed slot " .. result.time_from
                .. "-" .. result.time_to .. " conflicts with an existing slot — discarded")
            return
        end

        local slot = {
            time_from      = result.time_from,
            time_to        = result.time_to,
            location_id    = result.location_id,
            activity       = result.activity,
            narrative_hint = result.narrative_hint,
            crystallized   = true,   -- transparency marker, harmless extra field
        }
        table.insert(data.routine, slot)
        _write(data)

        -- Mirror onto the LIVE npc.lua tick object (same translation
        -- M.npc_object applies at build time) so the NPC can follow this
        -- slot THIS session, not just after a reload.
        local npc_obj = meta.npc_obj
        if npc_obj and npc_obj.config then
            npc_obj.config.routine = npc_obj.config.routine or {}
            table.insert(npc_obj.config.routine, {
                time           = { slot.time_from, slot.time_to },
                location       = slot.location_id,
                activity       = slot.activity,
                narrative_hint = slot.narrative_hint,
            })
        end
    end)
end

--- Count one real interaction with an NPC toward routine crystallization.
-- Called automatically from persona.agent_object()'s wrapped :decide() (a
-- think_as_npc dialogue turn) and from the npc_life_event tool. Every
-- _CRYSTALLIZE_EVERY interactions, fires an async job asking whether a
-- pattern has emerged across the recent interaction summaries.
-- @param id       NPC identifier.
-- @param summary  Short text describing what this interaction was about.
-- @param npc_obj  Optional live npc.lua object, for immediate tick mirroring
--                 if crystallization succeeds (passed through job meta).
function M._track_interaction(id, summary, npc_obj)
    local data = _npcs[id]
    if not data then return end

    data.interaction_log = data.interaction_log or {}
    table.insert(data.interaction_log, { turn = _current_turn, summary = tostring(summary or "") })
    while #data.interaction_log > _MAX_INTERACTION_LOG do
        table.remove(data.interaction_log, 1)
    end
    data.interaction_count = (data.interaction_count or 0) + 1
    _write(data)

    if not (_ok_jobs and _jobs) then return end
    if data.interaction_count < _CRYSTALLIZE_EVERY then return end
    data.interaction_count = 0   -- reset regardless of outcome — try again after the next batch
    _write(data)

    if _pending_crystallize[id] then return end
    _pending_crystallize[id] = true

    local routine_desc = "(none yet — this NPC has no routine)"
    if #(data.routine or {}) > 0 then
        local lines = {}
        for _, r in ipairs(data.routine) do
            table.insert(lines, "  " .. (r.time_from or "?") .. "-" .. (r.time_to or "?")
                .. " " .. (r.activity or "?") .. " [" .. (r.location_id or "?") .. "]")
        end
        routine_desc = table.concat(lines, "\n")
    end
    local log_lines = {}
    for _, e in ipairs(data.interaction_log) do
        table.insert(log_lines, "  - " .. e.summary)
    end

    local sys = "You are looking for a recurring PATTERN in an NPC's recent "
             .. "interactions in a realistic life-simulation RPG. Only "
             .. "propose a routine slot if the SAME kind of thing genuinely "
             .. "recurs across these interactions — most of the time there "
             .. "is NO clear pattern yet, and pattern_found=false is the "
             .. "correct, expected answer. Never invent one to be helpful. "
             .. "For location_id use the format {npc_id}_{room}. "
             .. "World: " .. _context
    local user = string.format(
        "NPC: %s (%s). Personality: %s\nCurrent routine:\n%s\nRecent interactions:\n%s\n\n"
     .. "Is there a recurring pattern? If so, propose ONE routine slot.",
        data.name or id, data.job or "?", data.personality or "—",
        routine_desc, table.concat(log_lines, "\n"))

    _jobs.submit("persona_crystallize_routine", sys, user, {
        schema = _ROUTINE_CRYSTALLIZE_SCHEMA, tier = "gen",
        meta = { npc_id = id, npc_obj = npc_obj },
    })
end

-- ── Personal stats safety net (stage 4) ──────────────────────────────────────
--
-- _generation_looks_valid already rejects an empty stats_defaults and forces
-- a retry, but retries are capped (M.generate uses retries=2) and this also
-- has to cover NPCs that never went through generation at all (hand-authored
-- register_static configs that forgot stats_defaults, or old files written
-- before this feature existed). Best-effort, async, fires at most once per
-- NPC per session — if it fails, the NPC just runs on _common_stats alone,
-- never a crash or a stuck retry loop.

local _PERSONAL_STAT_SCHEMA = [[{
  "type": "object",
  "required": ["stats"],
  "properties": {
    "stats": {
      "type": "object",
      "description": "1-2 personality-specific float stats (0.0-1.0) that make this character distinct from a generic NPC — NOT generic ones like energy/mood/stress, those already exist.",
      "additionalProperties": { "type": "number" }
    }
  }
}]]

local _pending_stat_backfill = {}

if _ok_jobs and type(_jobs) == "table" and _jobs.on then
    _jobs.on("persona_backfill_stats", function(result, meta, err)
        local id = meta and meta.npc_id
        if id then _pending_stat_backfill[id] = nil end
        if not result or type(result.stats) ~= "table" then
            _log_err("[" .. tostring(id) .. "] personal stat backfill failed: " .. tostring(err))
            return
        end
        local data = id and _npcs[id]
        if not data then return end
        data.stats_defaults = data.stats_defaults or {}
        local applied = {}
        for k, v in pairs(result.stats) do
            if type(v) == "number" and data.stats_defaults[k] == nil then
                local clamped = math.max(0, math.min(1, v))
                data.stats_defaults[k] = clamped
                applied[k] = clamped
            end
        end
        _write(data)
        local npc_obj = meta.npc_obj
        if npc_obj and npc_obj.stats then
            for k, v in pairs(applied) do
                if npc_obj.stats[k] == nil then npc_obj.stats[k] = v end
            end
        end
    end)
end

--- If this NPC has no PERSONAL (non-common) stat yet, fire one cheap async
-- job to generate 1-2. Call whenever an NPC becomes live (M.npc_object
-- already does, automatically). Idempotent: no-ops if she already has a
-- personal stat or a backfill is already in flight for her.
-- @param id       NPC identifier.
-- @param npc_obj  Optional live npc.lua object, for immediate mirroring.
function M._backfill_personal_stats(id, npc_obj)
    if not (_ok_jobs and _jobs) then return end
    local data = _npcs[id]
    if not data then return end
    for k in pairs(data.stats_defaults or {}) do
        if _common_stats[k] == nil then return end   -- already has at least one — nothing to do
    end
    if _pending_stat_backfill[id] then return end
    _pending_stat_backfill[id] = true

    local sys = "Propose 1-2 personality-specific numeric stats (0.0-1.0) for "
             .. "an NPC in a life-simulation RPG — traits distinct to THIS "
             .. "character (e.g. 'gelosia' for a jealous character, "
             .. "'avidità' for a merchant), not generic ones. "
             .. "World: " .. _context
    local user = string.format("NPC: %s (%s). Personality: %s\n\nPropose the stats.",
        data.name or id, data.job or "?", data.personality or "—")
    _jobs.submit("persona_backfill_stats", sys, user, {
        schema = _PERSONAL_STAT_SCHEMA, tier = "gen",
        meta = { npc_id = id, npc_obj = npc_obj },
    })
end

-- ── Session isolation ──────────────────────────────────────────────────────────

--- Fork the template folder into a new timestamped session folder.
-- Call from set_initial_state() only. Copies all .lua NPC files from
-- template_path (defaults to current _base_path) into the new folder so
-- hand-authored NPCs start fresh from the originals. Generated NPCs
-- (no file in template) are re-generated by generate_all() as usual.
-- Returns the new path — store in state._persona_path for save/restore.
function M.new_session(template_path)
    template_path = (template_path or _base_path):gsub("([^/])$", "%1/")
    local base_no_slash = template_path:gsub("/$", "")
    local sessions_dir  = base_no_slash .. "_sessions"
    os.execute('mkdir -p "' .. sessions_dir .. '"')
    local ts       = os.date("%Y%m%d_%H%M%S")
    local new_path = sessions_dir .. "/" .. ts .. "/"
    os.execute('mkdir -p "' .. new_path .. '"')
    -- Copy hand-authored .lua files from template → session folder.
    local ok, handle = pcall(io.popen, 'ls -1 "' .. template_path .. '" 2>/dev/null')
    if ok and handle then
        for fname in handle:lines() do
            if fname:match("%.lua$") and not fname:match("%.broken") then
                local src = template_path .. fname
                local dst = new_path .. fname
                os.execute('cp "' .. src .. '" "' .. dst .. '"')
            end
        end
        handle:close()
    end
    _base_path = new_path
    _PERSONA_BASE_PATH = new_path
    _npcs = {}
    return new_path
end

--- Switch active path to an existing session folder (call from restore_state only).
-- Clears in-memory NPCs so reload_all() reloads from the restored folder.
function M.use_path(path)
    if not path or path == "" then return end
    _base_path = path
    if _base_path:sub(-1) ~= "/" then _base_path = _base_path .. "/" end
    _PERSONA_BASE_PATH = _base_path
    _npcs = {}
end

-- ── Registry access ────────────────────────────────────────────────────────────

function M.get_path() return _base_path end
function M.get(id)  return _npcs[id] end
function M.all()    return _npcs     end

function M.known_ids()
    local ids = {}
    for id in pairs(_npcs) do table.insert(ids, id) end
    table.sort(ids)
    return ids
end

-- ── Object factories ───────────────────────────────────────────────────────────

--- Create a npc.lua NPC object from this persona's current data.
-- Translates persona routine format → npc.lua config format.
-- @param id            NPC identifier.
-- @param world_adapter World adapter (see npc.lua docs).
-- @return              NPC object or nil.
function M.npc_object(id, world_adapter)
    local data = _npcs[id]
    if not data then return nil end
    local NPC_lib = require("lib/npc")

    local routine = {}
    for _, r in ipairs(data.routine or {}) do
        local entry = {
            time           = { r.time_from, r.time_to },
            location       = r.location_id or r.location,
            activity       = r.activity,
            stats          = r.stats,
            outfit         = r.outfit,
            narrative_hint = r.narrative_hint,
            day            = r.day,
            -- Phase 1 multi-narrator: a routine slot flagged llm=true asks the
            -- agent to render that beat (tick_and_log); carried through to npc.lua.
            llm            = r.llm,
            situation      = r.situation,
        }
        if r.variations then
            entry.variations = r.variations
        end
        table.insert(routine, entry)
    end

    -- Merge dream-grown additions into config
    local config_seqs = {}
    for seq_name, steps in pairs(data.npc_sequences or {}) do
        local converted = {}
        for _, s in ipairs(steps) do
            table.insert(converted, {
                location       = s.location_id or s.location,
                activity       = s.activity,
                narrative_hint = s.narrative_hint,
                stats          = s.stats or {},
            })
        end
        config_seqs[seq_name] = converted
    end

    local config_needs = {}
    local junk_needs = 0
    for _, n in ipairs(data.npc_needs or {}) do
        -- npc_needs is dream-written and structured; a hand-edited file may
        -- contain plain strings or partial tables (uninformed editor) — skip
        -- them for behaviour instead of building needs with nil stat/threshold
        -- that break NPC.tick later.
        if type(n) == "table" and n.stat and n.threshold then
            table.insert(config_needs, {
                stat      = n.stat,
                threshold = n.threshold,
                options   = {{ condition={}, sequence=n.sequence_name, description=n.description }},
            })
        else
            junk_needs = junk_needs + 1
        end
    end
    if junk_needs > 0 then
        _log_err("[" .. id .. "] " .. junk_needs .. " malformed npc_needs entries "
            .. "ignored (npc_needs is dream-written: {stat,threshold,...}; "
            .. "hand-authored behaviour goes in 'needs', full npc.lua format)")
    end

    -- Hand-authored, verbatim (full npc.lua format, no lossy translation):
    -- data.sequences / data.needs let a master write rich behaviour — multi-step
    -- sequences with guards/outfit/object/llm, needs with time+condition — that
    -- the dream-grown npc_sequences/npc_needs translation can't express. Merged
    -- ON TOP, so authored and dream-grown behaviours coexist.
    for seq_name, steps in pairs(data.sequences or {}) do
        config_seqs[seq_name] = steps
    end
    for _, n in ipairs(data.needs or {}) do
        table.insert(config_needs, n)
    end

    local config_er = {}
    for ev_name, reaction in pairs(data.npc_event_reactions or {}) do
        if type(reaction) == "string" then
            -- tolerate hand-edited shorthand: plain string = the activity
            config_er[ev_name] = { activity = reaction, stats = {} }
        elseif type(reaction) == "table" then
            config_er[ev_name] = {
                activity       = reaction.activity,
                narrative_hint = reaction.narrative_hint,
                stats          = reaction.stats or {},
            }
        end
    end

    -- Copy, don't reference: merging dream-grown reactions into the persisted
    -- data table would duplicate them into event_reactions at the next _write.
    local base_er = {}
    for k, v in pairs(data.event_reactions or {}) do base_er[k] = v end
    for k, v in pairs(config_er) do base_er[k] = v end

    local base_stats = {}
    for k, v in pairs(data.stats_defaults or {}) do base_stats[k] = v end
    for k, v in pairs(data.npc_stats or {}) do base_stats[k] = v end

    local first_name = (data.name or id):match("^(%S+)") or id
    local nobj = NPC_lib.new(id, {
        stats_defaults  = base_stats,
        idle_activity   = first_name .. " is still, lost in thought.",
        routine         = routine,
        needs           = config_needs,
        sequences       = config_seqs,
        event_reactions = base_er,
        -- Stage 2b: a real event with no reaction is the ideal, orphan-free
        -- trigger to learn one (see M._track_unhandled_event further down).
        on_unhandled_event = function(npc_self, event_name, info)
            M._track_unhandled_event(id, event_name, info, npc_self)
        end,
        memory_size     = 30,
    }, world_adapter)

    -- Stage 4 safety net: she's about to become live/ticked — the natural
    -- moment to notice a missing personal stat and backfill it, async.
    local ok, err = pcall(M._backfill_personal_stats, id, nobj)
    if not ok then _log_err("[" .. id .. "] backfill_personal_stats: " .. tostring(err)) end

    return nobj
end

--- Create an agent.lua agent object from this persona's current data.
-- Injects active conditions into the system prompt automatically.
-- @param id    NPC identifier.
-- @param opts  Table: { npc=npc_obj, turn_counter=tc, model="...", provider="...",
--                       max_per_turn=1, max_history=30 }.
-- @return      Agent object or nil.
function M.agent_object(id, opts)
    local data = _npcs[id]
    if not data then return nil end
    local agent_lib = require("lib/agent")
    opts = opts or {}

    -- Detect corrupt/generic agent_system and reconstruct from persona fields
    local sys = data.agent_system or ""
    local sys_corrupt = (#sys < 30
        or sys == "System" or sys == "system"
        or sys:match("^System%.")    -- catches "System.Object" etc.
        or sys:match("^%s*$"))
    if sys_corrupt then
        sys = string.format(
            "You are %s, %d years old. You work as %s. You live in %s.\n%s",
            data.name or id,
            data.age or 0,
            data.job or "worker",
            data.home or "an apartment",
            data.personality or "")
        if data.workplace and data.workplace ~= "" then
            sys = sys .. "\nWorkplace: " .. data.workplace .. "."
        end
    end

    -- Prepend npc_summary if available — reflects who this NPC is TODAY (dream-evolved)
    if type(data.npc_summary) == "string" and #data.npc_summary > 10 then
        sys = "CURRENT PROFILE: " .. data.npc_summary .. "\n\n" .. sys
    end
    -- Inject physical appearance so the NPC knows (and stays consistent about) its body.
    if type(data.appearance) == "string" and data.appearance ~= "" then
        sys = sys .. "\n\nIl tuo aspetto fisico: " .. data.appearance
    end
    -- Inject secret: the LLM must know it but never reveal it directly
    if data.secret and data.secret ~= "" then
        sys = sys .. "\n\nYou have a secret you guard and never reveal spontaneously: "
                  .. data.secret
                  .. " You may let tension or reticence show on this topic, but you never confess it explicitly."
    end
    -- Inject family: prevents the LLM from inventing inconsistent relatives
    if data.family and #data.family > 0 then
        local flines = {}
        for _, f in ipairs(data.family) do
            local line = f.relation .. ": " .. (f.name or "?")
            if f.notes and f.notes ~= "" then line = line .. " — " .. f.notes end
            table.insert(flines, line)
        end
        sys = sys .. "\n\nFamily (stable data — do not invent other relatives):\n- "
                  .. table.concat(flines, "\n- ")
    end
    -- Inject relationships (dream-evolved, non-family people this NPC knows)
    -- so dream-driven changes are visible in THIS NPC's own dialogue voice,
    -- not just the narrator-facing M.format() block.
    if data.relationships and next(data.relationships) then
        local rlines = {}
        for k, v in pairs(data.relationships) do table.insert(rlines, k .. ": " .. v) end
        table.sort(rlines)
        sys = sys .. "\n\nHow you see people you know:\n- " .. table.concat(rlines, "\n- ")
    end
    if data.conditions and #data.conditions > 0 then
        sys = sys .. "\n\nCurrent conditions: " .. table.concat(data.conditions, ", ") .. "."
    end
    if data.short_term_goals and #data.short_term_goals > 0 then
        sys = sys .. "\nImmediate goal: " .. table.concat(data.short_term_goals, "; ") .. "."
    end
    if data.carrying and #data.carrying > 0 then
        sys = sys .. "\nYou carry with you: " .. table.concat(data.carrying, ", ") .. "."
    end
    -- Inject known facts (gossip/ambient knowledge) with provenance —
    -- this IS what the NPC knows beyond direct experience.
    if data.known_facts and #data.known_facts > 0 then
        local fl = {}
        for _, f in ipairs(data.known_facts) do
            table.insert(fl, f.fact .. " (" .. (f.source or "?") .. ")")
        end
        sys = sys .. "\n\nThings you have learned (and how):\n- "
                  .. table.concat(fl, "\n- ")
    end

    -- Knowledge fence (C): explicit limits to prevent hallucination
    sys = sys .. [[

KNOWLEDGE LIMITS (HARD RULE):
- You only know what is in your life_events, the things you have learned listed above, and your direct interactions with people.
- You do not know the player's thoughts, private notes, or plans.
- You do not know other residents' secrets unless they told you directly.
- Do not invent details about people you have never met in person.
- If you have no information about something, say openly "I don't know" or "I'm not aware of that".
- Do not anticipate future events or summarise things you were not explicitly told.]]

    local ag = agent_lib.new(id, {
        system           = sys,
        model            = opts.model       or _model,
        provider         = opts.provider    or _provider,
        npc              = opts.npc,
        turn_counter     = opts.turn_counter,
        max_per_turn     = opts.max_per_turn,
        max_history      = opts.max_history or 30,
        short_term_goals = data.short_term_goals or {},
        long_term_goals  = data.long_term_goals  or {},
        -- Pass state_phrases so agent._format_states() injects human-readable phrases
        -- instead of raw "hunger=0.72" dumps.
        state_phrases    = data.state_phrases or {},
    })

    -- Stage 3: count every real dialogue interaction (think_as_npc → decide)
    -- toward routine crystallization — see M._track_interaction. Wrapped
    -- here rather than in agent.lua itself: agent.lua stays persona-agnostic,
    -- same separation already used for npc.lua's on_unhandled_event hook.
    if ag then
        local real_decide = ag.decide
        local npc_obj_ref = opts.npc   -- for immediate tick mirroring if crystallization fires
        ag.decide = function(self, situation, schema)
            local ok, err = pcall(M._track_interaction, id, situation, npc_obj_ref)
            if not ok then _log_err("[" .. id .. "] track_interaction: " .. tostring(err)) end
            return real_decide(self, situation, schema)
        end
    end

    return ag
end

-- ── Outfit / appearance ────────────────────────────────────────────────────────

-- Find the routine slot active at time_str (HH:MM) on day_str (or nil).
-- Handles midnight wrap (22:00-07:00) and day-restricted slots.
local function _find_active_slot(rt, time_str, day_str)
    local function tomin(hhmm)
        local h, m = tostring(hhmm or ""):match("^(%d+):(%d+)$")
        if not h then return nil end
        return tonumber(h) * 60 + tonumber(m)
    end
    local now = tomin(time_str)
    if not now then return nil end

    local function time_hit(r)
        local f, t = tomin(r.time_from), tomin(r.time_to)
        if not (f and t) then return false end
        if t <= f then return (now >= f) or (now < t) end  -- wraps midnight
        return (now >= f) and (now < t)
    end

    -- Two passes: day-SPECIFIC slots win over generic (no day) slots, so a
    -- weekend variant beats a weekday slot that forgot its day restriction.
    for _, r in ipairs(rt or {}) do
        if type(r.day) == "table" and #r.day > 0 then
            for _, dn in ipairs(r.day) do
                if dn == day_str and time_hit(r) then return r end
            end
        end
    end
    for _, r in ipairs(rt or {}) do
        if (type(r.day) ~= "table" or #r.day == 0) and time_hit(r) then
            return r
        end
    end
    return nil
end

--- What this NPC is wearing right now.
-- Resolution: outfit_override (runtime, set via npc_life_event) wins over
-- the active routine slot's outfit. Returns string or nil.
function M.current_outfit(id, time_str, day_str)
    local d = _npcs[id]
    if not d then return nil end
    if type(d.outfit_override) == "string" and d.outfit_override ~= "" then
        return d.outfit_override
    end
    local slot = _find_active_slot(d.routine, time_str, day_str)
    return slot and slot.outfit or nil
end

--- Set (or clear with nil/"") a runtime outfit override. Persists to file.
function M.set_outfit_override(id, outfit)
    local d = _npcs[id]
    if not d then return false end
    if outfit == nil or outfit == "" then
        d.outfit_override = nil
    else
        d.outfit_override = outfit
    end
    _write(d)
    return true
end

-- ── Routine variations: earned from WITNESSED repetition (stage 3b) ─────────
--
-- Counts only occurrences the protagonist actually WITNESSED (co-located
-- with the NPC while the slot is active) — an off-screen repeat is
-- invisible to the player, costs nothing, and teaches nothing; only a
-- witnessed repeat is where sameness becomes perceptible. Per-slot counter
-- (variations attach to one specific slot, stored directly on it — reuses
-- _find_active_slot rather than needing a separate slot-id scheme).

local _VARIATION_SCHEMA = [[{
  "type": "object",
  "required": ["activity","prob"],
  "properties": {
    "activity":       { "type": "string", "description": "What she does INSTEAD of the slot's usual activity, when this variation fires." },
    "prob":           { "type": "number", "minimum": 0.05, "maximum": 0.9, "description": "Base probability this variation fires on any given occurrence of the slot." },
    "narrative_hint": { "type": ["string","null"] },
    "stats":          { "type": "object", "additionalProperties": { "type": "number" } },
    "prob_boost_when": {
      "type": ["object","null"],
      "description": "OPTIONAL: raise the probability when a specific stat crosses a threshold. Only include if there's a genuine psychological reason for THIS character — most variations don't need one.",
      "properties": {
        "stat":         { "type": "string", "description": "MUST be exactly one of the NPC's existing stat names listed in the prompt." },
        "min":          { "type": ["number","null"] },
        "max":          { "type": ["number","null"] },
        "boosted_prob": { "type": "number", "minimum": 0.05, "maximum": 0.95 }
      }
    }
  }
}]]

local _WITNESS_THRESHOLD   = 3
local _MAX_SLOT_VARIATIONS = 2

-- In-flight guard, per NPC (not per-slot — one variation generation in
-- flight per NPC at a time is a fine restriction, keeps this simple).
local _pending_variation_gen = {}

if _ok_jobs and type(_jobs) == "table" and _jobs.on then
    _jobs.on("persona_variation_gen", function(result, meta, err)
        local id = meta and meta.npc_id
        if id then _pending_variation_gen[id] = nil end
        if not result then
            _log_err("[" .. tostring(id) .. "] variation generation failed: " .. tostring(err))
            return
        end
        local data = id and _npcs[id]
        if not data then return end
        if type(result.activity) ~= "string" or result.activity == ""
           or type(result.prob) ~= "number" then
            _log_err("[" .. id .. "] variation generation: malformed result")
            return
        end

        -- Re-resolve the slot fresh by (time_from,time_to) — never hold a
        -- table reference across the async gap, a reload/restore may have
        -- replaced `data` in the meantime (same principle as 2a/2b/3).
        local slot = nil
        for _, r in ipairs(data.routine or {}) do
            if r.time_from == meta.time_from and r.time_to == meta.time_to then
                slot = r; break
            end
        end
        if not slot then return end   -- slot no longer exists (edited/removed meanwhile)
        if #(slot.variations or {}) >= _MAX_SLOT_VARIATIONS then return end

        local variation = {
            activity       = result.activity,
            prob           = math.max(0.05, math.min(0.9, result.prob)),
            narrative_hint = result.narrative_hint,
            stats          = result.stats or {},
        }
        if type(result.prob_boost_when) == "table" and type(result.prob_boost_when.stat) == "string" then
            if (data.stats_defaults or {})[result.prob_boost_when.stat] ~= nil then
                variation.prob_boost_when = {
                    stat         = result.prob_boost_when.stat,
                    min          = result.prob_boost_when.min,
                    max          = result.prob_boost_when.max,
                    boosted_prob = math.max(0.05, math.min(0.95, result.prob_boost_when.boosted_prob or 0.5)),
                }
            else
                -- Orphan reference (same class of bug fixed in stages 2a/2b):
                -- drop ONLY prob_boost_when, keep the variation itself —
                -- without a real stat behind it, npc.lua's own eff-fallback
                -- would make it fire ~always (the exact footgun documented
                -- in CLAUDE.md), so dropping is strictly safer than keeping.
                _log_err("[" .. id .. "] variation: prob_boost_when.stat '"
                    .. tostring(result.prob_boost_when.stat)
                    .. "' is not a real stat on this NPC — dropped, kept the variation itself")
            end
        end

        slot.variations = slot.variations or {}
        table.insert(slot.variations, variation)
        _write(data)

        -- Mirror onto the live npc.lua object (same time-key re-resolution;
        -- npc_obj.config.routine uses {time={from,to}}, not time_from/time_to).
        local npc_obj = meta.npc_obj
        if npc_obj and npc_obj.config and npc_obj.config.routine then
            for _, r in ipairs(npc_obj.config.routine) do
                if r.time and r.time[1] == meta.time_from and r.time[2] == meta.time_to then
                    r.variations = r.variations or {}
                    table.insert(r.variations, variation)
                    break
                end
            end
        end
    end)
end

--- Count one WITNESSED occurrence of an NPC's currently-active routine
-- slot, and fire an async variation-generation job once _WITNESS_THRESHOLD
-- is reached for that slot (capped at _MAX_SLOT_VARIATIONS per slot).
-- Off-screen (unwitnessed) occurrences cost nothing and count for nothing —
-- nobody would notice the repetition, so there's nothing to fix.
-- Call from the adventure's tick path right after NPC.tick (quickstart's
-- run_npc_tick does this automatically whenever use_npc_tick is on).
-- @param id             NPC identifier.
-- @param time_str, day_str  Current in-game clock (to resolve the active slot).
-- @param witnessed      True if the protagonist is co-located with this NPC right now.
-- @param npc_obj        Live npc.lua object, for immediate tick mirroring.
function M.track_routine_variation(id, time_str, day_str, witnessed, npc_obj)
    if not witnessed then return end
    if not (_ok_jobs and _jobs) then return end
    local data = _npcs[id]
    if not data then return end
    local slot = _find_active_slot(data.routine, time_str, day_str)
    if not slot then return end
    if #(slot.variations or {}) >= _MAX_SLOT_VARIATIONS then return end

    slot.witness_count = (slot.witness_count or 0) + 1
    if slot.witness_count < _WITNESS_THRESHOLD then
        _write(data)
        return
    end
    slot.witness_count = 0   -- reset — try again after the next batch of witnessed visits
    _write(data)

    if _pending_variation_gen[id] then return end
    _pending_variation_gen[id] = true

    local stat_names = {}
    for k in pairs(data.stats_defaults or {}) do table.insert(stat_names, k) end
    table.sort(stat_names)

    local recent_lines = {}
    local evs = data.life_events or {}
    for i = math.max(1, #evs - 3), #evs do
        table.insert(recent_lines, "  - " .. (evs[i].event or "?"))
    end

    local sys = "You are adding texture to an NPC's daily routine in a "
             .. "realistic life-simulation RPG. The protagonist has noticed "
             .. "her do the SAME thing at this time repeatedly — propose ONE "
             .. "alternate activity she sometimes does instead, grounded in "
             .. "who she is and what's been happening to her lately. Only "
             .. "tie it to a stat (prob_boost_when) if there is a genuine "
             .. "psychological reason for THIS character. "
             .. "World: " .. _context
    local user = string.format(
        "NPC: %s (%s). Personality: %s\nCurrent summary: %s\n"
     .. "Usual routine slot being varied: %s-%s, %s.\n"
     .. "This NPC's existing stats (ONLY use one of these names if you use prob_boost_when): %s\n"
     .. "Recent life events:\n%s\n\nPropose the variation.",
        data.name or id, data.job or "?", data.personality or "—",
        (data.npc_summary and data.npc_summary ~= "") and data.npc_summary or "(none yet)",
        slot.time_from, slot.time_to, slot.activity or "?",
        #stat_names > 0 and table.concat(stat_names, ", ") or "(none)",
        #recent_lines > 0 and table.concat(recent_lines, "\n") or "  (none)")

    _jobs.submit("persona_variation_gen", sys, user, {
        schema = _VARIATION_SCHEMA, tier = "gen",
        meta = { npc_id = id, time_from = slot.time_from, time_to = slot.time_to, npc_obj = npc_obj },
    })
end

--- One-line appearance string for prompt/visual injection ("" if unknown).
-- Use in: think_as_npc situation, narrator system prompt, image prompts.
function M.format_appearance(id, time_str, day_str)
    local d      = _npcs[id]
    local name   = (d and d.name) or id
    local body   = d and d.appearance
    local outfit = M.current_outfit(id, time_str, day_str)
    if body and body ~= "" and outfit then
        return name .. ": " .. body .. " Indossa: " .. outfit
    elseif body and body ~= "" then
        return name .. ": " .. body
    elseif outfit then
        return name .. " indossa: " .. outfit
    end
    return ""
end

-- ── Formatting helpers ─────────────────────────────────────────────────────────

--- Format identity, conditions, and goals as a multi-line string for system prompt injection.
function M.format(id)
    local d = _npcs[id]
    if not d then return "(persona not found: " .. id .. ")" end
    local lines = {
        "Name: "      .. (d.name or id),
        "Age: "       .. tostring(d.age or "?"),
        "Lives at: "  .. (d.home or "?"),
        "Job: "       .. (d.job  or "?"),
        "Works at: "  .. (d.workplace or "?"),
        "Personality: " .. (d.personality or "?"),
    }
    if d.secret and d.secret ~= "" then
        table.insert(lines, "[secret] " .. d.secret)
    end
    if d.family and #d.family > 0 then
        local flines = {}
        for _, f in ipairs(d.family) do
            local line = (f.relation or "?") .. ": " .. (f.name or "?")
            if f.notes and f.notes ~= "" then line = line .. " (" .. f.notes .. ")" end
            table.insert(flines, line)
        end
        table.insert(lines, "Family: " .. table.concat(flines, "; "))
    end
    if d.relationships and next(d.relationships) then
        local rels = {}
        for k, v in pairs(d.relationships) do
            table.insert(rels, k .. ": " .. v)
        end
        table.sort(rels)
        table.insert(lines, "Relationships: " .. table.concat(rels, "; "))
    end
    if d.conditions and #d.conditions > 0 then
        table.insert(lines, "Conditions: " .. table.concat(d.conditions, ", "))
    end
    if d.carrying and #d.carrying > 0 then
        table.insert(lines, "Carrying: " .. table.concat(d.carrying, ", "))
    end
    if d.short_term_goals and #d.short_term_goals > 0 then
        table.insert(lines, "Goal: " .. table.concat(d.short_term_goals, "; "))
    end
    return table.concat(lines, "\n")
end

--- Format the daily routine as a multi-line string.
function M.format_routine(id)
    local d = _npcs[id]
    if not d or not d.routine then return "" end
    local lines = {}
    for _, r in ipairs(d.routine) do
        local loc = r.location_label or r.location_id or r.location or "?"
        table.insert(lines, string.format("  %s–%s  %-30s [%s]",
            r.time_from, r.time_to, r.activity, loc))
    end
    return table.concat(lines, "\n")
end

--- Validate a persona's routine: HH:MM format, location presence, and 24h
-- coverage per day (15-minute resolution — gaps shorter than 15 min are
-- ignored). Returns an array of human-readable issue strings (empty = OK).
-- This is the mechanical check for the most common NPC bug class:
-- routines with holes leave the NPC in limbo for part of the day.
function M.validate_routine(id)
    local d = _npcs[id]
    if not d then return { "persona not found: " .. tostring(id) } end
    local rt = d.routine or {}
    if #rt == 0 then return { "routine empty" } end
    local issues = {}

    local function tomin(hhmm)
        local h, m = tostring(hhmm or ""):match("^(%d+):(%d+)$")
        if not h then return nil end
        h, m = tonumber(h), tonumber(m)
        if h > 23 or m > 59 then return nil end
        return h * 60 + m
    end

    -- Format / location checks
    for i, r in ipairs(rt) do
        if not tomin(r.time_from) or not tomin(r.time_to) then
            table.insert(issues, string.format(
                "slot #%d: invalid time '%s-%s' (use HH:MM)",
                i, tostring(r.time_from), tostring(r.time_to)))
        end
        local lid = r.location_id or r.location
        if not lid or lid == "" then
            table.insert(issues, "slot #" .. i .. ": missing location")
        end
    end

    -- Which day labels are used? Slots without 'day' apply every day.
    local day_names, has_day_slots = {}, false
    for _, r in ipairs(rt) do
        if type(r.day) == "table" and #r.day > 0 then
            has_day_slots = true
            for _, dn in ipairs(r.day) do day_names[dn] = true end
        end
    end
    local days = {}
    if has_day_slots then
        for dn in pairs(day_names) do table.insert(days, dn) end
        table.sort(days)
    else
        days = { "*" }   -- single pass: every slot applies every day
    end

    -- Coverage per day on a 15-minute grid
    for _, day in ipairs(days) do
        local covered = {}
        for _, r in ipairs(rt) do
            local applies
            if type(r.day) ~= "table" or #r.day == 0 then
                applies = true
            else
                applies = false
                for _, dn in ipairs(r.day) do
                    if dn == day then applies = true; break end
                end
            end
            if applies then
                local f, t = tomin(r.time_from), tomin(r.time_to)
                if f and t then
                    if t <= f then  -- wraps midnight (e.g. 22:00-07:00)
                        for m = f, 1439, 15 do covered[m - m % 15] = true end
                        for m = 0, t - 1, 15 do covered[m - m % 15] = true end
                    else
                        for m = f, t - 1, 15 do covered[m - m % 15] = true end
                    end
                end
            end
        end
        local prefix = (day ~= "*") and (day .. ": ") or ""
        local gap_from = nil
        for m = 0, 1439, 15 do
            if not covered[m] then
                if not gap_from then gap_from = m end
            elseif gap_from then
                table.insert(issues, string.format("%sgap %02d:%02d-%02d:%02d",
                    prefix, math.floor(gap_from/60), gap_from%60,
                    math.floor(m/60), m%60))
                gap_from = nil
            end
        end
        if gap_from then
            table.insert(issues, string.format("%sgap %02d:%02d-24:00",
                prefix, math.floor(gap_from/60), gap_from%60))
        end
    end
    return issues
end

--- Format the last N life events as a bullet list.
function M.format_history(id, max_entries)
    local d = _npcs[id]
    if not d or not d.life_events or #d.life_events == 0 then return "" end
    max_entries = max_entries or 5
    local lines = {}
    local start = math.max(1, #d.life_events - max_entries + 1)
    for i = start, #d.life_events do
        local e = d.life_events[i]
        table.insert(lines, "- [" .. (e.date or "?") .. "] " .. (e.event or "?"))
    end
    return table.concat(lines, "\n")
end

-- ── Known facts & gossip ───────────────────────────────────────────────────────
-- Knowledge propagation between NPCs. Each persona carries a capped list of
-- facts it knows WITH PROVENANCE; facts travel on colocation (gossip) or via
-- ambient events (world.apply_ambient_result). Injected into the agent prompt
-- so what an NPC "knows" is grounded instead of hallucinated.

local _MAX_KNOWN_FACTS = 12

--- Add a fact this NPC knows. Dedup by exact text; FIFO cap.
-- @return true if added, false if duplicate / persona missing.
function M.add_known_fact(id, fact, source, date_str)
    local d = _npcs[id]
    if not d or type(fact) ~= "string" or fact == "" then return false end
    d.known_facts = d.known_facts or {}
    for _, f in ipairs(d.known_facts) do
        if f.fact == fact then return false end
    end
    table.insert(d.known_facts, {
        fact   = fact,
        source = source or "?",
        date   = date_str or "?",
        turn   = _current_turn,
    })
    while #d.known_facts > _MAX_KNOWN_FACTS do table.remove(d.known_facts, 1) end
    _write(d)
    return true
end

function M.known_facts(id)
    return (_npcs[id] and _npcs[id].known_facts) or {}
end

-- VN interface: NPC-specific verb-coin verbs (curated via npc_life_event).
function M.vn_verbs(id)
    return (_npcs[id] and _npcs[id].vn_verbs) or {}
end

-- VN interface: curated conversation topics for the "parla" submenu.
function M.topics(id)
    return (_npcs[id] and _npcs[id].topics) or {}
end

--- Gossip pass: with probability prob, to_id learns one random fact from
-- from_id. Call on colocation (e.g. inside the tick hook for due groups).
-- @return The transferred fact string, or nil if nothing happened.
function M.gossip(from_id, to_id, prob, date_str)
    local src, dst = _npcs[from_id], _npcs[to_id]
    if not src or not dst then return nil end
    if math.random() > (prob or 0.3) then return nil end
    local pool = src.known_facts or {}
    if #pool == 0 then return nil end
    local f = pool[math.random(#pool)]
    if M.add_known_fact(to_id, f.fact,
            "sentito da " .. (src.name or from_id), date_str) then
        return f.fact
    end
    return nil
end

--- Post-undo reconciliation: remove life_events and known_facts recorded at
-- a turn LATER than current_turn. Undo rewinds the save state but not the
-- persona files — without this, an undone turn leaves "future" events in the
-- NPCs' lives forever. Call from restore_state after an undo, or any time
-- state.turn moves backwards. Entries without a turn stamp are kept.
-- @return number of entries removed.
function M.prune_future_events(current_turn)
    current_turn = tonumber(current_turn)
    if not current_turn then return 0 end
    local removed = 0
    for _, d in pairs(_npcs) do
        local changed = false
        for _, field in ipairs({ "life_events", "known_facts" }) do
            local arr = d[field]
            if type(arr) == "table" then
                for i = #arr, 1, -1 do
                    local t = tonumber(arr[i].turn)
                    if t and t > current_turn then
                        table.remove(arr, i)
                        removed = removed + 1
                        changed = true
                    end
                end
            end
        end
        if changed then _write(d) end
    end
    return removed
end

-- ── Life event patch ───────────────────────────────────────────────────────────

--- Apply a structured patch to a persona and rewrite the .lua file on disk.
--
-- @param id     NPC identifier.
-- @param patch  Table with any of these optional fields:
--
--   date                   string   In-game date/turn label (e.g. "day 5").
--   event                  string   What happened — appended to life_events.
--   conditions_add         array    Strings to add to conditions.
--   conditions_remove      array    Strings to remove from conditions.
--   routine_replace        array    Full routine replacement (time_from/time_to/location/activity).
--   relationships_patch    object   key=role/person, value=description — merged into relationships.
--   agent_system_append    string   1-2 sentences appended to agent_system.
--   agent_system_replace   string   Full replacement of agent_system.
--   short_term_goals_replace array  Replaces short_term_goals entirely.
--   long_term_goals_add    array    Appended to long_term_goals.
--
-- @return { ok=true } or { ok=false, error="..." }
function M.patch(id, patch)
    local data = _npcs[id]
    if not data then
        return { ok=false, error="persona not found: " .. tostring(id) }
    end
    patch = patch or {}

    -- Append life event (with dedup)
    if patch.event then
        data.life_events = data.life_events or {}
        if not _event_is_duplicate(data.life_events, patch.event) then
            table.insert(data.life_events, {
                date  = patch.date  or "?",
                event = patch.event,
                turn  = _current_turn,
            })
        end
    end

    -- Conditions add
    if type(patch.conditions_add) == "table" then
        data.conditions = data.conditions or {}
        for _, c in ipairs(patch.conditions_add) do
            local found = false
            for _, existing in ipairs(data.conditions) do
                if existing == c then found = true; break end
            end
            if not found then table.insert(data.conditions, c) end
        end
    end

    -- Conditions remove
    if type(patch.conditions_remove) == "table" then
        data.conditions = data.conditions or {}
        for _, c in ipairs(patch.conditions_remove) do
            for i = #data.conditions, 1, -1 do
                if data.conditions[i] == c then table.remove(data.conditions, i) end
            end
        end
    end

    -- Routine replacement
    if type(patch.routine_replace) == "table" then
        data.routine = patch.routine_replace
    end

    -- Relationships merge
    if type(patch.relationships_patch) == "table" then
        data.relationships = data.relationships or {}
        for k, v in pairs(patch.relationships_patch) do
            data.relationships[k] = v
        end
    end

    -- Agent system update
    if type(patch.agent_system_replace) == "string" and patch.agent_system_replace ~= "" then
        data.agent_system = patch.agent_system_replace
    elseif type(patch.agent_system_append) == "string" and patch.agent_system_append ~= "" then
        data.agent_system = (data.agent_system or "") .. "\n" .. patch.agent_system_append
    end

    -- Secret replace
    if type(patch.secret_replace) == "string" and patch.secret_replace ~= "" then
        data.secret = patch.secret_replace
    end

    -- Carrying: descriptive items the NPC has with them (no mechanics —
    -- for systemic objects use world.set_holder / move_object instead)
    if type(patch.carrying_add) == "table" then
        data.carrying = data.carrying or {}
        for _, c in ipairs(patch.carrying_add) do
            local found = false
            for _, existing in ipairs(data.carrying) do
                if existing == c then found = true; break end
            end
            if not found then table.insert(data.carrying, c) end
        end
        while #data.carrying > 10 do table.remove(data.carrying, 1) end
    end
    if type(patch.carrying_remove) == "table" then
        data.carrying = data.carrying or {}
        for _, c in ipairs(patch.carrying_remove) do
            for i = #data.carrying, 1, -1 do
                if data.carrying[i] == c then table.remove(data.carrying, i) end
            end
        end
    end

    -- VN verb-coin: NPC-specific action verbs (visual-novel interface).
    -- These are EXTRA verbs surfaced when the player selects this NPC, on top of
    -- the generic palette (e.g. a guard → "perquisisci", a barista → "ordina").
    -- Pure interface metadata; the response is still produced by the agent.
    if type(patch.vn_verbs_add) == "table" then
        data.vn_verbs = data.vn_verbs or {}
        for _, v in ipairs(patch.vn_verbs_add) do
            local found = false
            for _, e in ipairs(data.vn_verbs) do if e == v then found = true; break end end
            if not found then table.insert(data.vn_verbs, v) end
        end
        while #data.vn_verbs > 12 do table.remove(data.vn_verbs, 1) end
    end
    if type(patch.vn_verbs_remove) == "table" then
        data.vn_verbs = data.vn_verbs or {}
        for _, v in ipairs(patch.vn_verbs_remove) do
            for i = #data.vn_verbs, 1, -1 do if data.vn_verbs[i] == v then table.remove(data.vn_verbs, i) end end
        end
    end

    -- VN conversation topics: specific subjects the player can bring up with this
    -- NPC (secrets surfacing, recent events, obsessions). Curated by the main LLM
    -- as the story unfolds. Surfaced in the "parla" submenu; the agent still
    -- decides how to react. Strings, deduped, capped.
    if type(patch.topics_add) == "table" then
        data.topics = data.topics or {}
        for _, t in ipairs(patch.topics_add) do
            local found = false
            for _, e in ipairs(data.topics) do if e == t then found = true; break end end
            if not found then table.insert(data.topics, t) end
        end
        while #data.topics > 12 do table.remove(data.topics, 1) end
    end
    if type(patch.topics_remove) == "table" then
        data.topics = data.topics or {}
        for _, t in ipairs(patch.topics_remove) do
            for i = #data.topics, 1, -1 do if data.topics[i] == t then table.remove(data.topics, i) end end
        end
    end

    -- Outfit override: set a temporary look, or clear it with "none"
    if type(patch.outfit_override) == "string" and patch.outfit_override ~= "" then
        if patch.outfit_override == "none" then
            data.outfit_override = nil
        else
            data.outfit_override = patch.outfit_override
        end
    end

    -- Family: add new members (no duplicates by name)
    if type(patch.family_add) == "table" then
        data.family = data.family or {}
        for _, new_member in ipairs(patch.family_add) do
            local exists = false
            for _, m in ipairs(data.family) do
                if m.name == new_member.name then exists = true; break end
            end
            if not exists then table.insert(data.family, new_member) end
        end
    end

    -- Goals
    if type(patch.event_reactions_replace) == "table" then
        data.event_reactions = patch.event_reactions_replace
    end

    if type(patch.short_term_goals_replace) == "table" then
        data.short_term_goals = patch.short_term_goals_replace
    end
    if type(patch.long_term_goals_replace) == "table" then
        data.long_term_goals = patch.long_term_goals_replace
    end
    if type(patch.long_term_goals_add) == "table" then
        data.long_term_goals = data.long_term_goals or {}
        for _, g in ipairs(patch.long_term_goals_add) do
            table.insert(data.long_term_goals, g)
        end
    end

    -- Direct identity-field edits (GUI editor). Whitelisted scalar fields only —
    -- structural fields (routine, life_events, etc.) have dedicated patch keys
    -- above so they can't be clobbered by a stray write.
    if type(patch.fields) == "table" then
        local EDITABLE = { name=true, age=true, job=true, home=true, workplace=true,
                           appearance=true, personality=true, npc_summary=true }
        for k, v in pairs(patch.fields) do
            if EDITABLE[k] then data[k] = v end
        end
    end

    local ok = _write(data)
    return ok
        and { ok=true }
        or  { ok=false, error="could not write: " .. _base_path .. id .. ".lua" }
end

-- ── Tool factories ─────────────────────────────────────────────────────────────

--- ToolDef: generate a new NPC persona file.
-- The main LLM calls this when the player encounters someone new.
function M.as_tool_generate(description)
    return {
        name        = "generate_npc",
        description = description or
            "Generate a new NPC the player is meeting for the first time. "
            .. "Creates a persistent persona file with full identity, daily routine, "
            .. "and agent config. Call ONCE per new character before narrating them. "
            .. "PRIMA controlla i personaggi già noti (statici + generati, mostrati nello "
            .. "stato/prompt): se il ruolo che ti serve esiste già (es. 'il vicario', 'il prete', "
            .. "'l'amante di Cettina') NON crearne uno nuovo — usa l'id esistente. Un secondo "
            .. "personaggio con nome/ruolo quasi identico a uno già noto confonde la storia. "
            .. "OBBLIGATORI 'id' (snake_case) e 'context' — MAI chiamare senza argomenti. "
            .. "Esempio: generate_npc(id=\"toto_pescatore\", "
            .. "context=\"pescatore 50enne, amante segreto di Carmela, vive al porto, burbero\"). "
            .. "Returns the NPC profile for use in narration.",
        params      = [[{
            "type": "object",
            "required": ["id", "context"],
            "properties": {
                "id": {
                    "type": "string",
                    "description": "Unique snake_case id (e.g. 'marco_203'). Must be stable — do not change after creation."
                },
                "context": {
                    "type": "string",
                    "description": "Who this person is: role, rough age, where they live, relationship to the world, personality hints."
                }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local id = _sanitize_id(a.id)
            if not id then
                return json.encode({ error="id e context OBBLIGATORI. Esempio: "
                    .. "generate_npc(id=\"toto_pescatore\", context=\"pescatore 50enne, "
                    .. "vive al porto, burbero\"). Riprova con argomenti completi." })
            end
            if _npcs[id] then
                return json.encode({ ok=true, already_exists=true,
                                     id=id, summary=M.format(id) })
            end
            local data, gen_err = M.generate(id, a.context)
            if not data then
                -- Actionable, never bare: state the reason and the ONE thing to
                -- change, or the narrator retries identically / mutates the id.
                return json.encode({ error = "generation failed: "
                    .. tostring(gen_err or "invalid data") .. ". "
                    .. "MANTIENI lo stesso id '" .. id .. "'. Riprova UNA sola volta "
                    .. "arricchendo il context (chi è, età, dove vive, giornata tipo, "
                    .. "carattere). Se fallisce ancora: NON generare, narra il "
                    .. "personaggio senza scheda e prosegui." })
            end
            return json.encode({
                ok            = true,
                id            = data.id,
                name          = data.name,
                age           = data.age,
                job           = data.job,
                personality   = data.personality,
                conditions    = data.conditions,
                relationships = data.relationships,
            })
        end,
    }
end

--- ToolDef: record a significant life event and patch the NPC's persona file.
-- The main LLM calls this when something important happens to an NPC.
function M.as_tool_life_event(description)
    return {
        name        = "npc_life_event",
        description = description or
            "Record a significant life event for an NPC and update their persona permanently. "
            .. "Use for: injury, arrest, job change, marriage, moving, relationship change. "
            .. "The persona file on disk is updated immediately — changes persist across sessions. "
            .. "Call only for events that meaningfully change who the NPC is or what they do.",
        params      = [[{
            "type": "object",
            "required": ["id", "date", "event"],
            "properties": {
                "id":    { "type": "string", "description": "NPC identifier" },
                "date":  { "type": "string", "description": "In-game date or label (e.g. 'day 5')" },
                "event": { "type": "string", "description": "What happened — one clear sentence" },
                "conditions_add": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Active conditions to add (e.g. ['gamba rotta', 'ospedalizzato'])"
                },
                "conditions_remove": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Conditions that are now resolved"
                },
                "routine_replace": {
                    "type": "array",
                    "description": "Full new routine if daily life changes significantly",
                    "items": {
                        "type": "object",
                        "required": ["time_from","time_to","location","activity"],
                        "properties": {
                            "time_from": { "type": "string" },
                            "time_to":   { "type": "string" },
                            "location":  { "type": "string" },
                            "activity":  { "type": "string" }
                        }
                    }
                },
                "relationships_patch": {
                    "type": "object",
                    "description": "New or updated relationships (key=role, value=description)",
                    "additionalProperties": { "type": "string" }
                },
                "agent_system_append": {
                    "type": "string",
                    "description": "1-2 sentences to append to this NPC's agent prompt. Use for persistent facts the agent must always know (e.g. 'Hai la gamba ingessata. Non riesci a camminare.')."
                },
                "short_term_goals_replace": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Replace current short-term goals entirely"
                },
                "long_term_goals_add": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Append to long-term goals"
                },
                "secret_replace": {
                    "type": "string",
                    "description": "Replace the NPC's secret with a new one. Use only if the secret has changed or been revealed."
                },
                "outfit_override": {
                    "type": "string",
                    "description": "Temporary outfit override (e.g. 'abito elegante nero' for a dinner). Pass 'none' to clear and return to the routine outfit."
                },
                "carrying_add": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Items the NPC now carries with them (descriptive, e.g. 'borsa di pelle marrone', 'pacco avvolto in carta di giornale')."
                },
                "carrying_remove": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Items the NPC no longer carries (exact strings)."
                },
                "vn_verbs_add": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Visual-novel interface: NPC-specific action verbs to offer the player for this character, beyond generic talk/give (e.g. ['perquisisci'] for a guard, ['ordina'] for a barista)."
                },
                "vn_verbs_remove": {
                    "type": "array", "items": { "type": "string" },
                    "description": "VN action verbs to remove for this NPC (exact strings)."
                },
                "topics_add": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Conversation topics the player can raise with this NPC (e.g. 'il furto di ieri', 'sua sorella'). Add as the story makes them relevant; the NPC's agent still decides how to react."
                },
                "topics_remove": {
                    "type": "array", "items": { "type": "string" },
                    "description": "Conversation topics no longer relevant (exact strings)."
                },
                "family_add": {
                    "type": "array",
                    "description": "Add new family members discovered during the story.",
                    "items": {
                        "type": "object",
                        "required": ["name","relation"],
                        "properties": {
                            "name":     { "type": "string" },
                            "relation": { "type": "string" },
                            "notes":    { "type": "string" }
                        }
                    }
                }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local id = _sanitize_id(a.id)
            if not id then
                return json.encode({ error="id is required" })
            end
            if not _npcs[id] then
                return json.encode({ error="NPC '" .. id .. "' non esiste — crealo PRIMA con "
                    .. "generate_npc(id=\"" .. id .. "\", context=\"<chi è>\"), "
                    .. "POI richiama npc_life_event." })
            end
            a.id = id
            local result = M.patch(id, a)
            if result.ok then
                local d = _npcs[id]
                -- Count toward stage 3 routine crystallization — a real,
                -- narratively significant event is exactly as strong a
                -- signal as a dialogue turn (see M._track_interaction).
                local ok, err = pcall(M._track_interaction, id, "life event: " .. tostring(a.event or ""))
                if not ok then _log_err("[" .. id .. "] track_interaction: " .. tostring(err)) end
                return json.encode({
                    ok                = true,
                    id                = id,
                    conditions        = d.conditions,
                    life_events_count = d.life_events and #d.life_events or 0,
                })
            end
            return json.encode(result)
        end,
    }
end

return M
