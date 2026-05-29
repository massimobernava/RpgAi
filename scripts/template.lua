-- =============================================================================
--  template.lua  —  RpgAi Adventure Template
--
--  HOW TO USE THIS FILE:
--    1. Copy to scripts/my_adventure.lua
--    2. Follow the [STEP N] markers top to bottom
--    3. Delete any OPTIONAL block you don't need
--    4. Run:  ./build/rpgai --web --provider ollama --model llama3.2 \
--                           --path scripts/ --script my_adventure.lua
--
--  INCLUDED FEATURES (all optional except REQUIRED markers):
--    - Locations + travel map
--    - Items / inventory
--    - Time + day cycle
--    - Code-driven NPCs  (lib/npc.lua)
--    - LLM-driven agents (lib/agent.lua) with shared turn counter
--    - Persistent memory across sessions (lib/memory.lua)
--    - Tool calling: dice, skill check, memory read/write, custom tools
--    - Image system (t2i backgrounds + NPC portraits)
--    - Hooks: before_ai_turn, after_ai_turn
--    - Custom /commands
-- =============================================================================

-- ── REQUIRED ─────────────────────────────────────────────────────────────────
local json        = require("json")
-- ── OPTIONAL: native GUI visual world (rpgai-gui only) ───────────────────────
-- local visual = require("lib/visual")  -- tile map + NPC sprites for rpgai-gui
-- ── OPTIONAL: uncomment as needed ────────────────────────────────────────────
-- local json_repair = require("json_repair")   -- auto-repairs broken LLM JSON
-- local tools_lib   = require("tools")         -- pre-built tool definitions
-- local NPC_lib     = require("npc")           -- code-driven NPC engine
-- local agent       = require("agent")         -- LLM-driven NPC reactions
-- local memory      = require("memory")        -- cross-session persistent memory
-- local world       = require("lib/world")     -- procedural world expansion (NPC/location/object on-demand)
-- ─────────────────────────────────────────────────────────────────────────────

-- ── OPTIONAL: per-turn tool call guard (MODE B / C) ──────────────────────────
-- Prevents the LLM from calling the same tool more than once per turn.
-- Pattern: check _tool_calls[name], set it, return error if already set.
-- Reset the table at the start of every turn in before_ai_turn.
-- Each tool fn adds one line: see [TOOL CALL GUARD] comments in [STEP 7].
-- local _tool_calls = {}
-- ─────────────────────────────────────────────────────────────────────────────


-- =============================================================================
-- [STEP 1] TITLE AND SETTINGS
-- =============================================================================

local SCRIPT_NAME  = "template"     -- used for memory file: template_memory.json
local SCRIPT_PATH  = "./scripts/"   -- base path for memory file and images

-- [STEP 1 — OPTIONAL] Uncomment if using memory.lua
-- memory.init(SCRIPT_NAME, SCRIPT_PATH)


-- =============================================================================
-- [STEP 2] WORLD DATA
-- Define locations, items and static NPC configs here.
-- Keep static (never changes at runtime). Mutable state lives in `state` below.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- Locations
-- Each key is a location_id. The schema enum must match these keys exactly.
-- ---------------------------------------------------------------------------
local LOCATIONS = {
    forest_path = {
        name = "Forest Path",
        desc = "A dirt path winding between ancient oaks. "
             .. "Dappled sunlight filters through the canopy. "
             .. "A stone bridge is visible to the east. "
             .. "The village lies to the west.",
    },
    stone_bridge = {
        name = "Stone Bridge",
        desc = "An old arched bridge over a fast-flowing stream. "
             .. "The stones are mossy and slippery. "
             .. "A toll booth sits at the far end, unmanned.",
    },
    village_square = {
        name = "Village Square",
        desc = "A modest square with a dry fountain at its centre. "
             .. "The tavern door is open. "
             .. "Market stalls line the edges, half-empty at this hour.",
    },
    -- [STEP 2] Add more locations here
}

-- Travel map: which location_ids are reachable from each location.
-- Used by the LLM schema to enumerate valid movement targets.
local TRAVEL_MAP = {
    forest_path    = { "stone_bridge", "village_square" },
    stone_bridge   = { "forest_path" },
    village_square = { "forest_path" },
    -- [STEP 2] Keep in sync with LOCATIONS above
}

-- All valid location_id values (for JSON schema enum).
-- Build this automatically — no need to update manually.
local ALL_LOCATION_IDS = { '""' }  -- "" = stay in place
for id in pairs(LOCATIONS) do table.insert(ALL_LOCATION_IDS, '"' .. id .. '"') end
table.sort(ALL_LOCATION_IDS)

-- ---------------------------------------------------------------------------
-- Items
-- ---------------------------------------------------------------------------
local ITEMS = {
    torch    = { name="Torch",       desc="Burns brightly. Keeps the dark at bay." },
    coin     = { name="Silver Coin", desc="Standard currency." },
    key      = { name="Iron Key",    desc="Fits a lock you haven't found yet." },
    -- [STEP 2] Add more items here
}

-- All valid item_ids (for JSON schema enum).
local ALL_ITEM_IDS = {}
for id in pairs(ITEMS) do table.insert(ALL_ITEM_IDS, '"' .. id .. '"') end
table.sort(ALL_ITEM_IDS)

-- ---------------------------------------------------------------------------
-- [STEP 2 — OPTIONAL] Static NPC data
-- Used by both code-driven (npc.lua) and simple table-driven NPCs.
-- ---------------------------------------------------------------------------
local NPC_DATA = {
    mira = {
        name        = "Mira",
        description = "The innkeeper. Warm, observant, 40s. Always wiping her hands on her apron.",
        location    = "village_square",
    },
    guard = {
        name        = "Bridge Guard",
        description = "A bored soldier at the toll booth. Suspicious of strangers.",
        location    = "stone_bridge",
    },
    -- [STEP 2] Add more NPCs here
}

-- ---------------------------------------------------------------------------
-- [STEP 2 — OPTIONAL] Code-driven NPC configs for lib/npc.lua
-- Delete this entire block if not using npc.lua.
-- ---------------------------------------------------------------------------
--[[
local MIRA_CONFIG = {
    stats_defaults = { energy=0.8, mood=0.7, loneliness=0.3 },
    stat_bounds    = { energy={min=0,max=1}, mood={min=0,max=1}, loneliness={min=0,max=1} },
    idle_activity  = "wipes down the counter, humming a tune",
    routine = {
        { time={"06:00","21:00"}, location="village_square",
          activity="manages the tavern, serves customers",
          stats={ energy=-0.05, loneliness=-0.1 } },
        { time={"21:30","05:59"}, location="village_square",
          activity="sleeps in the back room",
          stats={ energy=0.3 } },
    },
    needs = {},
    sequences = {},
    event_reactions = {
        ["brawl_started"] = {
            activity="grabs an oak tankard and shouts 'Not in here!'",
            stats={ mood=-0.2 },
            narrative_hint = "Mira is furious — defuse it or she'll throw someone out",
        },
    },
}
]]--


-- =============================================================================
-- [STEP 3] MUTABLE STATE
-- Everything that changes during play lives here. Must be JSON-serialisable
-- (no functions, no userdata). Restored from snapshot on load.
-- =============================================================================

local state = {}

local function default_state()
    return {
        player = {
            name      = "Adventurer",
            location  = "forest_path",    -- starting location_id
            inventory = {},               -- list of item_ids
            gold      = 10,
            hp        = 10,
            max_hp    = 10,
            -- [STEP 3] Add player stats, skills, flags here
        },
        turn = 0,
        time = "08:00",
        day  = "monday",

        -- [STEP 3 — OPTIONAL] NPC runtime state (if using NPC system)
        npc_locations  = { mira="village_square", guard="stone_bridge" },
        npc_outfits    = { mira="APRON", guard="ARMOUR" },
        npc_stats      = {
            mira  = { energy=0.8, mood=0.7, loneliness=0.3 },
            guard = { energy=0.9, mood=0.5 },
        },
        npc_activities = {},
        npc_engaged    = { mira=false, guard=false },
        npc_memories   = { mira={}, guard={} },
        narrative_context = {},  -- hints injected into system prompt by NPC tick

        -- [STEP 3 — OPTIONAL] GM notes (in-session, from tools.remember)
        notes = {},

        -- [STEP 3] Add world flags, quest states, etc.
        -- quest_flags = { bridge_unlocked=false, found_key=false },
    }
