-- =============================================================================
--  template.lua  —  RpgAi Adventure Template
--
--  ┌─────────────────────────────────────────────────────────────────────────┐
--  │  FOR CLAUDE — when asked to create a new adventure:                     │
--  │  DEFAULT PATH is scripts/template_min.lua (declarative quick.define     │
--  │  spec, lib/quickstart.lua). Use THIS template only for features         │
--  │  quickstart does not wire: world.lua procedural locations/objects,      │
--  │  npc.lua code-driven routines, adventure events files, session-         │
--  │  isolated personas.                                                     │
--  │  On this path: READ this entire header, then ASK the questions in       │
--  │  §DECISIONS before writing any code. No code until you have answers.    │
--  └─────────────────────────────────────────────────────────────────────────┘
--
--  FOR HUMAN AUTHORS:
--    0. Simple adventure? Start from template_min.lua instead (way less code)
--    1. Copy this file to my_scripts/my_adventure.lua
--    2. Answer the §DECISIONS questions to know which blocks to keep
--    3. Follow [STEP N] markers, delete OPTIONAL blocks you don't need
--    4. Run: ./build/rpgai --web --path my_scripts/ --script my_adventure.lua
--           [+ provider flags: --provider openrouter --or-key ... --or-model ...]
--
-- =============================================================================
--  §DECISIONS — answer these before writing a single line of code
-- =============================================================================
--
--  1. GENRE & TONE
--     What setting? (fantasy / contemporary / sci-fi / horror / adult / other)
--     Content level? (family / suggestive / explicit)
--     Language? (it / en / other) — affects system prompt and NPC dialogue
--     Narrative person? (second person "you" is standard; first person possible)
--
--  2. ARCHITECTURE MODE — pick one, shapes everything else
--     MODE A — Schema only (no tools). Simple. Works with any provider/model.
--              State changes (location, inventory, HP) live in JSON response fields.
--              Best for: local models, simple linear stories, quick prototypes.
--     MODE B — Tools only. Minimal schema (narration only). All state via tools.
--              Best for: complex NPC interaction, procedural worlds, cloud models.
--     MODE C — Mix. Tools for complex state (move, NPC react), schema for simple
--              atomic outcomes (game_over, custom flags). Most flexible.
--              Recommended default for new adventures with NPCs.
--
--  3. FEATURES CHECKLIST — which optional systems to include?
--     [ ] Time + day cycle         → advance_time tool / giorno_index
--     [ ] Inventory + money        → cambia_inventario tool / state.inventario
--     [ ] NPC agents (lib/agent)   → think_as_npc tool / init_agents()
--     [ ] Persistent memory        → memory.lua / memory_write + memory_read tools
--     [ ] Notes with scope         → remember tool (player/public/npc scopes)
--     [ ] Procedural locations     → world.lua / generate_location tool
--     [ ] Procedural objects       → world.lua / generate_object + object_action tools
--     [ ] Procedural NPCs          → persona.lua / generate_npc tool
--     [ ] Plot seeds (trame)       → trame/ dir / assegna_ruolo_trama tool
--     [ ] Image generation         → get_scene_images / get_asset_prompt
--     [ ] Code-driven NPCs (npc.lua) → routines, needs, events
--     [ ] Debug commands           → /debug_log, /set_field etc.
--     [ ] Custom /commands         → process_player_input stubs
--
--  4. NPCS
--     How many pre-defined NPCs? (list names, roles, personalities)
--     Are they pre-defined in code (static) or generated on-demand (persona.lua)?
--     Which NPCs get an LLM agent? (all recommended; at minimum: key characters)
--     Max agent LLM calls per turn? (default 3; increase for many active NPCs)
--
--  5. WORLD
--     List all locations upfront (static) or expand procedurally (world.lua)?
--     Travel map: free movement or restricted?
--     Starting location?
--
--  6. PROVIDER
--     Local (ollama) or cloud (openrouter/claude/openai)?
--     Note: MODE B/C tools require a provider with tool-calling support.
--     Local models (ollama) work best with MODE A or simple MODE C schemas.
--
--  7. PRIVATE SCRIPT?
--     If yes → place in my_scripts/, not scripts/
--     Never reference private script names in commits, README, or public output.
--
-- =============================================================================
--  AVAILABLE FEATURES (all optional except REQUIRED)
-- =============================================================================
--    REQUIRED:  json, get_welcome_message, set_initial_state,
--               get_status_for_ai, get_system_prompt, get_json_schema,
--               process_ai_response, process_player_input, get_display_state,
--               get_state_snapshot, restore_state
--
--    MODE B/C:  get_tools() with: think_as_npc, move_player, move_npc,
--               advance_time, set_activity, cambia_inventario, remember,
--               generate_npc, generate_location, generate_object, object_action,
--               npc_life_event, memory_write, memory_read
--
--    HOOKS:     before_ai_turn (reset _tool_calls + agents each turn)
--               after_ai_turn  (side effects after LLM response)
--
--    LIBS:      lib/agent.lua   — LLM-driven NPC agents
--               lib/memory.lua  — cross-session persistent NPC facts
--               lib/npc.lua     — code-driven NPC routines/needs
--               lib/world.lua   — procedural location/object generation
--               lib/persona.lua — file-backed procedural NPC generation
--               lib/tools.lua   — tools.build() + roll_dice, skill_check
--               lib/json_repair — safe_json_decode() global
--
-- =============================================================================

