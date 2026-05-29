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

local json = require("json")

local M = {}

-- ── Registry ───────────────────────────────────────────────────────────────────

local _npcs      = {}    -- id -> data table (in-memory mirror of .lua files)
local _base_path = "./scripts/npcs/"
local _context   = ""
local _model     = nil
local _provider  = nil

-- Dream growth caps (configurable via persona.init opts)
local _max_sequences       = 5
local _max_needs           = 3
local _max_event_reactions = 6
local _max_stats           = 4

local _default_template    = nil   -- set via opts.default_template in M.init()

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
    os.execute('mkdir -p "' .. _base_path .. '"')
end

-- ── Lua table serializer ───────────────────────────────────────────────────────
-- Converts a data table to a Lua source string.
-- Supports: nil, boolean, number, string, nested tables.
-- Output is deterministic (keys sorted alphabetically).

local function _serialize(val, indent)
    indent = indent or ""
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
    "id", "name", "age", "job", "home", "workplace",
    "personality", "secret",
    "life_events",
    "relationships", "family", "conditions",
    "routine", "stats_defaults", "event_reactions",
    "agent_system", "npc_summary", "short_term_goals", "long_term_goals",
    -- Template-provided base (set at generation time, stable across sessions):
    "state_phrases", "inactive_behaviors",
    -- Dream-grown behavioral additions (accumulated nightly):
    "dream_count", "npc_stats", "npc_sequences", "npc_needs", "npc_event_reactions",
    "dream_log",
}

local function _write(data)
    local path = _base_path .. data.id .. ".lua"
    local f = io.open(path, "w")
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
    return true
end

local function _load_file(id)
    local path = _base_path .. id .. ".lua"
    local ok, chunk = pcall(loadfile, path)
    if not ok or not chunk then return nil end
    local ok2, data = pcall(chunk)
    if not ok2 or type(data) ~= "table" or not data.id then return nil end
    return data
end

-- ── File loading ───────────────────────────────────────────────────────────────

--- Load a single NPC file into the registry.
-- @param id  NPC identifier (= filename without .lua).
-- @return    Data table or nil.
function M.load(id)
    local data = _load_file(id)
    if data then _npcs[data.id] = data end
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

-- ── LLM generation ─────────────────────────────────────────────────────────────