end


-- =============================================================================
-- [STEP 4 — OPTIONAL] NPC SYSTEM (lib/npc.lua + lib/agent.lua)
-- Delete this entire section if you are not using autonomous NPCs.
-- =============================================================================

local npcs = {}          -- { id -> npc_lib NPC object }
local agents = {}        -- { id -> agent_lib agent object }
local turn_counter = nil -- shared turn counter for all agents

--[[
-- World adapter factory — each NPC gets its own; all read/write shared state.
local function make_world()
    return {
        getLocation    = function(name)
            if name == "player" then return state.player.location end
            return state.npc_locations[name]
        end,
        setLocation    = function(name, loc)  state.npc_locations[name] = loc  end,
        isInLocation   = function(name, loc)
            if name == "player" then return state.player.location == loc end
            return state.npc_locations[name] == loc
        end,
        countInLocation = function(loc)
            local n = (state.player.location == loc) and 1 or 0
            for _, l in pairs(state.npc_locations) do if l == loc then n=n+1 end end
            return n
        end,
        getAppearance  = function(name)    return state.npc_outfits[name] or "NORMAL" end,
        setAppearance  = function(name, o) state.npc_outfits[name] = o end,
        distance       = function(a, b) return (a == b) and 0 or 2 end,  -- replace with real map
    }
end

local function init_npcs()
    local world = make_world()
    -- Code-driven NPC (routine, needs, events)
    npcs["mira"] = NPC_lib.new("mira", MIRA_CONFIG, world)

    -- Restore NPC state from snapshot
    for name, npc in pairs(npcs) do
        if state.npc_stats[name]   then
            for s, v in pairs(state.npc_stats[name]) do npc.stats[s] = v end
        end
        if state.npc_engaged and state.npc_engaged[name] ~= nil then
            npc:setEngaged(state.npc_engaged[name])
        end
        if state.npc_memories and state.npc_memories[name] and #state.npc_memories[name] > 0 then
            npc.memory = state.npc_memories[name]
        else
            -- Seed background lore once at game start
            if name == "mira" then
                npc:pushMemory("grew up in the village, inherited the tavern from her father", 2)
            end
        end
    end

    -- Shared turn counter: max 2 total agent LLM calls per turn
    turn_counter = agent.new_turn_counter(2)

    -- LLM-driven agent for mira (composed with npc object above)
    agents["mira"] = agent.new("mira", {
        system   = "You are Mira, the village innkeeper. "
                .. "You are warm, sharp-eyed, and know everyone's secrets. "
                .. "Always stay in character. Reply concisely (2-3 sentences).",
        model    = nil,           -- nil = use engine default; or "llama3.2"
        provider = nil,           -- nil = use engine default; or "ollama"
        npc          = npcs["mira"],  -- links code-driven state for fallback
        turn_counter = turn_counter,  -- shared cap across all agents
        short_term_goals = { "serve the tavern customers" },
        long_term_goals  = { "keep the peace in the village" },
        memory_enabled   = true,      -- injects memory.lua entries into prompt
    })
end

-- Sync NPC stats/engagement back into state after each tick (for save/load)
local function sync_npc_state()
    for name, npc in pairs(npcs) do
        state.npc_stats[name] = {}
        for s, v in pairs(npc.stats) do state.npc_stats[name][s] = v end
        state.npc_engaged[name]  = npc:isEngaged()
        state.npc_memories[name] = npc.memory
    end
    for name, ag in pairs(agents) do
        -- agent state is lightweight; only snapshot if you call agent_snapshot()
        _ = ag  -- suppress unused warning
    end
end

-- Run one NPC tick (call from process_ai_response or before_ai_turn)
local function run_npc_tick(extra_events)
    if not NPC_lib then return end
    local protagonist_loc = state.player.location
    local results, _ = NPC_lib.tick(npcs, state.time, state.day, protagonist_loc)
    state.npc_activities    = {}
    state.narrative_context = {}
    for name, r in pairs(results) do
        state.npc_activities[name] = r.activity
        state.npc_locations[name]  = r.location
        if r.narrative_hint then
            table.insert(state.narrative_context, "[" .. name .. "] " .. r.narrative_hint)
        end
    end
    if extra_events and #extra_events > 0 then
        local reactions = NPC_lib.dispatch(extra_events, npcs, protagonist_loc)
        for name, r in pairs(reactions) do
            if r.narrative_hint then
                table.insert(state.narrative_context,
                    "[" .. name .. " reagisce] " .. r.narrative_hint)
            end
        end
    end
    sync_npc_state()
end
]]--


-- =============================================================================
-- [STEP 5] HELPERS
-- =============================================================================

local function has_item(id)
    for _, v in ipairs(state.player.inventory) do if v == id then return true end end
    return false
end

local function add_item(id)
    if not has_item(id) then table.insert(state.player.inventory, id) end
end

local function remove_item(id)
    for i, v in ipairs(state.player.inventory) do
        if v == id then table.remove(state.player.inventory, i); return true end
    end
    return false
end

local function inventory_string()
    if #state.player.inventory == 0 then return "nothing" end
    local names = {}
    for _, id in ipairs(state.player.inventory) do
        table.insert(names, (ITEMS[id] and ITEMS[id].name) or id)
    end
    return table.concat(names, ", ")
end

-- Returns list of NPC names present at the player's current location.
local function npcs_in_location(loc_id)
    local present = {}
    for id, loc in pairs(state.npc_locations or {}) do
        if loc == loc_id then
            local data = NPC_DATA[id]
            if data then
                table.insert(present, {
                    id          = id,
                    name        = data.name,
                    description = data.description,
                    activity    = (state.npc_activities or {})[id],
                    memories    = npcs[id] and npcs[id]:getMemoryContext(4) or nil,
                })
            end
        end
    end
    return present
end

-- [STEP 5 — OPTIONAL] Time system
local DAYS = { "monday","tuesday","wednesday","thursday","friday","saturday","sunday" }
local function advance_time(minutes)
    minutes = minutes or 30
    local h, m = state.time:match("(%d+):(%d+)")
    h = tonumber(h); m = tonumber(m) + minutes
    if m >= 60 then h = h + math.floor(m/60); m = m % 60 end
    if h >= 24 then
        h = h % 24
        for i, d in ipairs(DAYS) do
            if d == state.day then state.day = DAYS[(i%7)+1]; break end
        end
    end
    state.time = string.format("%02d:%02d", h, m)
end


-- =============================================================================
-- [STEP 6] REQUIRED FUNCTIONS
-- The engine calls these; all must be present.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- Intro text. Shown before the game starts. Ask for the player's name here.
-- ---------------------------------------------------------------------------
function get_welcome_message()
    return [[
══════════════════════════════════════════════════════
  MY ADVENTURE TITLE
  (Replace with your title)
══════════════════════════════════════════════════════

Opening paragraph — set the scene and tone.
One or two sentences is enough. Keep it atmospheric.

What is your name, traveler?
(Press Enter to play as "Adventurer")
]]
end

-- ---------------------------------------------------------------------------
-- Called after the player types their name (or any first-turn text).
-- ---------------------------------------------------------------------------
function set_initial_state(player_input)
    state = default_state()
    if player_input and player_input ~= "" then
        state.player.name = player_input
    end
    -- [STEP 6 — OPTIONAL] Init NPC objects after state is ready
    -- init_npcs()
    -- run_npc_tick()
end