-- ── RECOMMENDED: use adventure.lua framework to eliminate boilerplate ────────
-- local adv       = require("lib/adventure")  -- shared tools, HUD, save/restore
-- adv.set_config(CFG)                         -- set feature flags (see §DECISIONS)
--
-- With adventure.lua, your script only needs:
--   - LOCATIONS, TRAVEL_MAP, MAIN_NPCS config tables
--   - get_welcome_message, set_initial_state, get_json_schema
--   - process_ai_response (adventure-specific state only)
--   - get_system_prompt (header + rules; common blocks via adv.prompt_*)
--   - Adventure-specific tools (pass as `extra` to adv.get_tools())
--   - get_tools(), before_ai_turn(), get_display_state(), get_state_snapshot(),
--     restore_state() all become 1-3 line wrappers.
-- See pharma_ceo.lua for a complete example.
-- ─────────────────────────────────────────────────────────────────────────────
--
-- ── NPC SYSTEM — two tiers ───────────────────────────────────────────────────
-- MAIN NPCs (hand-crafted, evolve over time):
--   1. Define MAIN_NPCS config table (name, age, relationship, personality,
--      agent_system, short/long_term_goals). Other fields optional.
--   2. persona.register_static(id, cfg) in set_initial_state → writes
--      npcs/<adventure>/<id>.lua if missing; loads from disk if exists.
--   3. persona.reload_all() → authoritative source is the .lua file on disk.
--   4. rebuild_npc_data() → NPC_DATA for adv.prompt_npc_* built from persona.
--   5. npc_life_event tool and dream_tick apply to main NPCs same as generated.
--
-- GENERATED NPCs (LLM-created on demand):
--   1. generate_npc(id, context) tool writes npcs/<adventure>/<id>.lua.
--   2. Positions tracked in state.gen_npc_locations (separate from npc_locations).
--   3. move_npc accepts both static and generated NPCs.
--   4. npc_life_event applies to them too.
-- ─────────────────────────────────────────────────────────────────────────────

-- ── REQUIRED ─────────────────────────────────────────────────────────────────
local json        = require("json")
-- ── OPTIONAL: native GUI visual world (rpgai-gui only) ───────────────────────
-- local visual = require("lib/visual")  -- tile map + NPC sprites for rpgai-gui
-- ── OPTIONAL: uncomment as needed ────────────────────────────────────────────
-- require("lib/json_repair")            -- registers safe_json_decode() as global
-- local tools_lib   = require("lib/tools")     -- pre-built tool definitions
-- local NPC_lib     = require("lib/npc")       -- code-driven NPC engine
-- local agent_lib   = require("lib/agent")     -- LLM-driven NPC reactions
-- local memory      = require("lib/memory")    -- cross-session persistent memory
-- local persona     = require("lib/persona")   -- NPC persona files (main + generated)
-- local world       = require("lib/world")     -- procedural world expansion
-- ─────────────────────────────────────────────────────────────────────────────

-- ── MODE B/C: per-turn tool call guard ───────────────────────────────────────
-- Reset in before_ai_turn(). Each tool checks _tool_calls[key] before running.
-- For think_as_npc use key "think_as_npc_"..npc_id.."_result" (cache pattern).
-- local _tool_calls = {}
-- ─────────────────────────────────────────────────────────────────────────────