local _SCHEMA = [[{
  "type": "object",
  "required": ["id","name","age","job","home","workplace","personality","secret","relationships","family","routine",
               "stats_defaults","event_reactions","agent_system",
               "short_term_goals","long_term_goals"],
  "properties": {
    "id":           { "type": "string" },
    "name":         { "type": "string" },
    "age":          { "type": "integer", "minimum": 5, "maximum": 99 },
    "job":          { "type": "string" },
    "home":         { "type": "string", "description": "Apartment identifier and floor, e.g. 'Appartamento 202, secondo piano'. Used to locate the NPC in the building." },
    "workplace":    { "type": "string", "description": "Where this NPC works or spends most daytime hours outside the building. E.g. 'Manzoni middle school, via Roma 12' or 'retired, does not work'." },
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
    local existing = _load_file(id)
    if existing then
        _npcs[id] = existing
        return existing
    end

    -- Load template (specific > default > nil)
    local tmpl_name = template_name or _default_template
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

    local sys = "You are a character generator for a life-simulation RPG in the Italian comedy drama style. "
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
             .. "Use the 'day' field to differentiate weekday vs weekend behaviour: "
             .. "work/school slots get day=[\"lunedì\",\"martedì\",\"mercoledì\",\"giovedì\",\"venerdì\"], "
             .. "weekend leisure slots get day=[\"sabato\",\"domenica\"]. "
             .. "Slots valid every day (sleep, meals at home) omit 'day' entirely. "
             .. "Cover both weekday AND weekend versions of the daytime block. "
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
             .. (tmpl and tmpl.prompt_hint and ("\n\nTEMPLATE GUIDANCE: " .. tmpl.prompt_hint) or "")

    local user = "Generate the character id='" .. id .. "'. Context: " .. (context or id)

    local schema = _SCHEMA:gsub(
        "Snake_case world id: {npc_id}_{room}, e%.g%. 'elena_302_bagno', 'elena_302_camera'%. Same physical room = same id always%.",
        "Snake_case world id: {prefix}_{room} where prefix='" .. loc_pfx .. "', e.g. '"
            .. loc_pfx .. "_cucina', '" .. loc_pfx .. "_camera'. "
            .. "NEVER use the NPC id as prefix — always use '" .. loc_pfx .. "'. Same physical room = same id always.")
    local ok, raw = pcall(query_llm, sys, "[]", user, schema, _model, _provider)
    if not ok then return nil end

    local ok2, data = pcall(json.decode, raw)
    if not ok2 or type(data) ~= "table" or not data.name then return nil end

    -- Sanity-check against RAG contamination: reject if the output looks like
    -- it mixed in content from RAG examples (English boilerplate, wrong IDs, etc.)
    local function _generation_looks_valid(d)
        -- agent_system must be a real character voice, not a placeholder
        if type(d.agent_system) ~= "string" or #d.agent_system < 80 then
            return false, "agent_system too short or missing"
        end
        -- routine must exist and location_ids must follow the {id}_{room} convention
        local rt = d.routine or {}
        if #rt < 4 then
            return false, "routine insufficient (" .. #rt .. " slots)"
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
        return true, "ok"
    end

    local valid, reason = _generation_looks_valid(data)
    if not valid then
        -- Log the rejection reason; caller will see nil and can retry
        pcall(function()
            local f = io.open("/tmp/persona_generate_reject.log", "a")
            if f then
                f:write(os.date("%Y-%m-%d %H:%M:%S") .. " [" .. id .. "] REJECTED: " .. reason .. "\n")
                f:close()
            end
        end)
        return nil
    end

    data.id         = id
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
        -- inactive_behaviors: full pool from template (dream will activate entries over time)
        data.inactive_behaviors = {}
        for _, b in ipairs(tmpl.inactive_behaviors or {}) do
            table.insert(data.inactive_behaviors, b)
        end
    else
        data.state_phrases      = data.state_phrases      or {}
        data.inactive_behaviors = data.inactive_behaviors or {}
    end

    _npcs[id] = data
    _write(data)

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

local _DREAM_SCHEMA = [[{
  "type": "object",
  "required": ["dream_narrative","aspect_developed","life_event_summary","addition_type"],
  "properties": {
    "dream_narrative":    { "type": "string", "description": "2-3 sentences describing the dream." },
    "aspect_developed":   { "type": "string", "description": "Psychological aspect that emerged (e.g. 'fear of loneliness', 'desire for freedom')." },
    "life_event_summary": { "type": "string", "description": "One-line summary for life_events, prefixed with [dream]." },
    "npc_summary_update": { "type": "string", "description": "Update who this character is TODAY in 2-3 sentences, including recent developments. Must reflect evolution beyond the initial profile." },
    "day_summary":        { "type": "string", "description": "OPTIONAL. If there are 5+ recent events, compress the NON-dream events into a single sentence starting with '[compressed]'. Omit if there are few events." },
    "addition_type": { "type": "string", "enum": ["sequence","need","event_reaction","stat"] },
    "sequence": {
      "type": "object",
      "properties": {
        "name":  { "type": "string", "description": "Snake_case name (e.g. 'seeks_comfort')." },
        "steps": {
          "type": "array", "minItems": 1, "maxItems": 4,
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
    },
    "need": {
      "type": "object",
      "properties": {
        "stat":          { "type": "string", "description": "Name of the stat that triggers the need." },
        "threshold":     { "type": "number", "minimum": 0.1, "maximum": 1.0 },
        "description":   { "type": "string", "description": "What the NPC does when the threshold is reached." },
        "sequence_name": { "type": "string", "description": "Name of an existing sequence to link to." }
      }
    },
    "event_reaction": {
      "type": "object",
      "properties": {
        "event_name":     { "type": "string", "description": "Snake_case event name to react to." },
        "activity":       { "type": "string" },
        "narrative_hint": { "type": ["string","null"] },
        "stats":          { "type": "object", "additionalProperties": { "type": "number" } }
      }
    },
    "stat": {
      "type": "object",
      "properties": {
        "name":        { "type": "string", "description": "Snake_case stat name (e.g. 'loneliness')." },
        "initial":     { "type": "number", "minimum": 0.0, "maximum": 1.0 },
        "description": { "type": "string" }
      }
    }
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

--- Run a nightly dream for an NPC: deepens psychology, adds behavioral code.
-- Call from after_ai_turn when NPC is in sleep slot and hasn't dreamed today.
-- Returns { narrative, aspect, life_event, addition_type, additions } or nil on failure.
-- @param id        NPC identifier.
-- @param date_str  In-game date label (e.g. "day 3").
function M.dream(id, date_str)
    local data = _npcs[id]
    if not data then return nil end

    -- Count current additions
    local seq_count = 0; for _ in pairs(data.npc_sequences or {}) do seq_count = seq_count + 1 end
    local nd_count  = #(data.npc_needs or {})
    local er_count  = 0; for _ in pairs(data.npc_event_reactions or {}) do er_count = er_count + 1 end
    local st_count  = 0; for _ in pairs(data.npc_stats or {}) do st_count = st_count + 1 end

    -- If all caps hit, just record a quiet night
    if seq_count >= _max_sequences and nd_count >= _max_needs
       and er_count >= _max_event_reactions and st_count >= _max_stats then
        local ev = "[dream] quiet night, dreams without echo."
        data.life_events = data.life_events or {}
        if not _event_is_duplicate(data.life_events, ev) then
            table.insert(data.life_events, { date=date_str, event=ev })
        end
        data.dream_count = (data.dream_count or 0) + 1
        _write(data)
        return { narrative="quiet night", aspect="rest", life_event=ev,
                 addition_type=nil, additions={} }
    end

    -- Build list of available addition types
    local available = {}
    if seq_count  < _max_sequences       then table.insert(available, "sequence") end
    if er_count   < _max_event_reactions then table.insert(available, "event_reaction") end
    if st_count   < _max_stats           then table.insert(available, "stat") end
    -- 'need' requires at least one sequence to reference
    local seq_keys = {}
    for k in pairs(data.npc_sequences or {}) do table.insert(seq_keys, k) end
    if nd_count < _max_needs and #seq_keys > 0 then
        table.insert(available, "need")
    end
    if #available == 0 then return nil end

    -- Check inactive_behaviors pool: prefer activating a pre-defined behavior over free invention.
    -- Pick the first candidate whose type matches an available slot.
    local ib_pool     = data.inactive_behaviors or {}
    local ib_candidate = nil
    local ib_index    = nil
    for i, b in ipairs(ib_pool) do
        for _, avail in ipairs(available) do
            if b.type == avail then
                ib_candidate = b
                ib_index     = i
                break
            end
        end
        if ib_candidate then break end
    end

    -- Build recent life events context
    local recent_evs = {}
    local evs = data.life_events or {}
    for i = math.max(1, #evs - 4), #evs do
        table.insert(recent_evs, "  - " .. (evs[i].date or "?") .. ": " .. (evs[i].event or "?"))
    end

    local sys = "You are a dream system for an NPC in a realistic life-simulation RPG. "
             .. "Generate a meaningful dream that reveals or deepens the NPC's psychology. "
             .. "The dream must produce ONE single structural change to behaviour. "
             .. "Available types: " .. table.concat(available, ", ") .. ". "
             .. (#seq_keys > 0 and ("Existing sequences (for need): " .. table.concat(seq_keys, ", ") .. ". ") or "")
             .. "For location_id in sequences use the format {npc_id}_{room}. "
             .. "World: " .. _context
             .. (ib_candidate and (
                    "\n\nGUIDED BEHAVIOR (preferred): The dream should activate this behavior — "
                 .. "type: " .. ib_candidate.type
                 .. ", suggested name: " .. (ib_candidate.name or "auto")
                 .. ", description: " .. (ib_candidate.hint or "")
                 .. (ib_candidate.trigger_stat and (". Triggered when stat '" .. ib_candidate.trigger_stat .. "' is high.") or "")
                 .. " Use this as the core of the dream narrative and the addition_type output."
                ) or "")

    local user = string.format(
        "NPC: %s (%d years old, %s).\nPersonality: %s\nConditions: %s\nShort-term goal: %s\nRecent events:\n%s\n\nGenerate the dream.",
        data.name or id,
        data.age or 0,
        data.job or "unknown",
        data.personality or "—",
        table.concat(data.conditions or {}, ", "),
        table.concat(data.short_term_goals or {}, "; "),
        #recent_evs > 0 and table.concat(recent_evs, "\n") or "  (none)"
    )

    local ok, raw = pcall(query_llm, sys, "[]", user, _DREAM_SCHEMA, _model, _provider)
    if not ok then return nil end
    local ok2, dream = pcall(json.decode, raw)
    if not ok2 or type(dream) ~= "table" or not dream.dream_narrative then return nil end

    -- Apply structural addition
    local add_type = dream.addition_type
    local additions = {}

    if add_type == "sequence" and dream.sequence and dream.sequence.name
       and type(dream.sequence.steps) == "table" and seq_count < _max_sequences then
        data.npc_sequences = data.npc_sequences or {}
        data.npc_sequences[dream.sequence.name] = dream.sequence.steps
        additions.sequence = dream.sequence

    elseif add_type == "need" and dream.need and dream.need.stat
       and dream.need.sequence_name and nd_count < _max_needs
       and (data.npc_sequences or {})[dream.need.sequence_name] then
        data.npc_needs = data.npc_needs or {}
        table.insert(data.npc_needs, dream.need)
        additions.need = dream.need

    elseif add_type == "event_reaction" and dream.event_reaction
       and dream.event_reaction.event_name and er_count < _max_event_reactions then
        data.npc_event_reactions = data.npc_event_reactions or {}
        data.npc_event_reactions[dream.event_reaction.event_name] = dream.event_reaction
        additions.event_reaction = dream.event_reaction

    elseif add_type == "stat" and dream.stat and dream.stat.name
       and st_count < _max_stats then
        data.npc_stats = data.npc_stats or {}
        data.npc_stats[dream.stat.name] = dream.stat.initial or 0.3
        additions.stat = dream.stat
    end

    -- If an inactive_behaviors candidate was used, remove it from the pool
    if ib_candidate and ib_index and (
        (add_type == "sequence"       and additions.sequence)       or
        (add_type == "need"           and additions.need)           or
        (add_type == "event_reaction" and additions.event_reaction) or
        (add_type == "stat"           and additions.stat)
    ) then
        table.remove(data.inactive_behaviors, ib_index)
    end

    -- Update npc_summary if provided
    if type(dream.npc_summary_update) == "string" and dream.npc_summary_update ~= "" then
        data.npc_summary = dream.npc_summary_update
    end

    -- Compress previous day events if day_summary provided and there are enough events
    if type(dream.day_summary) == "string" and dream.day_summary ~= "" then
        local evs = data.life_events or {}
        -- Find events from previous day (non-dream, non-compressed) and replace with summary
        local to_keep = {}
        local prev_day_events = {}
        for _, ev in ipairs(evs) do
            local is_dream    = (ev.event or ""):match("^%[dream%]")
            local is_compress = (ev.event or ""):match("^%[compressed%]")
            local is_today    = (ev.date or ""):find(date_str, 1, true)
            if is_dream or is_compress or is_today then
                table.insert(to_keep, ev)
            else
                table.insert(prev_day_events, ev)
            end
        end
        if #prev_day_events >= 5 then
            -- Extract day label from first prev event date for the compressed entry
            local day_label = (prev_day_events[1].date or "day ?"):match("day %d+") or "?"
            table.insert(to_keep, 1, {
                date  = day_label,
                event = dream.day_summary:match("^%[compressed%]") and dream.day_summary
                        or ("[compressed] " .. dream.day_summary),
            })
            data.life_events = to_keep
        end
    end

    -- Record dream as life event (with dedup)
    local ev_summary = dream.life_event_summary or ("[dream] " .. (dream.aspect_developed or "—"))
    data.life_events = data.life_events or {}
    if not _event_is_duplicate(data.life_events, ev_summary) then
        table.insert(data.life_events, { date=date_str, event=ev_summary })
    end
    data.dream_count = (data.dream_count or 0) + 1

    -- Dream log entry (persisted + written as file header comments)
    data.dream_log = data.dream_log or {}
    local addition_name = (add_type == "sequence"       and dream.sequence       and dream.sequence.name)
                       or (add_type == "need"           and dream.need           and dream.need.stat)
                       or (add_type == "event_reaction" and dream.event_reaction and dream.event_reaction.event_name)
                       or (add_type == "stat"           and dream.stat           and dream.stat.name)
                       or nil
    table.insert(data.dream_log, {
        date          = date_str,
        aspect        = dream.aspect_developed,
        narrative     = dream.dream_narrative,
        addition_type = add_type,
        addition_name = addition_name,
    })

    _write(data)

    return {
        narrative     = dream.dream_narrative,
        aspect        = dream.aspect_developed,
        life_event    = ev_summary,
        addition_type = add_type,
        additions     = additions,
    }
end

--- Apply dream additions to a live npc.lua object's config (in-memory only).
-- Called by dream_tick after a successful dream; also usable directly.
local function _apply_additions(npc_obj, additions)
    if not npc_obj or not additions then return end
    local adds = additions

    if adds.sequence and adds.sequence.name and type(adds.sequence.steps) == "table" then
        npc_obj.config.sequences = npc_obj.config.sequences or {}
        local converted = {}
        for _, s in ipairs(adds.sequence.steps) do
            table.insert(converted, {
                location       = s.location_id or s.location,
                activity       = s.activity,
                narrative_hint = s.narrative_hint,
                stats          = s.stats or {},
            })
        end
        npc_obj.config.sequences[adds.sequence.name] = converted
    end

    if adds.need and adds.need.stat then
        npc_obj.config.needs = npc_obj.config.needs or {}
        table.insert(npc_obj.config.needs, {
            stat      = adds.need.stat,
            threshold = adds.need.threshold,
            options   = {{ condition={}, sequence=adds.need.sequence_name,
                           description=adds.need.description }},
        })
    end

    if adds.event_reaction and adds.event_reaction.event_name then
        npc_obj.config.event_reactions = npc_obj.config.event_reactions or {}
        npc_obj.config.event_reactions[adds.event_reaction.event_name] = {
            activity       = adds.event_reaction.activity,
            narrative_hint = adds.event_reaction.narrative_hint,
            stats          = adds.event_reaction.stats or {},
        }
    end

    if adds.stat and adds.stat.name then
        npc_obj.config.stats_defaults = npc_obj.config.stats_defaults or {}
        npc_obj.config.stats_defaults[adds.stat.name] = adds.stat.initial or 0.3
        if not npc_obj.stats[adds.stat.name] then
            npc_obj.stats[adds.stat.name] = adds.stat.initial or 0.3
        end
    end
end

--- Nightly dream tick — call from after_ai_turn.
-- Picks one known persona that hasn't dreamed today and runs M.dream() for it.
-- Mutates last_dream[id] = day_index on success.
-- Returns { id, result } if a dream happened, nil otherwise.
--
-- @param time_str   Current in-game time, e.g. "02:30".
-- @param day_index  Integer day counter (used as guard key and date label).
-- @param last_dream Mutable table { npc_id → day_index_of_last_dream }.
-- @param npc_objects Optional { npc_id → npc.lua object } to patch live config.
--                    Personas not in npc_objects still get their file updated.
function M.dream_tick(time_str, day_index, last_dream, npc_objects)
    -- Only run in deep-sleep window 01:00-05:00
    local h = tonumber((time_str or "12:00"):match("^(%d+)")) or 12
    if h < 1 or h > 5 then return nil end

    npc_objects = npc_objects or {}
    last_dream  = last_dream  or {}

    for id in pairs(_npcs) do
        if last_dream[id] ~= day_index then
            local date_str = "day " .. tostring(day_index)
            local result   = M.dream(id, date_str)
            if result then
                last_dream[id] = day_index
                _apply_additions(npc_objects[id], result.additions)
                return { id=id, result=result }
            end
        end
    end
    return nil
end

-- ── Registry access ────────────────────────────────────────────────────────────

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
    for _, n in ipairs(data.npc_needs or {}) do
        table.insert(config_needs, {
            stat      = n.stat,
            threshold = n.threshold,
            options   = {{ condition={}, sequence=n.sequence_name, description=n.description }},
        })
    end

    local config_er = {}
    for ev_name, reaction in pairs(data.npc_event_reactions or {}) do
        config_er[ev_name] = {
            activity       = reaction.activity,
            narrative_hint = reaction.narrative_hint,
            stats          = reaction.stats or {},
        }
    end

    local base_er = data.event_reactions or {}
    for k, v in pairs(config_er) do base_er[k] = v end

    local base_stats = {}
    for k, v in pairs(data.stats_defaults or {}) do base_stats[k] = v end
    for k, v in pairs(data.npc_stats or {}) do base_stats[k] = v end

    local first_name = (data.name or id):match("^(%S+)") or id
    return NPC_lib.new(data.name or id, {
        stats_defaults  = base_stats,
        idle_activity   = first_name .. " is still, lost in thought.",
        routine         = routine,
        needs           = config_needs,
        sequences       = config_seqs,
        event_reactions = base_er,
        memory_size     = 30,
    }, world_adapter)
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
    if data.conditions and #data.conditions > 0 then
        sys = sys .. "\n\nCurrent conditions: " .. table.concat(data.conditions, ", ") .. "."
    end
    if data.short_term_goals and #data.short_term_goals > 0 then
        sys = sys .. "\nImmediate goal: " .. table.concat(data.short_term_goals, "; ") .. "."
    end

    -- Knowledge fence (C): explicit limits to prevent hallucination
    sys = sys .. [[

KNOWLEDGE LIMITS (HARD RULE):
- You only know what is in your life_events and your direct interactions with people.
- You do not know the player's thoughts, private notes, or plans.
- You do not know other residents' secrets unless they told you directly.
- Do not invent details about people you have never met in person.
- If you have no information about something, say openly "I don't know" or "I'm not aware of that".
- Do not anticipate future events or summarise things you were not explicitly told.]]

    return agent_lib.new(id, {
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
    if type(patch.short_term_goals_replace) == "table" then
        data.short_term_goals = patch.short_term_goals_replace
    end
    if type(patch.long_term_goals_add) == "table" then
        data.long_term_goals = data.long_term_goals or {}
        for _, g in ipairs(patch.long_term_goals_add) do
            table.insert(data.long_term_goals, g)
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
            if not a.id or a.id == "" then
                return json.encode({ error="id is required" })
            end
            if _npcs[a.id] then
                return json.encode({ ok=true, already_exists=true,
                                     id=a.id, summary=M.format(a.id) })
            end
            local data = M.generate(a.id, a.context)
            if not data then
                return json.encode({ error="generation failed — LLM returned invalid data. Retry." })
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
            if not a.id or a.id == "" then
                return json.encode({ error="id is required" })
            end
            if not _npcs[a.id] then
                return json.encode({ error="NPC '" .. a.id .. "' not found — call generate_npc first" })
            end
            local result = M.patch(a.id, a)
            if result.ok then
                local d = _npcs[a.id]
                return json.encode({
                    ok                = true,
                    id                = a.id,
                    conditions        = d.conditions,
                    life_events_count = d.life_events and #d.life_events or 0,
                })
            end
            return json.encode(result)
        end,
    }
end

return M