-- Called if the player pressed Enter with empty input.
function generate_initial_state()
    set_initial_state("Adventurer")
end

-- ---------------------------------------------------------------------------
-- World state serialised to JSON — sent to the LLM before each turn.
-- Include only what the LLM needs to narrate accurately.
-- ---------------------------------------------------------------------------
function get_status_for_ai()
    local loc   = LOCATIONS[state.player.location] or {}
    local exits = TRAVEL_MAP[state.player.location] or {}
    local present = npcs_in_location(state.player.location)

    return json.encode({
        player = {
            name      = state.player.name,
            location  = loc.name or state.player.location,
            hp        = state.player.hp .. "/" .. state.player.max_hp,
            gold      = state.player.gold,
            inventory = inventory_string(),
        },
        location = {
            id          = state.player.location,
            name        = loc.name or state.player.location,
            description = loc.desc or "",
            exits       = exits,
        },
        npcs_present = present,
        time  = state.time,
        day   = state.day,
        turn  = state.turn,
        -- [STEP 6] Add any other context the LLM needs (quest flags, world state)
    })
end

-- ---------------------------------------------------------------------------
-- System prompt — rebuilt every turn. The most important function.
--
-- STRATEGY: choose one mode and stick to it.
--
--   MODE A — JSON schema only (no tools):
--     State changes live in the JSON response fields.
--     The LLM fills new_location, hp_change, picked_up, etc.
--     process_ai_response() reads those fields and updates state.
--     Best for: simple adventures, local models, no tool-calling support.
--
--   MODE B — Tool calling only:
--     State changes happen via tool calls (move_player, change_stat, …).
--     get_json_schema() returns a minimal schema with only "narration".
--     process_ai_response() only reads r.narration (or r.narrative).
--     Best for: complex state machines, NPCs, multi-step actions.
--     Requires: provider with tool calling (OpenAI, OpenRouter, Claude).
--
--   MODE C — Mix (some tools + some schema fields):
--     Complex actions (move NPC, give item) go through tools.
--     Simple atomic things (game_over, picked_up) stay in the schema.
--     Most flexible, slightly more complex to reason about.
--
-- [STEP 6] Pick a mode. Delete the WORKFLOW/TOOL sections for MODE A.
-- ---------------------------------------------------------------------------
function get_system_prompt()
    local loc    = LOCATIONS[state.player.location] or {}
    local loc_id = state.player.location

    -- ── PART 1: Character + setting (static, change once per adventure) ────
    -- [STEP 6] Replace with your world, genre, player role
    local header = string.format(
        "You are the narrator of \"My Adventure\" — a fantasy adventure.\n"
     .. "The player is %s.\n"
     .. "Write in second person, present tense. Be vivid but concise (2-4 sentences).",
        state.player.name
    )

    -- ── PART 2: Fundamental rules ──────────────────────────────────────────
    -- [STEP 6] Adapt rules to your game mechanics
    local rules = [[

## RULES
1. The protagonist acts ONLY on player input. Narrate consequences, do not invent player actions.
2. Do not invent locations, NPCs or items beyond what is defined in the world.
3. Respect world flags: locked doors, sealed passages, quest prerequisites.]]
    -- [STEP 6] Add game-specific rules here, e.g.:
    -- rules = rules .. "\n4. Sexual content: respect NPC relationship score (zone). Do not anticipate."

    -- ── PART 3: Current location ───────────────────────────────────────────
    local location_block = string.format(
        "\n\n## CURRENT LOCATION: %s\n%s",
        loc.name or loc_id,
        loc.desc or ""
    )
    -- [STEP 6] If your LOCATIONS table has extra fields (acoustic, npc_notes, etc.):
    -- if loc.notes then location_block = location_block .. "\nNote: " .. loc.notes end

    -- ── PART 4: NPC positions and activities ───────────────────────────────
    -- Shows the LLM where every NPC is and what they are doing.
    -- [STEP 6] Delete this block if you have no NPCs or manage them via tools.
    local npc_positions = {}
    for id, l in pairs(state.npc_locations or {}) do
        local data = NPC_DATA[id]
        if data then
            local act = (state.npc_activities or {})[id]
            table.insert(npc_positions, string.format(
                "  %s → %s%s",
                data.name,
                l,
                act and (" | " .. act) or ""
            ))
        end
    end
    table.sort(npc_positions)
    local npc_pos_block = #npc_positions > 0
        and ("\n\n## NPC POSITIONS\n" .. table.concat(npc_positions, "\n"))
        or ""

    -- ── PART 5: Personalities of NPCs present in this location ────────────
    -- Only inject personality for NPCs the player can currently interact with.
    -- [STEP 6] Delete if no NPCs, or expand with full personality strings.
    local personalities = {}
    for id, l in pairs(state.npc_locations or {}) do
        if l == loc_id then
            local data = NPC_DATA[id]
            if data then
                table.insert(personalities, "=== " .. data.name .. " ===\n" .. data.description)
            end
        end
    end
    local personality_block = #personalities > 0
        and ("\n\n## NPC PERSONALITIES (present here)\n" .. table.concat(personalities, "\n\n"))
        or ""

    -- ── PART 6: Injected context (dynamic each turn) ───────────────────────

    -- GM notes (stored by tools.remember / tools.forget)
    local notes_block = ""
    if state.notes and #state.notes > 0 then
        notes_block = "\n\n## GM NOTES\n"
        for _, note in ipairs(state.notes) do
            notes_block = notes_block .. "- " .. note .. "\n"
        end
    end

    -- NPC narrative hints from NPC tick (npc.lua)
    local hints_block = ""
    if state.narrative_context and #state.narrative_context > 0 then
        hints_block = "\n\n## NPC CONTEXT (weave into narration)\n"
        for _, hint in ipairs(state.narrative_context) do
            hints_block = hints_block .. hint .. "\n"
        end
    end

    -- Pending world events (e.g. an NPC enters the room between turns)
    -- [STEP 6] Set state.pending_event in process_ai_response or after_ai_turn,
    -- inject it here, then clear it. The LLM will open the scene accordingly.
    local event_block = ""
    if state.pending_event then
        event_block = "\n\n[AUTOMATIC EVENT] " .. state.pending_event
        state.pending_event = nil
    end

    -- ── PART 7: WORKFLOW + TOOL RULES (MODE B / C only) ───────────────────
    -- [STEP 6 — MODE A] Delete this block entirely if NOT using tool calling.
    -- [STEP 6 — MODE B/C] Uncomment and list EVERY tool with its calling rule.
    local tool_rules = ""
    --[[
    tool_rules = [[

════════════════════════════════════════════════════
WORKFLOW — MANDATORY ORDER (call tools BEFORE narrating)
════════════════════════════════════════════════════
 1. generate_npc        — new NPC encountered face-to-face. MAX 2/turn.
 2. think_as_npc       — NPC reaction/dialogue. Cached per turn. "speech" → VERBATIM in narration.
 3. advance_time       — BEFORE any action that takes time. MAX 1/turn.
 4. move_player        — explicit player movement to new location.
 5. generate_location  — location never visited before. BEFORE entering it.
 6. generate_object    — new interactable object. BEFORE describing/using it.
 7. object_action      — action on existing object (open, read, use, examine).
 8. object_write       — write content into object (bulletin board, mailbox, register).
 9. move_npc           — NPC movement not covered by routine. BEFORE narrating it.
10. npc_life_event     — permanent change to an NPC (agreement, trauma, change). Persists to disk.
11. remember           — SUBJECTIVE player note (scope: player=only you, public=everyone knows, npc=shared with one NPC).
12. memory_write       — OBJECTIVE fact about an NPC, cross-session. Only from think_as_npc or direct input. MAX 3/turn.
13. memory_read        — read a persistent fact about an NPC.
THEN: write narration. STOP.

════════════════════════════════════════
TOOL CALLING RULES
════════════════════════════════════════
advance_time     — BEFORE narrating any action that takes time. MAX 1 per turn.
move_player      — ONLY if player explicitly moves. NOT if already in the room.
generate_location — BEFORE entering a place that does not exist yet. Idempotent.
generate_object  — BEFORE describing or interacting with an object seen for the first time.
object_action    — Use on objects that already exist. Do NOT call generate_object first if the object is known.
move_npc         — BEFORE narrating an NPC in a new location. MAX 1 per NPC per turn.
think_as_npc     — ONCE per NPC per turn. Cached: repeating returns same result.
                   "speech" field = exact words → quote VERBATIM in narration. Do not paraphrase.
remember         — player's subjective notes. Use scope=public for facts everyone in the world knows.
memory_write     — persistent entity facts only (from think_as_npc or direct player input). Not for deductions.
]]
    -- [STEP 6] Add your custom tools here following the same pattern:
    -- my_tool — when to call it, how many times max per turn, what it must NOT be used for.
    ]]--

    return header .. rules .. location_block
        .. npc_pos_block .. personality_block
        .. notes_block .. hints_block .. event_block
        .. tool_rules