-- ── MODE B/C: NPC agent response schema ──────────────────────────────────────
-- Pass as second arg to agent:decide(). Separates thought from spoken dialogue.
-- "intent" = internal state, narrate in 3rd person, NEVER quote.
-- "speech" = exact words said aloud, copy VERBATIM with « » in narration.
--[==[
local _NPC_THINK_SCHEMA = [[{
    "type": "object",
    "required": ["intent", "speech"],
    "properties": {
        "intent": { "type": "string",
                    "description": "1-2 sentences: what the NPC thinks/feels right now. NOT dialogue." },
        "speech": { "type": "string",
                    "description": "Exact words spoken aloud. Empty string if NPC says nothing." }
    }
}]]
]==]
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
        turn         = 0,
        time         = "08:00",
        day          = "monday",
        giorno_index = 1,   -- absolute day counter (day 1, 2, 3…); increment in /sleep command

        -- [STEP 3 — OPTIONAL] Inventory + money
        -- inventory: free-form string list (no enum). cambia_inventario tool manages it.
        inventory = {},     -- e.g. { "house keys", "phone", "notebook" }
        money     = 0,      -- integer, any currency unit

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
        narrative_context = {},

        -- [STEP 3 — OPTIONAL] Notes with scope (from remember tool)
        -- Each entry: { date="HH:MM day", content="...", scope="player"|"public"|"npc:id" }
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

    -- Shared turn counter: max N total agent LLM calls per turn across ALL agents
    turn_counter = agent_lib.new_turn_counter(3)

    -- One agent per NPC. agent:as_tool() works (decodes args_json) but creates
    -- one tool per NPC; prefer the generic think_as_npc tool in get_tools()
    -- which dispatches by id and adds per-turn caching + location validation.
    agents["mira"] = agent_lib.new("mira", {
        system   = "You are Mira, the village innkeeper. "
                .. "You are warm, sharp-eyed, and know everyone's secrets. "
                .. "Always stay in character. Reply concisely (2-3 sentences).",
        model    = nil,           -- nil = use engine default; or "llama3.2"
        provider = nil,           -- nil = use engine default; or "ollama"
        npc          = npcs["mira"],  -- links code-driven state for fallback
        turn_counter = turn_counter,  -- shared cap across all agents
        short_term_goals = { "serve the tavern customers" },
        long_term_goals  = { "keep the peace in the village" },
        memory_enabled   = true,
    })
    -- [STEP 4] Add one agent per NPC here. Same pattern for each.
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

-- [STEP 5 — OPTIONAL] Debug tool logger
-- Set DEBUG_LOG=true at module level to enable. Collects tool call trace in _dbg_tools.
-- local DEBUG_LOG = false
-- local _dbg_tools = {}
-- local function _log_tool(name, args, result)
--     if not DEBUG_LOG then return end
--     table.insert(_dbg_tools, { name=name, args=args,
--         result=type(result)=="string" and result:sub(1,200) or tostring(result) })
-- end

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
-- [OPTIONAL] Character-creation questionnaire.
-- Return a list of scripted questions asked BEFORE the game starts. The engine
-- drives the Q&A in the UI, collects answers as { field = answer }, and passes
-- them to set_initial_state() as a JSON string.
--   type = "text"   → free-text answer
--   type = "choice" → quick-pick buttons (player can still type a custom value)
-- Keep questions to ESTETICA the NPCs can SEE (body, hair, clothes) — not
-- personality. Personality the game can't enforce is wasted text; visible
-- traits feed NPC reactions, narration, and image generation.
-- Delete this function entirely to keep the classic single-name prompt.
-- ---------------------------------------------------------------------------
function get_character_questions()
    return {
        { field="name",    prompt="Come ti chiami?",            type="text" },
        { field="eta",     prompt="Quanti anni hai?",           type="text" },
        { field="sesso",   prompt="Sesso?",                     type="choice",
          options={ "uomo", "donna" } },
        { field="capelli", prompt="Com'è la tua capigliatura?", type="choice",
          options={ "corti scuri", "lunghi biondi", "rasati", "ricci castani", "brizzolati" } },
        { field="corpo",   prompt="Corporatura?",               type="choice",
          options={ "atletico", "esile", "robusto", "minuto" } },
        { field="vestiti", prompt="Come sei vestito?",          type="text" },
    }
end