end

-- ---------------------------------------------------------------------------
-- JSON Schema the LLM response must conform to.
--
-- MODE A (no tools): full schema — all state changes live here.
--   Remove fields you don't use. Fewer fields = better LLM compliance.
--
-- MODE B (tool calling only): minimal schema — only "narration" required.
--   The LLM calls tools for state changes; the schema is just the final text.
--   process_ai_response() reads only r.narration (or r.narrative).
--
-- MODE C (mix): combine — keep schema fields for simple atomic outcomes
--   (game_over, picked_up), use tools for complex state changes (move NPC).
--
-- [STEP 6] Pick the schema that matches your chosen mode. Delete the others.
-- ---------------------------------------------------------------------------
function get_json_schema()

    -- ══════════════════════════════════════════════════════════════════════
    -- MODE A — JSON schema only (active by default, no tools needed)
    -- ══════════════════════════════════════════════════════════════════════
    local exits     = TRAVEL_MAP[state.player.location] or {}
    local loc_enum  = { '""' }
    for _, id in ipairs(exits) do table.insert(loc_enum, '"' .. id .. '"') end
    local item_enum = table.concat(ALL_ITEM_IDS, ", ")

    return string.format([[{
  "type": "object",
  "required": ["narration", "game_over"],
  "properties": {
    "narration":      { "type": "string",
                        "description": "2-4 sentences, second person present tense" },
    "new_location":   { "type": "string", "enum": [%s],
                        "description": "location_id if player moved, empty string to stay" },
    "time_passes":    { "type": "integer", "minimum": 0, "maximum": 120,
                        "description": "minutes elapsed this turn (0 = instant action)" },
    "picked_up":      { "type": "array", "items": { "type": "string", "enum": [%s] },
                        "description": "item_ids found this turn, empty array if none" },
    "dropped":        { "type": "array", "items": { "type": "string", "enum": [%s] },
                        "description": "item_ids discarded this turn, empty array if none" },
    "hp_change":      { "type": "integer", "minimum": -10, "maximum": 5,
                        "description": "HP delta: negative = damage, positive = healing, 0 = unchanged" },
    "gold_change":    { "type": "integer",
                        "description": "Gold delta: negative = spent, positive = found" },
    "game_over":      { "type": "boolean" },
    "game_over_reason": { "type": "string",
                        "description": "Win/loss description. Empty string while game continues." }
  }
}]], table.concat(loc_enum, ", "), item_enum, item_enum)

    -- [STEP 6 — MODE A] Extend schema with NPC fields:
    --   "engage_npc"    : {"type":"object"}  — {NpcName: true/false}
    --   "push_npc_memory": {"type":"object"} — {NpcName: "short memory text"}
    --   "event_name"    : {"type":"string"}  — world event to dispatch to NPC tick
    --   "event_info"    : {"type":"object"}  — {type:"broadcast"|"direct"|"location"|"area", ...}
    --   "quest_update"  : {"type":"object"}  — any quest flag changes you define
    --
    -- [STEP 6 — MODE B] Replace the return above with this minimal schema:
    --[==[
    return [[{
      "type": "object",
      "required": ["narration"],
      "properties": {
        "narration": { "type": "string", "description": "Narration of the turn." }
      }
    }]]
    ]==]--
    --
    -- [STEP 6 — MODE C] Keep game_over + a few atomic fields; tools handle the rest:
    --[==[
    return [[{
      "type": "object",
      "required": ["narration", "game_over"],
      "properties": {
        "narration":         { "type": "string" },
        "game_over":         { "type": "boolean" },
        "game_over_reason":  { "type": "string" }
      }
    }]]
    ]==]--
end

-- ---------------------------------------------------------------------------
-- Process the LLM's JSON response. Validate, update state, return narration.
-- If validation fails, return { success=false, error="..." } and the engine retries.
-- ---------------------------------------------------------------------------
function process_ai_response(reply)
    -- [STEP 6 — OPTIONAL] Use json_repair if getting malformed responses:
    -- local ok, r = pcall(json.decode, json_repair.repair(reply))
    local ok, r = pcall(json.decode, reply)
    if not ok or type(r) ~= "table" then
        return { success=false, error="Invalid JSON: " .. tostring(r) }
    end
    if not r.narration or r.narration == "" then
        return { success=false, error="Missing narration" }
    end

    -- Move player
    if r.new_location and r.new_location ~= "" then
        local valid = LOCATIONS[r.new_location]
        if valid then
            -- [STEP 6] Add movement restriction checks here:
            -- if r.new_location == "locked_room" and not state.quest_flags.has_key then ...
            state.player.location = r.new_location
        end
    end

    -- Advance time
    local mins = tonumber(r.time_passes) or 30
    if mins > 0 then advance_time(mins) end

    -- Pick up items
    if type(r.picked_up) == "table" then
        for _, id in ipairs(r.picked_up) do
            if ITEMS[id] then add_item(id) end
        end
    end

    -- Drop items
    if type(r.dropped) == "table" then
        for _, id in ipairs(r.dropped) do remove_item(id) end
    end

    -- HP change
    if type(r.hp_change) == "number" and r.hp_change ~= 0 then
        state.player.hp = math.max(0, math.min(state.player.max_hp,
                                               state.player.hp + r.hp_change))
        if state.player.hp <= 0 then
            return {
                success          = true,
                narration        = r.narration .. "\n\nYou have fallen. The world grows dark.",
                game_over        = true,
                game_over_reason = "DEFEAT — You died.",
            }
        end
    end

    -- Gold change
    if type(r.gold_change) == "number" and r.gold_change ~= 0 then
        state.player.gold = math.max(0, state.player.gold + r.gold_change)
    end

    -- [STEP 6 — OPTIONAL] Process NPC events (if using NPC system)
    --[[
    local extra_events = {}
    if r.event_name and r.event_name ~= "" then
        table.insert(extra_events, {
            name = r.event_name, info = r.event_info or { type="broadcast" }, source="player"
        })
    end
    if type(r.engage_npc) == "table" then
        for name, flag in pairs(r.engage_npc) do
            if npcs[name] then npcs[name]:setEngaged(flag == true) end
        end
    end
    if type(r.push_npc_memory) == "table" then
        for name, text in pairs(r.push_npc_memory) do
            if npcs[name] and type(text) == "string" and text ~= "" then
                npcs[name]:pushMemory(text)
            end
        end
    end
    run_npc_tick(extra_events)
    ]]--

    -- [STEP 6] Add win conditions here:
    -- if has_item("amulet") and state.player.location == "altar" then ... end

    state.turn = state.turn + 1

    if r.game_over == true then
        return {
            success          = true,
            narration        = r.narration,
            game_over        = true,
            game_over_reason = r.game_over_reason or "Your adventure ends here.",
        }
    end

    return {
        success          = true,
        narration        = r.narration,
        game_over        = false,
        game_over_reason = "",
    }
end

-- ---------------------------------------------------------------------------
-- Handle /commands before the LLM sees the input.
-- Return handled=true to skip the LLM for this turn.
-- ---------------------------------------------------------------------------
function process_player_input(input)
    local cmd = input:lower():match("^(/[%w_]+)")

    if cmd == "/inv" or cmd == "/inventory" or cmd == "/i" then
        return { success=true, handled=true,
                 output = "Inventory: " .. inventory_string()
                       .. "  |  Gold: " .. state.player.gold }
    end

    if cmd == "/map" or cmd == "/exits" then
        local exits = TRAVEL_MAP[state.player.location] or {}
        local lines = { "Exits:" }
        for _, id in ipairs(exits) do
            local dest = LOCATIONS[id]
            table.insert(lines, "  • " .. (dest and dest.name or id))
        end
        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end

    if cmd == "/time" then
        return { success=true, handled=true,
                 output = state.time .. ", " .. state.day .. "  (turn " .. state.turn .. ")" }
    end

    if cmd == "/hp" then
        return { success=true, handled=true,
                 output = "HP: " .. state.player.hp .. "/" .. state.player.max_hp }
    end

    -- [STEP 6 — OPTIONAL] NPC status command
    --[[
    if cmd == "/npcs" then
        local lines = { "NPC STATUS:" }
        for id, data in pairs(NPC_DATA) do
            local loc  = state.npc_locations[id] or "?"
            local act  = (state.npc_activities or {})[id] or "—"
            table.insert(lines, string.format("  %-10s  %-20s  %s", data.name, loc, act))
        end
        return { success=true, handled=true, output=table.concat(lines, "\n") }
    end
    ]]--

    return { success=true, handled=false }
end