-- ---------------------------------------------------------------------------
-- Called after the player finishes the questionnaire (or types their name).
-- When get_character_questions() exists, `player_input` is a JSON string of all
-- answers. Otherwise it is the raw name (classic flow). Handle both.
-- ---------------------------------------------------------------------------
function set_initial_state(player_input)
    state = default_state()

    local answers = nil
    if player_input and player_input:match("^%s*{") then
        local ok, decoded = pcall(json.decode, player_input)
        if ok then answers = decoded end
    end

    if answers then
        state.player.name = answers.name or "Adventurer"
        -- One source for visible traits → NPC prompts + image prompts + arrival.
        -- adv.player_appearance() reads state.player.appearance / .outfit.
        state.player.appearance = string.format(
            "%s, %s anni, corporatura %s, capelli %s.",
            answers.sesso or "?", answers.eta or "?",
            answers.corpo or "?", answers.capelli or "?")
        state.player.outfit = answers.vestiti or ""
        -- [OPTIONAL] register the player as a persona for uniform handling:
        -- persona.register_static("player", {
        --     name=state.player.name, age=tonumber(answers.eta),
        --     appearance=state.player.appearance, outfit_override=state.player.outfit })
        -- then: state.player.appearance = persona.format_appearance("player")
    elseif player_input and player_input ~= "" then
        state.player.name = player_input
    end

    -- [STEP 6 — OPTIONAL] Init NPC objects after state is ready
    -- init_npcs()
    -- run_npc_tick()
end

-- Called if the player pressed Enter with empty input (no questionnaire).
function generate_initial_state()
    set_initial_state("Adventurer")
end

-- ---------------------------------------------------------------------------
-- [OPTIONAL] Arrival scene. Called once after set_initial_state(), before the
-- first player turn. Return a narration string shown as turn 0. The master owns
-- the prompt template; the LLM weaves in the player's visible traits and the
-- NPCs currently present (or just generated). Delete to skip the arrival scene.
-- ---------------------------------------------------------------------------
function generate_arrival()
    local sys = [[Sei il narratore di questa avventura. Scrivi la scena d'arrivo
del protagonista in seconda persona, 3-5 frasi, atmosferica. Descrivi come gli
altri presenti lo vedono in base al suo aspetto. Non inventare azioni del
giocatore: descrivi solo l'ambiente, l'arrivo e le prime reazioni.]]

    local loc = LOCATIONS[state.player.location] or {}
    local present = npcs_in_location(state.player.location)
    local user = table.concat({
        "Protagonista: " .. (adv and adv.player_appearance() or state.player.name),
        "Luogo: " .. (loc.name or state.player.location) .. " — " .. (loc.desc or ""),
        "Presenti: " .. (next(present) and json.encode(present) or "nessuno"),
    }, "\n")

    -- query_llm(sys, history_json, user, schema, model, provider, label)
    local schema = [[{ "type":"object", "required":["narration"],
                       "properties":{ "narration":{"type":"string"} } }]]
    local ok, reply = pcall(query_llm, sys, "[]", user, schema, nil, nil, "narrator")
    if not ok then return "" end
    local okd, data = pcall(json.decode, reply)
    if okd and type(data) == "table" and not data.error and data.narration then
        return data.narration
    end
    return ""  -- LLM failed or returned no narration → skip the arrival scene
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

    -- Notes with scope (from remember tool).
    -- scope="player" and "public" go into master prompt.
    -- scope="npc:id" is injected only into think_as_npc for that NPC (handled in tool fn).
    local notes_block = ""
    if state.notes and #state.notes > 0 then
        local player_notes, public_notes = {}, {}
        for _, n in ipairs(state.notes) do
            if type(n) == "string" then
                table.insert(player_notes, n)  -- backward compat
            elseif n.scope == "public" then
                table.insert(public_notes, n.content or "")
            elseif (not n.scope) or n.scope == "player" then
                table.insert(player_notes, n.content or "")
            end
        end
        if #player_notes > 0 then
            notes_block = notes_block .. "\n\n## PERSONAL NOTES (only you know)\n"
            for _, n in ipairs(player_notes) do notes_block = notes_block .. "- " .. n .. "\n" end
        end
        if #public_notes > 0 then
            notes_block = notes_block .. "\n\n## PUBLIC FACTS (everyone knows)\n"
            for _, n in ipairs(public_notes) do notes_block = notes_block .. "- " .. n .. "\n" end
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
    -- [STEP 6 — MODE B/C] Uncomment and adapt to the tools you actually register.
    local tool_rules = ""
    --[==[
    tool_rules = [[

════════════════════════════════════════════════════════════════
WORKFLOW — MANDATORY ORDER (call tools BEFORE narrating)
════════════════════════════════════════════════════════════════
 1. [opt] think_as_npc(id, situation) — NPC reaction. Cached/turn. MAX 1 per NPC.
 2. [opt] advance_time(minutes)       — BEFORE any action that takes time. MAX 1/turn.
 3. [opt] move_player(location)       — explicit player movement. MAX 1/turn.
 4. [opt] move_npc(id, location)      — NPC movement before narrating it. MAX 1/NPC/turn.
 5. [opt] set_activity(npc, activity) — update NPC activity without moving. MAX 1/NPC/turn.
 6. [opt] cambia_inventario(...)      — ONLY on explicit pickup/drop/spend/earn. FORBIDDEN for implied costs.
 7. [opt] remember(note, scope)       — player note. scope: player|public|npc. MAX 2/turn.
 8. [opt] generate_npc(id, context)   — new NPC met face-to-face. MAX 2/turn.
 9. [opt] generate_location(id)       — location never visited. BEFORE entering it.
10. [opt] generate_object(id)         — new object. BEFORE describing/using it.
11. [opt] object_action(id, action)   — action on existing object.
12. [opt] npc_life_event(id, patch)   — permanent NPC change. Persists to disk.
13. [opt] memory_write(entity, ...)   — confirmed fact about an NPC. MAX 3/turn.
14. [opt] memory_read(entity, ...)    — read persistent fact.
Then: write narration. STOP.

CRITICAL — think_as_npc returns { "intent": "...", "speech": "..." }:
  "intent" = internal state — narrate in 3RD PERSON, NEVER quote with «».
  "speech" = exact words spoken aloud — copy VERBATIM between «». Empty = silence.

RULES:
think_as_npc  — only if NPC is in same location as player. Cached: repeat = same result.
move_player   — ONLY on explicit movement. NOT if player is already in the room.
advance_time  — BEFORE narrating time-consuming actions. MAX 1/turn.
move_npc      — BEFORE narrating NPC in new location. MAX 1 per NPC/turn.
memory_write  — confirmed facts only. NOT intentions, deductions, or future plans.
]]
    -- [STEP 6] Add your custom tools here following the same pattern.
    ]==]

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
    -- Use safe_json_decode (registered by require("lib/json_repair")) for auto-repair.
    -- Falls back to plain decode if json_repair not loaded.
    local r, _, err
    if safe_json_decode then
        r, _, err = safe_json_decode(reply)
    else
        local ok; ok, r = pcall(json.decode, reply)
        if not ok then err = r; r = nil end
    end
    if not r or type(r) ~= "table" then
        return { success=false, error="Invalid JSON: " .. tostring(err or reply) }
    end
    if not r.narration or r.narration == "" then
        return { success=false, error="Missing narration" }
    end

    -- MODE A only: move player and advance time from schema fields.
    -- In MODE B/C these are handled by move_player / advance_time tools — delete these blocks.
    if r.new_location and r.new_location ~= "" and LOCATIONS[r.new_location] then
        -- [STEP 6] Add movement restriction checks here if needed.
        state.player.location = r.new_location
    end
    if r.time_passes and tonumber(r.time_passes) and tonumber(r.time_passes) > 0 then
        advance_time(tonumber(r.time_passes))
    end

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
        local loc_id = state.player.location
        local exits  = TRAVEL_MAP[loc_id] or {}
        local cur    = LOCATIONS[loc_id]
        local lines  = {
            "Location: " .. (cur and cur.name or loc_id) .. "  [" .. loc_id .. "]",
            "Exits:"
        }
        for _, id in ipairs(exits) do
            local dest = LOCATIONS[id]
            table.insert(lines, "  • " .. (dest and dest.name or id) .. "  [" .. id .. "]")
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
    -- [STEP 6] Adapt fields to your adventure (remove HP/gold if not used, add money etc.)
    local inv = state.inventory or state.player.inventory or {}
    local inv_str = #inv > 0 and table.concat(inv, ", ") or "(empty)"
    return string.format("[ %s  |  %s %s  |  📍%s  |  here: %s ]\n[ HP:%d/%d  💰%d  🎒%s ]",
        state.player.name,
        state.time, state.day,
        loc and loc.name or state.player.location,
        #npcs_here > 0 and table.concat(npcs_here, ", ") or "—",
        state.player.hp or 0, state.player.max_hp or 0,
        state.money or state.player.gold or 0,
        inv_str)
end