-- ---------------------------------------------------------------------------
-- HUD text shown above the chat every turn.
-- ---------------------------------------------------------------------------
function get_display_state()
    local loc  = LOCATIONS[state.player.location]
    local name = loc and loc.name or state.player.location
    local npcs_here = {}
    for id, l in pairs(state.npc_locations or {}) do
        if l == state.player.location then
            table.insert(npcs_here, NPC_DATA[id] and NPC_DATA[id].name or id)
        end
    end
    table.sort(npcs_here)
    return string.format("[ %s  |  %s %s  |  HP:%d/%d  G:%d  Inv:%s%s ]",
        state.player.name,
        state.time, state.day,
        state.player.hp, state.player.max_hp,
        state.player.gold,
        inventory_string(),
        #npcs_here > 0 and ("  |  " .. table.concat(npcs_here, ", ")) or "")
end

-- ---------------------------------------------------------------------------
-- Save / Load
-- ---------------------------------------------------------------------------
function get_state_snapshot()
    return json.encode(state)
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then
        return { success=false, error="Failed to parse snapshot: " .. tostring(data) }
    end
    state = data
    -- [STEP 6 — OPTIONAL] Re-init NPC objects after load
    -- init_npcs()
    return { success=true }
end

-- ---------------------------------------------------------------------------
-- Sidebar command list shown in the web UI.
-- ---------------------------------------------------------------------------
function get_commands()
    return {
        { cmd="/inv",  desc="Show inventory and gold" },
        { cmd="/map",  desc="Show exits from current location" },
        { cmd="/time", desc="Show current time and day" },
        { cmd="/hp",   desc="Show HP" },
        -- [STEP 6] Add your custom commands here
    }
end


-- =============================================================================
-- [STEP 7 — OPTIONAL] TOOLS
-- Enable tool calling so the LLM can trigger actions from within a turn.
-- Delete this function if not using tool calling.
-- Requires a provider that supports tool calling (OpenAI, OpenRouter, Claude).
-- =============================================================================
--[==[
function get_tools()
    -- Uncomment only the tools you actually need.
    return tools_lib.build({

        -- Dice rolls — essential for any game with skill checks or combat
        tools_lib.roll_dice(state),

        -- Skill checks — requires state.player.skills = { skill_name = bonus }
        -- tools_lib.skill_check(state),

        -- Inventory check — useful for trade and crafting
        -- tools_lib.inventory_check(state),

        -- GM notes — short narrative facts the LLM can store and retrieve.
        -- WARNING: in complex MODE B scripts these tools cause call explosions:
        -- the LLM accumulates 10-15 notes per session and cycles remember/forget
        -- every turn. If you see [TOOL LOOP] in logs, remove these two lines first.
        -- Use memory_write below for anything that needs to persist cross-session.
        -- tools_lib.remember(state),
        -- tools_lib.forget(state),

        -- Persistent memory (cross-session) — read/write via main LLM only
        {
            name = "memory_write",
            description = "Save a persistent fact about an entity (NPC, location, player).",
            params = [[{ "type":"object", "required":["entity","category","content"],
                         "properties": {
                             "entity":   { "type":"string" },
                             "category": { "type":"string" },
                             "content":  { "type":"string" }
                         }}]],
            fn = function(args_json)
                local a = json.decode(args_json)
                memory.write(a.entity, a.category, a.content)
                return json.encode({ ok=true })
            end,
        },
        {
            name = "memory_read",
            description = "Read a persistent fact about an entity.",
            params = [[{ "type":"object", "required":["entity","category"],
                         "properties": {
                             "entity":   { "type":"string" },
                             "category": { "type":"string" }
                         }}]],
            fn = function(args_json)
                local a = json.decode(args_json)
                local val = memory.read(a.entity, a.category)
                return json.encode({ value = val or "(no memory)" })
            end,
        },

        -- ── Custom tools — [TOOL CALL GUARD] pattern ──────────────────────────
        -- To prevent the LLM from calling the same tool twice per turn:
        --   1. Declare: local _tool_calls = {} at the top of the file.
        --   2. Reset:   _tool_calls = {} in before_ai_turn (see [STEP 8]).
        --   3. In each tool fn, add these two lines before doing anything:
        --        if _tool_calls["my_tool"] then
        --            return json.encode({ error="my_tool already called this turn" })
        --        end
        --        _tool_calls["my_tool"] = true
        --   The LLM sees the error in the tool result and stops retrying.
        --
        -- NPC lock (if using npc.lua sequences):
        --   Before moving an NPC via a move_npc tool, check:
        --     if npcs[npc_name] and npcs[npc_name].current_sequence ~= nil then
        --         return json.encode({ error="NPC is managed by NPC system this turn" })
        --     end
        --
        -- [STEP 7] Add your custom tools here:
        -- {
        --     name = "my_tool",
        --     description = "What this tool does. MAX 1 call per turn.",
        --     params = '{ "type":"object", "required":["arg1"], "properties": { "arg1": { "type":"string" } } }',
        --     fn = function(args_json)
        --         if _tool_calls["my_tool"] then
        --             return json.encode({ error="my_tool already called this turn" })
        --         end
        --         _tool_calls["my_tool"] = true
        --         local a = json.decode(args_json)
        --         -- do something with a.arg1
        --         return json.encode({ result = "ok" })
        --     end,
        -- },

        -- ── LLM-driven NPC agent tool (if using agent.lua) ───────────────────
        -- The description of this tool is what the main LLM reads before calling it.
        -- Include the verbatim-dialogue instruction directly in the description so
        -- the LLM knows to carry the agent's words into the narration.
        -- agents["mira"]:as_tool("think_as_mira",
        --     "Ask Mira how she reacts to the current situation. "
        --     .. "IMPORTANT: any dialogue Mira speaks in the response must appear "
        --     .. "VERBATIM in the narration — do not paraphrase or omit her words."),

        -- [STEP 7] Add your custom tools here:
        -- {
        --     name = "my_tool",
        --     description = "What this tool does.",
        --     params = '{ "type":"object", "required":["arg1"], "properties": { "arg1": { "type":"string" } } }',
        --     fn = function(args_json)
        --         local a = json.decode(args_json)
        --         -- do something with a.arg1
        --         return json.encode({ result = "ok" })
        --     end,
        -- },
    })
end
]==]--


-- =============================================================================
-- [STEP 8 — OPTIONAL] HOOKS
-- before_ai_turn: called before the LLM. Can short-circuit with skip_llm=true.
-- after_ai_turn:  called after a successful LLM response.
-- Delete either function if not needed.
-- =============================================================================

--[[
function before_ai_turn(player_input)
    -- [TOOL CALL GUARD] Reset per-turn call tracker (if using anti-loop pattern)
    -- _tool_calls = {}

    -- Reset agent per-turn caches and shared turn counter
    agent.reset_all_turns(agents, turn_counter)

    -- Run code-driven NPC tick BEFORE the LLM sees the input
    -- run_npc_tick()

    -- Short-circuit example: skip LLM for trivial commands
    -- local trimmed = player_input:lower():match("^%s*(.-)%s*$")
    -- if trimmed == "wait" or trimmed == "aspetta" then
    --     advance_time(30)
    --     return { skip_llm=true, narration="Time passes quietly." }
    -- end

    return nil  -- let the turn proceed normally
end

function after_ai_turn(narration, raw_reply)
    -- Called after the LLM responded and state is already updated.
    -- Good for: NPC event forwarding, logging, side-effects.

    -- Example: tell agents about player speech
    -- if agents["mira"] then
    --     agents["mira"]:on_event("player_spoke", "player said something")
    -- end

    _ = narration  -- suppress unused warning
    _ = raw_reply
end
]]--


-- =============================================================================
-- [STEP 9 — OPTIONAL] IMAGE SYSTEM
-- Generates AI illustrations for scenes using t2i + i2i.
-- Delete all three functions if not using images.
-- =============================================================================

--[[
local IMAGE_DIR   = "images/my_adventure/"
local IMAGE_STYLE = "fantasy illustration, painterly style, detailed, atmospheric"
-- [STEP 9] Replace style with your genre: "anime", "photorealistic", "oil painting", etc.

local ASSET_PATHS = {
    -- Backgrounds (one per location_id)
    forest_path    = IMAGE_DIR .. "bg_forest_path.jpg",
    stone_bridge   = IMAGE_DIR .. "bg_stone_bridge.jpg",
    village_square = IMAGE_DIR .. "bg_village_square.jpg",
    -- NPC portraits (one per NPC id)
    mira  = IMAGE_DIR .. "npc_mira.jpg",
    guard = IMAGE_DIR .. "npc_guard.jpg",
    -- [STEP 9] Add paths for every location and NPC you want illustrated
}

local ASSET_PROMPTS = {
    -- Short English description of each asset, used to generate missing images.
    forest_path    = "dirt path through ancient oaks, dappled sunlight, stone bridge visible ahead",
    stone_bridge   = "mossy arched stone bridge over fast stream, toll booth, misty forest",
    village_square = "medieval village square, dry fountain, tavern, market stalls, morning light",
    mira           = "woman innkeeper 40s, warm smile, apron, half-body portrait, dark background",
    guard          = "bored soldier in light armour, spear, bridge background, half-body portrait",
    -- [STEP 9] Write one line per asset
}

function get_scene_images()
    local loc_id = state.player.location
    local assets = {}

    -- Background
    if ASSET_PATHS[loc_id] then
        table.insert(assets, { id=loc_id, path=ASSET_PATHS[loc_id] })
    end

    -- NPCs present
    for id, l in pairs(state.npc_locations or {}) do
        if l == loc_id and ASSET_PATHS[id] then
            table.insert(assets, { id=id, path=ASSET_PATHS[id] })
        end
    end

    -- base_image hint:
    --   "last" = same scene composition as last render → refine (faster, cheaper)
    --   nil    = composition changed → rebuild from collage (new location or NPCs)
    local hint = (state._last_image_loc == loc_id) and "last" or nil
    state._last_image_loc = loc_id

    return { assets=assets, base_image=hint }
end

function get_asset_path(id)
    return ASSET_PATHS[id]
end

function get_asset_prompt(id)
    local path = ASSET_PATHS[id]
    if not path then return nil end
    local desc = ASSET_PROMPTS[id] or id
    -- Let the LLM craft a proper t2i prompt from the raw description
    local sys = "You are a professional image generation prompt engineer. "
             .. "Convert the description into a detailed txt2img prompt. "
             .. "Max 80 words. English only. Output the prompt text only."
    local ok, prompt = pcall(query_llm, sys, "[]",
        "Description: " .. desc .. "\nStyle: " .. IMAGE_STYLE, "")
    return { path=path, prompt=ok and prompt or (desc .. ", " .. IMAGE_STYLE) }
end

function get_image_style()
    return IMAGE_STYLE
end
]]--

-- =============================================================================
-- [STEP 10 — OPTIONAL] VISUAL WORLD (rpgai-gui tile/sprite rendering)
-- Only called by the native GUI client (rpgai-gui). Ignored by console/web modes.
--
-- Requirements:
--   1. Uncomment: local visual = require("lib/visual") at the top.
--   2. Set BASE to your asset directory (visual.agents_base() auto-detects Agents).
--   3. Define rooms matching your game's locations.
--   4. Add NPCs with state_key = dot-path into get_state_snapshot() JSON.
--
-- The state_key is how rpgai-gui knows where to move a sprite.
-- It reads get_state_snapshot() after each turn and follows the path to a room name.
-- Example: state_key = "player.location" → reads state.player.location from snapshot.
--          state_key = "npcs.jenny.location" → reads state.npcs.jenny.location
-- The value must match one of the room names defined in the rooms list below.
-- =============================================================================

--[[
local BASE = visual.agents_base()  -- "/Users/.../Agents/"

function get_visual_world()
    return visual.encode({
        -- Tileset (Room_Builder PNG from Agents)
        tileset   = visual.room_builder(BASE),

        -- CSV floor map (exported from Tiled or Agents)
        floor_csv = BASE .. "asset/Laboratorio_floor.csv",

        -- Tile IDs that block pathfinding (walls, furniture edges)
        solid     = visual.LABORATORIO_SOLID,

        -- Rooms: name must match location values used in your state snapshot.
        -- bounds = {bx, by, bw, bh}  tile-space origin + size
        -- spawn  = {sx, sy}          walkable tile inside the room
        -- [STEP 10] Replace with your map's rooms. Use Tiled to get tile coords.
        rooms = {
            { name="forest_path",    bounds={1,  2,  13, 9}, spawn={7,  6}  },
            { name="stone_bridge",   bounds={15, 2,  13, 9}, spawn={21, 6}  },
            { name="village_square", bounds={1,  13, 7,  5}, spawn={4,  15} },
        },

        -- NPCs: one entry per character you want to animate on screen.
        -- state_key must point to a room-name string in your state snapshot JSON.
        npcs = {
            -- Player character (if you want to see yourself walking around)
            -- visual.npc("player",
            --     BASE .. "asset/characters/Body_48x48_02.png",
            --     "player.location"),

            -- [STEP 10] Add NPCs here. state_key = dot-path in snapshot JSON.
            -- visual.npc("mira",
            --     BASE .. "asset/characters/Body_48x48_01.png",
            --     "npc_locations.mira"),
        },

        walk_speed = 150,  -- pixels per second
    })
end
]]--

-- =============================================================================
-- [STEP 11 — OPTIONAL] PROCEDURAL WORLD (lib/world.lua)
-- Generate NPC, locations, and objects on-demand via LLM.
-- The world grows as the player explores — unknown entities are generated once
-- and then persist through saves.
-- Delete this section if your world is fully hand-crafted.
-- =============================================================================

--[[

-- 1. Require at the top of the file (replace the commented line above):
--    local world = require("lib/world")

-- 2. Call once during set_initial_state() or generate_initial_state():
--    world.init(
--        "A brief description of your world: setting, period, tone (1-3 sentences).",
--        -- Optional: override model/provider for generation LLM calls only:
--        -- { model="gpt-4o", provider="openai" }
--    )

-- 3. Pre-register static entities that should NOT be generated (they already exist):
--    world.set_npc("innkeeper", {
--        name="Mira", age=42, job="innkeeper",
--        personality="warm, observant, knows everyone's secrets",
--        relationships={ husband="Tomas (deceased)", regular="guard_bridge" },
--        routine={
--            { time="06:00", location="village_square", action="opens the tavern" },
--            { time="22:00", location="village_square", action="closes up and sleeps" },
--        },
--    })
--    world.set_location("village_square", {
--        id="village_square", name="Village Square",
--        description="A modest square with a dry fountain at its centre.",
--        objects={ "fountain", "market_stall" },
--        connected_to={ "forest_path", "tavern_interior" },
--        owner="",
--    })

-- 4. In get_tools() — expose three generation tools to the main LLM:
--    function get_tools()
--        return tools_lib.build({
--            world.as_tool_generate_npc(
--                "Generate a new NPC the player is meeting for the first time. "
--                .. "Call ONCE before narrating their appearance. "
--                .. "Do NOT call for NPCs already defined (innkeeper, guard, etc.)."),
--            world.as_tool_generate_location(
--                "Generate a new room or place the player wants to enter for the first time. "
--                .. "Call ONCE before narrating what the player sees inside."),
--            world.as_tool_generate_object(
--                "Generate a new interactable object the player wants to examine or use. "
--                .. "Call ONCE before narrating the interaction."),
--            world.as_tool_object_action(
--                "Apply an action (open, close, turn on, etc.) to an already-generated object. "
--                .. "Returns new state or error if action not valid from current state."),
--            world.as_tool_object_write(
--                "Write structured content into an existing object: append a message to a bulletin board, "
--                .. "add mail to a mailbox, update a register, remove an entry by index."),
--            -- ... other tools
--        })
--    end

-- 5. In get_system_prompt() — inject generated NPC personalities present at player's location:
--    local npc_block = ""
--    for id, npc_loc in pairs(state.npc_locations or {}) do
--        if npc_loc == state.player.location then
--            local gen = world.get_npc(id)
--            if gen then
--                npc_block = npc_block .. "\n\n=== " .. gen.name .. " ===\n" .. world.format_npc(id)
--            end
--        end
--    end

-- 6. In get_json_schema() — include generated locations in the movement enum:
--    -- Replace the static location enum with a dynamic one:
--    local known_locs = '"", ' .. world.all_location_ids_json()
--    -- Then use: table.concat(loc_enum, ", ") -> known_locs

-- 7. In get_state_snapshot() / restore_state() — persist the generated world:
--    function get_state_snapshot()
--        local snap = json.decode(json.encode(state))
--        snap._world = world.snapshot()
--        return json.encode(snap)
--    end
--    function restore_state(snapshot)
--        local ok, data = pcall(json.decode, snapshot)
--        if not ok then return { success=false, error=tostring(data) } end
--        world.restore(data._world)
--        data._world = nil
--        state = data
--        return { success=true }
--    end

-- DESIGN NOTES:
-- The main LLM decides WHEN to generate. It calls generate_npc when it determines
-- a character the player mentions doesn't exist yet. This is intentional: the LLM
-- has the narrative context to judge when generation is appropriate.
--
-- Generation is idempotent: calling ensure_npc("marco", ...) twice returns the
-- cached entity without a second LLM call. The tool also returns already_exists=true
-- so the main LLM knows it's recalling existing data, not creating new.
--
-- world.lua uses the engine default model/provider unless overridden in world.init().
-- For coherence, generation calls benefit from a capable model (Claude/GPT-4o),
-- even if the main game uses a cheaper model for narration.
--
-- LOCATION_ALIASES — prevent ID duplication when the LLM names a place differently
-- from an NPC's routine location_id. Define canonical IDs; generate_location redirects.
-- Example: if the player says "I go to the post office" the LLM might call
-- generate_location("post_office") when the NPC's routine already uses "ufficio_postale".
-- Fix: wrap world.as_tool_generate_location with an alias redirect:
--
--    local LOCATION_ALIASES = {
--        ["post office"] = "ufficio_postale",
--        ["post_office"] = "ufficio_postale",
--        ["outside"]     = "esterno",
--        ["street"]      = "esterno",
--    }
--    -- In get_tools():
--    local base_gen_loc = world.as_tool_generate_location("Generate a new location...")
--    local orig_fn = base_gen_loc.fn
--    base_gen_loc.fn = function(args_json)
--        local a = json.decode(args_json)
--        local canonical = LOCATION_ALIASES[(a.id or ""):lower()]
--        if canonical then
--            a.id = canonical; args_json = json.encode(a)
--            local r = json.decode(orig_fn(args_json)) or {}
--            r._redirected_to = canonical
--            return json.encode(r)
--        end
--        return orig_fn(args_json)
--    end

]]--


-- =============================================================================
-- [STEP 11b — OPTIONAL] PLOT SEEDS (trame/)
-- Pre-written story threads loaded at startup. One is chosen randomly each run.
-- The LLM knows the secret and the roles — the player discovers them by playing.
-- Roles are open slots (label + hint); the LLM fills them as NPCs are generated.
--
-- Pattern:
--   1. Create  scripts/trame/my_adventure_trame.lua  returning a list of trame tables.
--      Each trama: { id, titolo, segreto_centrale, ruoli=[{label,hint}], innesco, indizi=[] }
--   2. In module-level locals:
--        local ALL_TRAME = require("trame/my_adventure_trame")
--   3. Helper:
--        local function pick_trame(n)
--            n = n or 1; math.randomseed(os.time())
--            local pool = {}; for _, t in ipairs(ALL_TRAME) do table.insert(pool, t) end
--            for i = #pool, 2, -1 do local j=math.random(i); pool[i],pool[j]=pool[j],pool[i] end
--            local r = {}
--            for i = 1, math.min(n, #pool) do
--                local src = pool[i]
--                table.insert(r, { id=src.id, titolo=src.titolo, segreto_centrale=src.segreto_centrale,
--                    innesco=src.innesco, indizi=src.indizi, ruoli=src.ruoli, assegnazioni={} })
--            end
--            return r
--        end
--   4. In default_state():   trame_attive = {}
--   5. In set_initial_state(): state.trame_attive = pick_trame(1)
--   6. In restore_state():
--        state.trame_attive = state.trame_attive or {}
--        if #state.trame_attive == 0 then state.trame_attive = pick_trame(1) end
--   7. In get_system_prompt():
--        -- inject trame block so LLM knows the secret, roles and clues
--   8. Add assegna_ruolo_trama tool so LLM can bind generated NPCs to roles.
--
-- See scripts/trame/my_adventure_trame.lua for a full example.
-- =============================================================================


-- =============================================================================
-- [STEP 12 — OPTIONAL] PROCEDURAL NPCs (lib/persona.lua)
-- File-backed NPCs: each NPC is a .lua file on disk.
-- The file is the authoritative source — not the save JSON.
-- NPCs grow over time: dream system adds sequences, needs, npc_summary.
-- Use alongside world.lua for full procedural worlds.
-- Delete this section if using static hand-crafted NPCs only.
-- =============================================================================

--[[

-- 1. Require at the top:
--    local persona = require("lib/persona")
--    local agent   = require("lib/agent")    -- for LLM-driven reactions
--    local NPC     = require("npc")          -- for code-driven routines

-- 2. Init once:
--    persona.init(
--        "./scripts/npcs/",   -- directory where NPC .lua files are stored
--        "World context: setting, period, tone (2-4 sentences). "
--        .. "Used as context for every NPC generation LLM call.",
--        { max_sequences=4, max_needs=3, max_event_reactions=5, max_stats=3 }
--    )

-- 3. Storage for live NPC objects (module-level locals):
--    local generated_agents      = {}   -- { id → agent object }
--    local generated_npc_objects = {}   -- { id → npc.lua NPC object }
--    local gen_tc = agent.new_turn_counter(2)  -- max 2 LLM agent calls/turn

-- 4. WORLD ADAPTER — one per NPC, reads/writes state.generated_npc_locations.
--    IMPORTANT: npc.lua uses data.name (e.g. "Rosangela Lo Monte") as self.name,
--    but state keys use the NPC id (e.g. "rosangela_201"). The adapter must accept
--    BOTH via is_self(). Otherwise setLocation calls are silently ignored.
--
--    local function make_gen_world_adapter(npc_id)
--        local function is_self(name)
--            if name == npc_id then return true end
--            local p = persona.get(npc_id)
--            return p and name == p.name
--        end
--        return {
--            getLocation = function(name)
--                if is_self(name) then
--                    return (state.generated_npc_locations or {})[npc_id] or "entrance"
--                end
--                if name == "player" then return state.player.location end
--                return (state.generated_npc_locations or {})[name] or ""
--            end,
--            setLocation = function(name, loc)
--                if is_self(name) and state.generated_npc_locations then
--                    state.generated_npc_locations[npc_id] = loc
--                end
--            end,
--            isInLocation   = function(name, loc) return is_self(name) and (state.generated_npc_locations or {})[npc_id] == loc end,
--            countInLocation = function(loc)
--                local n = (state.player.location == loc) and 1 or 0
--                for _, l in pairs(state.generated_npc_locations or {}) do if l == loc then n=n+1 end end
--                return n
--            end,
--            getAppearance = function() return "NORMAL" end,
--            setAppearance = function() end,
--        }
--    end

-- 5. NPC tick — call from after_ai_turn (or inside advance_time tool):
--    local function tick_npc_routines()
--        if not next(generated_npc_objects) then return end
--        NPC.tick(generated_npc_objects, state.time, state.day, state.player.location)
--        -- NPC.tick calls setLocation internally via the world adapter — no extra update needed.
--    end

-- 6. Pre-seed specific NPCs at game start (idempotent — skips if file exists):
--    function set_initial_state(player_input)
--        state = default_state()
--        -- Generate only if the .lua file doesn't exist yet:
--        local function needs_gen(id)
--            if persona.get(id) then return false end
--            local f = io.open("./scripts/npcs/" .. id .. ".lua", "r")
--            if f then f:close(); return false end
--            return true
--        end
--        if needs_gen("shopkeeper_12") then
--            persona.generate("shopkeeper_12",
--                "id=shopkeeper_12. Runs the general store on the main square. "
--                .. "Middle-aged, gruff but fair. Knows all the village gossip.")
--        end
--    end

-- 7. In get_tools() — expose persona tools + think_as_npc with structured output:
--    The structured output schema for think_as_npc (prevents paraphrasing):
--    local _NPC_THINK_SCHEMA = [[{
--        "type": "object",
--        "required": ["intent", "speech"],
--        "properties": {
--            "intent": { "type": "string", "description": "1-2 sentences: what the NPC wants, emotional state. For master LLM only." },
--            "speech": { "type": "string", "description": "Exact words the NPC speaks. Copy VERBATIM to narration." }
--        }
--    }]]
--
--    function get_tools()
--        return tools_lib.build({
--            -- Generate a new NPC on first physical encounter:
--            persona.as_tool_generate(
--                "Generate a new character the player meets for the first time. "
--                .. "Call ONCE before narrating the encounter. Idempotent."),
--
--            -- Record a significant life event (persists to disk immediately):
--            persona.as_tool_life_event(
--                "Record a significant permanent event for an NPC (agreement, trauma, relationship change). "
--                .. "Use after think_as_npc confirmed the event happened."),
--
--            -- think_as_npc with structured {intent, speech} output:
--            {
--                name = "think_as_npc",
--                description = "Ask a generated NPC how they react. "
--                           .. "Returns {intent, speech}. Copy 'speech' VERBATIM to narration.",
--                params = [[{ "type":"object", "required":["id","situation"],
--                             "properties": {
--                                 "id":        { "type":"string" },
--                                 "situation": { "type":"string" }
--                             }}]],
--                fn = function(args_json)
--                    local a = json.decode(args_json)
--                    local npc_id = a.id or ""
--                    if not persona.get(npc_id) then
--                        return json.encode({ error="NPC not found — call generate_npc first." })
--                    end
--                    if not generated_agents[npc_id] then
--                        local adapter = make_gen_world_adapter(npc_id)
--                        generated_npc_objects[npc_id] = persona.npc_object(npc_id, adapter)
--                        generated_agents[npc_id] = persona.agent_object(npc_id, {
--                            npc = generated_npc_objects[npc_id], turn_counter = gen_tc,
--                        })
--                    end
--                    local result = generated_agents[npc_id]:decide(a.situation or "", _NPC_THINK_SCHEMA)
--                    state.npc_met = state.npc_met or {}
--                    state.npc_met[npc_id] = true
--                    return result
--                end,
--            },
--        })
--    end

-- 8. In get_system_prompt() — inject NPC personas present at player location:
--    local persona_blocks = {}
--    for _, npc_id in ipairs(persona.known_ids()) do
--        if (state.generated_npc_locations or {})[npc_id] == state.player.location then
--            local blk = persona.format(npc_id)
--            if blk and blk ~= "" then
--                table.insert(persona_blocks, "=== " .. (persona.get(npc_id).name or npc_id) .. " ===\n" .. blk)
--            end
--        end
--    end

-- 9. In get_state_snapshot() / restore_state():
--    function get_state_snapshot()
--        local snap = json.decode(json.encode(state))
--        snap._world = world.snapshot()   -- if also using world.lua
--        return json.encode(snap)
--    end
--    function restore_state(snapshot)
--        local ok, data = pcall(json.decode, snapshot)
--        if not ok then return { success=false, error=tostring(data) } end
--        if data._world then world.restore(data._world) end
--        state = data.state or data
--        persona.reload_all()   -- reload NPC .lua files from disk (authoritative source)
--        -- Rebuild live NPC objects for all known NPCs:
--        generated_agents = {}; generated_npc_objects = {}
--        for id in pairs(state.generated_npc_locations or {}) do
--            local p = persona.get(id)
--            if p then
--                local adapter = make_gen_world_adapter(id)
--                generated_npc_objects[id] = persona.npc_object(id, adapter)
--                generated_agents[id] = persona.agent_object(id, { npc=generated_npc_objects[id], turn_counter=gen_tc })
--            end
--        end
--        return { success=true }
--    end

-- 10. Dream system — call from after_ai_turn or /sleep command:
--     persona.dream_tick(state.time, state.giorno_index, state.npc_last_dream, generated_npc_objects)
--     -- dream_tick runs between 01:00-05:00, one NPC per call.
--     -- It adds sequences/needs to the NPC file and updates npc_summary.
--     -- To force all NPCs to dream (e.g. on /sleep):
--     --   for _ = 1, 30 do
--     --       if not persona.dream_tick("03:00", day_index, state.npc_last_dream, generated_npc_objects) then break end
--     --   end

-- DESIGN NOTES:
-- persona.lua is file-backed: each NPC's .lua file is the ground truth.
-- Save JSON does NOT contain NPC data — restore_state() reloads from disk.
-- dream_tick() grows NPCs nightly: adds sequences (new behaviours), needs (stat-driven
-- triggers), npc_summary (compressed persona for agent prompt). After ~5 sessions an
-- NPC has a significantly richer personality than at generation.
-- agent_system corruption check: if agent_system < 80 chars or starts with "System."
-- the agent is reconstructed from persona fields automatically (persona.agent_object).
-- LLM generation sanity check: generated routine location_ids must follow {id}_{room}
-- format; agent_system must be >= 80 chars. Rejected generations are logged to
-- /tmp/persona_generate_reject.log and return nil (caller can retry).

]]--


-- =============================================================================
-- END OF TEMPLATE
-- =============================================================================