-- ---------------------------------------------------------------------------
-- Save / Load
-- ---------------------------------------------------------------------------
function get_state_snapshot()
    local snap = {}
    for k, v in pairs(state) do snap[k] = v end
    -- [STEP 6 — OPTIONAL] Include agent history snapshots:
    -- snap._agents = {}
    -- for id, ag in pairs(agents) do snap._agents[id] = ag:agent_snapshot() end
    return json.encode(snap)
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then
        return { success=false, error="Failed to parse snapshot: " .. tostring(data) }
    end
    -- [STEP 6 — OPTIONAL] Restore agent history:
    -- local agent_data = data._agents; data._agents = nil
    state = data
    -- Re-init NPC objects after load (they are not JSON-serialisable):
    -- init_npcs()
    -- if agent_data then
    --     for id, snap_str in pairs(agent_data) do
    --         if agents[id] then agents[id]:agent_restore(snap_str) end
    --     end
    -- end
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
-- Requires: MODE B or C, provider with tool calling (OpenAI, OpenRouter, Claude).
-- Delete this function entirely if using MODE A (schema-only).
--
-- NOTE: agent:as_tool() works (one tool per NPC), but the generic think_as_npc
-- stub below is preferred: single tool, per-turn cache, location validation.
-- =============================================================================
--[==[
function get_tools()
    return tools_lib.build({

        -- ── think_as_npc — generic, dispatches by id ─────────────────────────
        -- One tool for ALL NPCs. Never create per-NPC tools (think_as_mira etc).
        -- Cached per turn: repeated calls for same id return the same result.
        {
            name = "think_as_npc",
            description = "Ask an NPC how they react to the current situation. "
                       .. "Only call if the NPC is in the same location as the player. "
                       .. "Cached per turn — repeat calls return the same result. "
                       .. "Returns {intent, speech}. "
                       .. "'intent' = internal state — narrate in 3RD PERSON, NEVER quote. "
                       .. "'speech' = exact words spoken — copy VERBATIM between «» in narration. "
                       .. "Empty 'speech' = NPC says nothing this turn.",
            params = [[{
                "type": "object",
                "required": ["id", "situation"],
                "properties": {
                    "id":        { "type": "string",
                                   "description": "NPC id (e.g. 'mira', 'guard')" },
                    "situation": { "type": "string",
                                   "description": "Observable facts only: what happened, where, what was said. Neutral — do NOT describe the expected response." }
                }
            }]],
            fn = function(args_json)
                local a      = json.decode(args_json)
                local npc_id = a.id or ""
                local cache  = "think_as_npc_" .. npc_id
                if _tool_calls[cache .. "_result"] then
                    return _tool_calls[cache .. "_result"]
                end
                if not agents[npc_id] then
                    return json.encode({ error="NPC '" .. npc_id .. "' has no agent." })
                end
                _tool_calls[cache] = true
                -- Inject public + npc-specific notes into situation
                local situation = a.situation or ""
                local shared = {}
                for _, n in ipairs(state.notes or {}) do
                    if type(n) == "table" then
                        if n.scope == "public" or n.scope == ("npc:" .. npc_id) then
                            table.insert(shared, n.content or "")
                        end
                    end
                end
                if #shared > 0 then
                    situation = situation .. "\n[Recent shared info: " .. table.concat(shared, "; ") .. "]"
                end
                local result = agents[npc_id]:decide(situation, _NPC_THINK_SCHEMA)
                _tool_calls[cache .. "_result"] = result
                return result
            end,
        },

        -- ── move_player ───────────────────────────────────────────────────────
        -- Call BEFORE narrating movement. Validates against TRAVEL_MAP.
        {
            name = "move_player",
            description = "Move the player to a new location. "
                       .. "Call BEFORE narrating the movement. "
                       .. "Only if player explicitly moves. NOT if already in the room. MAX 1/turn.",
            params = [[{
                "type": "object",
                "required": ["location"],
                "properties": {
                    "location": { "type": "string", "description": "location_id of destination" }
                }
            }]],
            fn = function(args_json)
                if _tool_calls["move_player"] then
                    return json.encode({ error="move_player already called this turn." })
                end
                _tool_calls["move_player"] = true
                local a   = json.decode(args_json)
                local loc = a.location or ""
                if not LOCATIONS[loc] then
                    return json.encode({ error="Unknown location: " .. loc })
                end
                local exits = TRAVEL_MAP[state.player.location] or {}
                local valid = false
                for _, e in ipairs(exits) do if e == loc then valid = true; break end end
                if not valid then
                    return json.encode({ error="'" .. loc .. "' not reachable from here." })
                end
                state.player.location = loc
                -- [STEP 7] Add per-location side-effects here (e.g. set flags).
                local dest = LOCATIONS[loc]
                return json.encode({ ok=true, location=loc, name=dest and dest.name or loc })
            end,
        },

        -- ── move_npc ─────────────────────────────────────────────────────────
        -- Call BEFORE narrating NPC in new location. MAX 1 per NPC per turn.
        {
            name = "move_npc",
            description = "Move an NPC to a new location. "
                       .. "Call BEFORE narrating the movement. MAX 1 per NPC per turn.",
            params = [[{
                "type": "object",
                "required": ["id", "location"],
                "properties": {
                    "id":       { "type": "string", "description": "NPC id" },
                    "location": { "type": "string", "description": "destination location_id" },
                    "activity": { "type": "string", "description": "What the NPC is doing there (optional)" }
                }
            }]],
            fn = function(args_json)
                local a   = json.decode(args_json)
                local id  = a.id or ""
                local key = "move_npc_" .. id
                if _tool_calls[key] then
                    return json.encode({ error="move_npc already called this turn for " .. id .. "." })
                end
                if not NPC_DATA[id] then
                    return json.encode({ error="NPC not found: " .. id })
                end
                _tool_calls[key] = true
                state.npc_locations[id] = a.location
                if a.activity and a.activity ~= "" then
                    state.npc_activities        = state.npc_activities or {}
                    state.npc_activities[id]    = a.activity
                end
                return json.encode({ ok=true, id=id, location=a.location })
            end,
        },

        -- ── advance_time ──────────────────────────────────────────────────────
        -- Call BEFORE narrating any time-consuming action. MAX 1/turn.
        {
            name = "advance_time",
            description = "Advance game time. "
                       .. "Call BEFORE narrating any action that takes time. MAX 1/turn.",
            params = [[{
                "type": "object",
                "required": ["minutes"],
                "properties": {
                    "minutes": { "type": "integer", "minimum": 1, "maximum": 480,
                                 "description": "Minutes to advance" }
                }
            }]],
            fn = function(args_json)
                if _tool_calls["advance_time"] then
                    return json.encode({ error="advance_time already called this turn." })
                end
                _tool_calls["advance_time"] = true
                local a = json.decode(args_json)
                advance_time(math.max(1, math.min(480, tonumber(a.minutes) or 30)))
                return json.encode({ ok=true, time=state.time, day=state.day })
            end,
        },

        -- ── set_activity ──────────────────────────────────────────────────────
        -- Update what an NPC is doing without moving them. MAX 1 per NPC per turn.
        {
            name = "set_activity",
            description = "Update what an NPC is doing in their current location. "
                       .. "MAX 1 per NPC per turn.",
            params = [[{
                "type": "object",
                "required": ["id", "activity"],
                "properties": {
                    "id":       { "type": "string", "description": "NPC id" },
                    "activity": { "type": "string", "description": "Short activity description (max 15 words)" }
                }
            }]],
            fn = function(args_json)
                local a   = json.decode(args_json)
                local id  = a.id or ""
                local key = "set_activity_" .. id
                if _tool_calls[key] then
                    return json.encode({ error="set_activity already called this turn for " .. id })
                end
                _tool_calls[key] = true
                if not NPC_DATA[id] and not (state.npc_locations or {})[id] then
                    return json.encode({ error="NPC not found: " .. id })
                end
                state.npc_activities        = state.npc_activities or {}
                state.npc_activities[id]    = a.activity
                return json.encode({ ok=true, id=id, activity=a.activity })
            end,
        },

        -- ── cambia_inventario ─────────────────────────────────────────────────
        -- Modify player inventory and/or money. Free-form item names (no enum).
        -- ONLY call on explicit player action — NEVER for implied narrative costs.
        {
            name = "cambia_inventario",
            description = "Modify player inventory and/or money. "
                       .. "Call ONLY when the player explicitly picks up, drops, buys, sells, or spends. "
                       .. "FORBIDDEN for implied/background costs the player didn't describe.",
            params = [[{
                "type": "object",
                "properties": {
                    "add":    { "type": "array", "items": { "type": "string" },
                                "description": "Items received or picked up." },
                    "remove": { "type": "array", "items": { "type": "string" },
                                "description": "Items used, given away, or lost. Use exact name already in inventory." },
                    "money":  { "type": "integer",
                                "description": "Money delta: positive=earned, negative=spent." }
                }
            }]],
            fn = function(args_json)
                local a = json.decode(args_json)
                state.inventory = state.inventory or {}
                state.money     = state.money     or 0
                if type(a.remove) == "table" then
                    for _, item in ipairs(a.remove) do
                        for i, existing in ipairs(state.inventory) do
                            if existing:lower() == item:lower() then
                                table.remove(state.inventory, i); break
                            end
                        end
                    end
                end
                if type(a.add) == "table" then
                    for _, item in ipairs(a.add) do
                        table.insert(state.inventory, item)
                    end
                end
                if type(a.money) == "number" then
                    state.money = state.money + a.money
                end
                -- _log_tool("cambia_inventario", a, "ok")
                return json.encode({ ok=true, inventory=state.inventory, money=state.money })
            end,
        },

        -- ── remember ──────────────────────────────────────────────────────────
        -- Player notes with scope. scope="npc:id" gets injected into think_as_npc for that NPC.
        {
            name = "remember",
            description = "Save a note for future turns. MAX 2/turn. "
                       .. "scope: 'player'=only you (default), "
                       .. "'public'=fact everyone in the world knows, "
                       .. "'npc:id'=shared only with a specific NPC.",
            params = [[{
                "type": "object",
                "required": ["note"],
                "properties": {
                    "note":   { "type": "string", "description": "Short fact (max 20 words)." },
                    "scope":  { "type": "string", "enum": ["player","public","npc"],
                                "description": "Who knows this fact." },
                    "npc_id": { "type": "string",
                                "description": "If scope='npc', the NPC id." }
                }
            }]],
            fn = function(args_json)
                _tool_calls["rem_count"] = (_tool_calls["rem_count"] or 0) + 1
                if _tool_calls["rem_count"] > 2 then
                    return json.encode({ error="remember: max 2/turn." })
                end
                local a     = json.decode(args_json)
                local note  = (a.note or ""):match("^%s*(.-)%s*$")
                if note == "" then return json.encode({ error="empty note" }) end
                local scope = a.scope or "player"
                if scope == "npc" and a.npc_id and a.npc_id ~= "" then
                    scope = "npc:" .. a.npc_id
                end
                state.notes = state.notes or {}
                table.insert(state.notes, {
                    date    = state.time .. " " .. state.day,
                    content = note,
                    scope   = scope,
                })
                if #state.notes > 25 then table.remove(state.notes, 1) end
                -- _log_tool("remember", { note=note, scope=scope }, "ok")
                return json.encode({ ok=true, note=note, scope=scope })
            end,
        },

        -- ── Persistent memory (cross-session) ─────────────────────────────────
        {
            name = "memory_write",
            description = "Save a confirmed fact about an NPC — only what physically happened "
                       .. "or was said VERBATIM. FORBIDDEN: future tense, inferred emotions, "
                       .. "plans not yet acted on. MAX 3/turn.",
            params = [[{ "type":"object", "required":["entity","category","content"],
                         "properties": {
                             "entity":   { "type":"string" },
                             "category": { "type":"string" },
                             "content":  { "type":"string", "description": "max 30 words" }
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

        -- ── Custom tool stub ──────────────────────────────────────────────────
        -- Copy this block for each custom tool. Remember to add it to WORKFLOW.
        -- {
        --     name = "my_tool",
        --     description = "What this tool does. MAX 1/turn.",
        --     params = [[{ "type":"object", "required":["arg1"],
        --                  "properties": { "arg1": { "type":"string" } } }]],
        --     fn = function(args_json)
        --         if _tool_calls["my_tool"] then
        --             return json.encode({ error="my_tool already called this turn." })
        --         end
        --         _tool_calls["my_tool"] = true
        --         local a = json.decode(args_json)
        --         -- do something with a.arg1
        --         return json.encode({ ok=true })
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
    -- Reset tool call guard and agent caches every turn (required for MODE B/C).
    _tool_calls = {}
    agent_lib.reset_all_turns(agents, turn_counter)

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

--[==[

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
--            "intent": { "type": "string", "description": "1-2 sentences: what the NPC is thinking/feeling right now — tensions, doubts, resistance. NOT a statement of desire toward the interaction." },
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
--                                 "situation": { "type":"string", "description": "Observable facts only: what happened, where, what was said. Neutral — do NOT describe the expected response or what you want the NPC to say." }
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

]==]


-- =============================================================================
-- END OF TEMPLATE
-- =============================================================================
