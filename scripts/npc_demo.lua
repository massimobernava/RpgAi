-- =============================================================================
-- npc_demo.lua  —  The Crossroads Inn
-- A minimal fantasy adventure demonstrating the NPC library (lib/npc.lua).
--
-- Two NPCs — Mira the innkeeper and Gareth the blacksmith — live their own
-- lives regardless of what the player does. They follow daily routines, react
-- to needs, execute multi-step sequences, and respond to world events.
-- =============================================================================

local json = require("json")
local NPC  = require("npc")

-- =============================================================================
-- WORLD DATA
-- =============================================================================

local LOCATION_DESC = {
    Tavern      = "a warm tavern smelling of ale and roasted meat",
    Inn_Room    = "a modest room above the tavern with a straw mattress",
    Market      = "the town market, stalls of goods and cheerful chatter",
    Blacksmith  = "the blacksmith's forge — hot, loud, smelling of iron",
    Town_Square = "the open town square with a weathered stone fountain",
    Forest_Edge = "the edge of the dark forest, wind moving through the pines",
}

local LOCATIONS = {}
for k in pairs(LOCATION_DESC) do table.insert(LOCATIONS, k) end
table.sort(LOCATIONS)

-- Abstract travel distances (used by area events)
local DIST = {
    Tavern      = { Inn_Room=1, Market=2, Blacksmith=3, Town_Square=2, Forest_Edge=5 },
    Inn_Room    = { Tavern=1,   Market=3, Blacksmith=4, Town_Square=3, Forest_Edge=6 },
    Market      = { Tavern=2,   Inn_Room=3, Blacksmith=2, Town_Square=1, Forest_Edge=3 },
    Blacksmith  = { Tavern=3,   Inn_Room=4, Market=2, Town_Square=2, Forest_Edge=3 },
    Town_Square = { Tavern=2,   Inn_Room=3, Market=1, Blacksmith=2, Forest_Edge=4 },
    Forest_Edge = { Tavern=5,   Inn_Room=6, Market=3, Blacksmith=3, Town_Square=4 },
}

local function dist_fn(a, b)
    if a == b then return 0 end
    return (DIST[a] and DIST[a][b]) or 999
end

-- =============================================================================
-- STATE
-- =============================================================================

local state = {}

local function default_state()
    return {
        player          = { name = "Stranger", location = "Tavern" },
        time            = "18:00",
        day             = "monday",
        turn            = 0,
        npc_locations   = { Mira = "Tavern",  Gareth = "Blacksmith" },
        npc_outfits     = { Mira = "APRON",   Gareth = "LEATHER_APRON" },
        npc_stats       = {
            Mira   = { energy = 0.8, mood = 0.7, loneliness = 0.2 },
            Gareth = { energy = 0.9, mood = 0.6, thirst     = 0.3 },
        },
        npc_activities    = {},
        npc_engaged       = { Mira = false, Gareth = false },
        npc_memories      = { Mira = {},    Gareth = {} },
        narrative_context = {},
    }
end

-- =============================================================================
-- WORLD ADAPTER FACTORY
-- Each NPC gets its own adapter; all adapters read/write the shared state.
-- =============================================================================

local function make_world()
    return {
        getLocation = function(name)
            if name == "player" then return state.player.location end
            return state.npc_locations[name]
        end,
        setLocation = function(name, loc)
            state.npc_locations[name] = loc
        end,
        isInLocation = function(name, loc)
            if name == "player" then return state.player.location == loc end
            return state.npc_locations[name] == loc
        end,
        countInLocation = function(loc)
            local n = (state.player.location == loc) and 1 or 0
            for _, l in pairs(state.npc_locations) do
                if l == loc then n = n + 1 end
            end
            return n
        end,
        getAppearance = function(name)
            return state.npc_outfits[name] or "NORMAL"
        end,
        setAppearance = function(name, outfit)
            state.npc_outfits[name] = outfit
        end,
        distance = dist_fn,
    }
end

-- =============================================================================
-- NPC CONFIGS
-- =============================================================================

local MIRA_CONFIG = {
    stats_defaults = { energy = 0.8, mood = 0.7, loneliness = 0.2 },
    stat_bounds    = {
        energy     = { min = 0, max = 1 },
        mood       = { min = 0, max = 1 },
        loneliness = { min = 0, max = 1 },
    },
    idle_activity = "wipes down the counter, humming a quiet tune",

    routine = {
        {   -- early morning: open the tavern
            time = { "06:00", "08:30" }, location = "Tavern",
            activity = "lights the hearth and sweeps the floor before anyone arrives",
            outfit   = "APRON",
            stats    = { energy = -0.05, mood = 0.05 },
        },
        {   -- daytime and evening: serve customers
            time = { "09:00", "22:00" }, location = "Tavern",
            activity = "serves food and drink with practiced efficiency",
            outfit   = "APRON",
            stats    = { energy = -0.1, loneliness = -0.05 },
            variations = {
                {   -- when alone: daydreams
                    prob      = 0.25,
                    condition = { { target = "self", is = "alone" } },
                    activity  = " — then stops and stares at the door, as if expecting someone",
                    stats     = { loneliness = 0.1 },
                    event     = "mira_daydream",
                    info      = { type = "location", target_location = "Tavern" },
                    narrative_hint = "Mira seems lonely — a kind word would go a long way",
                },
            },
        },
        {   -- night: sleep
            time   = { "22:30", "05:59" }, location = "Inn_Room",
            activity = "sleeps in her small room above the tavern",
            outfit   = "SLEEPWEAR",
            stats    = { energy = 0.35 },
        },
    },

    needs = {
        {   -- loneliness drives her to the market
            stat      = "loneliness",
            threshold = 0.75,
            time      = { "10:00", "17:00" },
            options   = {
                {   condition   = { { target = "Market", is = "occupied" } },
                    sequence    = "visit_market",
                    description = "feels the walls closing in and decides to get some air",
                },
            },
        },
    },

    sequences = {
        visit_market = {
            {   location = "Market",
                activity = "browses the market stalls, exchanging small talk with vendors",
                stats    = { loneliness = -0.35, mood = 0.1 },
            },
            {   location = "Tavern",
                activity = "returns to the tavern feeling lighter",
            },
        },
    },

    event_reactions = {
        ["brawl_started"] = {
            activity  = "grabs a heavy oak tankard and shouts 'Not in my tavern!'",
            stats     = { mood = -0.2 },
            narrative_hint = "Mira is furious — she'll throw someone out herself if she has to",
        },
        ["mysterious_arrival"] = {
            activity  = "watches the newcomer with careful, measuring eyes",
            stats     = { loneliness = -0.1 },
            narrative_hint = "Mira is curious about the stranger — she's more likely to talk if approached gently",
        },
    },
}

-- -----------------------------------------------------------------------------

local GARETH_CONFIG = {
    stats_defaults = { energy = 0.9, mood = 0.6, thirst = 0.3 },
    stat_bounds    = {
        energy = { min = 0, max = 1 },
        mood   = { min = 0, max = 1 },
        thirst = { min = 0, max = 1 },
    },
    idle_activity = "runs a whetstone along a blade, eyes distant",

    routine = {
        {   -- all day at the forge
            time     = { "07:00", "17:30" }, location = "Blacksmith",
            activity = "hammers at the forge, shaping a length of red-hot iron",
            outfit   = "LEATHER_APRON",
            stats    = { energy = -0.15, thirst = 0.12 },
        },
        {   -- evening: tavern
            time     = { "18:00", "22:00" }, location = "Tavern",
            activity = "nurses a large ale and talks horses and weather with whoever is near",
            outfit   = "WORK_CLOTHES",
            stats    = { thirst = -0.3, mood = 0.1 },
        },
        {   -- night: sleep
            time     = { "22:30", "06:59" }, location = "Inn_Room",
            activity = "sleeps the deep sleep of honest physical labour",
            outfit   = "SLEEPWEAR",
            stats    = { energy = 0.4 },
        },
    },

    needs = {
        {   -- extreme thirst: emergency tavern run
            stat      = "thirst",
            threshold = 0.85,
            options   = {
                {   condition   = { { target = "Tavern", is = "occupied" } },
                    sequence    = "emergency_drink",
                    description = "drops his hammer mid-swing — his throat is burning",
                },
            },
        },
    },

    sequences = {
        emergency_drink = {
            {   location = "Tavern",
                activity = "bursts through the door and orders the largest ale on the board",
                stats    = { thirst = -0.55, mood = 0.15 },
            },
            {   location = "Blacksmith",
                activity = "wipes his mouth with his sleeve and trudges back to the forge",
            },
        },
    },

    event_reactions = {
        ["brawl_started"] = {
            activity       = "sets down his ale slowly and cracks his knuckles",
            stats          = { mood = 0.1 },
            narrative_hint = "Gareth looks calm, almost pleased — he knows how to handle this",
        },
        ["mira_daydream"] = {
            -- only reaches Gareth if he's in the Tavern (location-type event)
            activity       = "glances at Mira, then back at his cup, saying nothing",
            narrative_hint = "Gareth noticed — there's a long history in that silence",
        },
    },
}

-- =============================================================================
-- NPC INSTANCES
-- =============================================================================

local npcs = {}

local function init_npcs()
    local world = make_world()
    npcs["Mira"]   = NPC.new("Mira",   MIRA_CONFIG,   world)
    npcs["Gareth"] = NPC.new("Gareth", GARETH_CONFIG, world)

    for name, npc in pairs(npcs) do
        -- restore stats
        if state.npc_stats[name] then
            for s, v in pairs(state.npc_stats[name]) do npc.stats[s] = v end
        end
        -- restore engagement
        if state.npc_engaged and state.npc_engaged[name] ~= nil then
            npc:setEngaged(state.npc_engaged[name])
        end
        -- restore memories (or seed defaults on first load)
        if state.npc_memories and state.npc_memories[name] and #state.npc_memories[name] > 0 then
            npc.memory = state.npc_memories[name]
        else
            -- background lore seeded once at game start
            if name == "Mira" then
                npc:pushMemory("grew up in this inn, inherited it from her father ten years ago", 2)
                npc:pushMemory("the merchant who used to visit every tuesday stopped coming last winter", 1)
            elseif name == "Gareth" then
                npc:pushMemory("lost a full wagon of iron stock to bandits on the northern road last spring", 2)
                npc:pushMemory("repaired the town gate hinges for free after the flood — the mayor never thanked him", 1)
            end
        end
    end
end

-- =============================================================================
-- NPC TICK — called each turn
-- =============================================================================

local function run_npc_tick(extra_events)
    local protagonist_loc = state.player.location
    local results, events = NPC.tick(npcs, state.time, state.day, protagonist_loc, dist_fn)

    state.npc_activities    = {}
    state.narrative_context = {}

    for name, r in pairs(results) do
        state.npc_activities[name] = r.activity
        state.npc_locations[name]  = r.location
        if r.narrative_hint then
            table.insert(state.narrative_context,
                         "[" .. name .. "] " .. r.narrative_hint)
        end
    end

    -- dispatch extra events (from player actions)
    if extra_events and #extra_events > 0 then
        local reactions = NPC.dispatch(extra_events, npcs, protagonist_loc, dist_fn)
        for name, r in pairs(reactions) do
            if r.narrative_hint then
                table.insert(state.narrative_context,
                             "[" .. name .. " reacts] " .. r.narrative_hint)
            end
        end
    end

    -- sync stats, engaged, memories back for save/load
    for name, npc in pairs(npcs) do
        state.npc_stats[name] = {}
        for s, v in pairs(npc.stats) do state.npc_stats[name][s] = v end
        if not state.npc_engaged  then state.npc_engaged  = {} end
        if not state.npc_memories then state.npc_memories = {} end
        state.npc_engaged[name]  = npc:isEngaged()
        state.npc_memories[name] = npc.memory
    end
end

-- =============================================================================
-- TIME
-- =============================================================================

local DAYS = { "monday","tuesday","wednesday","thursday","friday","saturday","sunday" }

local function advance_time(minutes)
    minutes = minutes or 30
    local h, m = state.time:match("(%d+):(%d+)")
    h = tonumber(h); m = tonumber(m) + minutes
    if m >= 60 then h = h + math.floor(m / 60); m = m % 60 end
    if h >= 24 then
        h = h % 24
        for i, d in ipairs(DAYS) do
            if d == state.day then state.day = DAYS[(i % 7) + 1]; break end
        end
    end
    state.time = string.format("%02d:%02d", h, m)
end

-- =============================================================================
-- REQUIRED SCRIPT FUNCTIONS
-- =============================================================================

function get_welcome_message()
    return [[
══════════════════════════════════════════════════════
  THE CROSSROADS INN
  A small town at the edge of somewhere dangerous
══════════════════════════════════════════════════════

You arrive at dusk, road-weary, at the Crossroads Inn.
Smoke rises from the chimney. A sign creaks in the wind.
Inside, two locals are going about their evening — they
won't stop for you, but they'll notice if you do something
worth noticing.

What is your name, traveler?
]]
end

function set_initial_state(player_input)
    state = default_state()
    state.player.name = (player_input ~= "" and player_input) or "Stranger"
    init_npcs()
    run_npc_tick()
end

function generate_initial_state()
    set_initial_state("Stranger")
end

function get_status_for_ai()
    -- build list of NPCs sharing the player's location
    local npcs_present = {}
    for name, loc in pairs(state.npc_locations) do
        if loc == state.player.location then
            local mem_ctx = npcs[name] and npcs[name]:getMemoryContext(4) or ""
            npcs_present[name] = {
                activity  = state.npc_activities[name] or "—",
                outfit    = state.npc_outfits[name]   or "NORMAL",
                engaged   = (state.npc_engaged and state.npc_engaged[name]) or false,
                memories  = mem_ctx ~= "" and mem_ctx or nil,
            }
        end
    end

    -- full NPC world for context (LLM can reference off-screen characters)
    local npc_world = {}
    for name, loc in pairs(state.npc_locations) do
        npc_world[name] = {
            location = loc,
            outfit   = state.npc_outfits[name],
            activity = state.npc_activities[name],
            engaged  = (state.npc_engaged and state.npc_engaged[name]) or false,
        }
    end

    return json.encode({
        player       = state.player,
        time         = state.time,
        day          = state.day,
        turn         = state.turn,
        location     = {
            name        = state.player.location,
            description = LOCATION_DESC[state.player.location] or state.player.location,
        },
        npcs_present = npcs_present,
        npc_world    = npc_world,
    })
end

function get_system_prompt()
    local lines = {
        "You are the narrator of a low-magic fantasy adventure set in a small roadside town.",
        "Write in second person, present tense. Be vivid but concise (2-4 sentences).",
        "",
        "RULES:",
        "- Describe only what the player can perceive from their current location.",
        "- NPCs have their own lives. If one is present, weave their activity into the scene.",
        "- The world does not pause for the player.",
        "- new_location must be one of the valid location names, or empty string to stay.",
        "- time_passes is minutes (suggest 20-60 for normal actions, 0 for instant reactions).",
        "- If the player's action could plausibly trigger a world event, set event_name and event_info.",
        "  Event info must include 'type': 'broadcast', 'direct', 'location', or 'area'.",
        "  For 'direct' also set 'target' (NPC name). For 'location' set 'target_location'.",
        "  For 'area' set 'origin_location' and 'radius' (integer).",
        "- Use engage_npc:{\"Name\":true} to freeze an NPC in the current scene (pauses their",
        "  routine and needs). Use false to release them when the scene naturally ends.",
        "  Example: lock Mira during a long conversation; release her when the player walks away.",
        "- Use push_npc_memory to record a significant event an NPC witnessed or experienced.",
        "  Keep each entry short (max 15 words). Only use for memorable or consequential moments.",
        "  These memories will persist and be shown to you in future turns.",
    }

    if #state.narrative_context > 0 then
        table.insert(lines, "")
        table.insert(lines, "NPC CONTEXT (player witnesses this — incorporate into narration):")
        for _, hint in ipairs(state.narrative_context) do
            table.insert(lines, "  " .. hint)
        end
    end

    return table.concat(lines, "\n")
end

function get_json_schema()
    local loc_enum = {}
    for _, l in ipairs(LOCATIONS) do table.insert(loc_enum, '"' .. l .. '"') end
    table.insert(loc_enum, '""')
    return string.format([[{
  "type": "object",
  "required": ["narration", "new_location", "time_passes"],
  "properties": {
    "narration":       { "type": "string" },
    "new_location":    { "type": "string", "enum": [%s] },
    "time_passes":     { "type": "integer", "minimum": 0, "maximum": 120 },
    "event_name":      { "type": "string" },
    "event_info":      { "type": "object" },
    "engage_npc": {
      "type": "object",
      "properties": {
        "Mira":   { "type": "boolean" },
        "Gareth": { "type": "boolean" }
      },
      "description": "true = hold NPC in scene (pause routine), false = release"
    },
    "push_npc_memory": {
      "type": "object",
      "properties": {
        "Mira":   { "type": "string" },
        "Gareth": { "type": "string" }
      },
      "description": "Record a significant event the NPC witnessed (max 15 words each)"
    }
  }
}]], table.concat(loc_enum, ","))
end

function process_ai_response(reply)
    local ok, r = pcall(json.decode, reply)
    if not ok or type(r) ~= "table" then
        return { success = false, error = "Invalid JSON from LLM" }
    end
    if not r.narration or r.narration == "" then
        return { success = false, error = "Missing narration field" }
    end

    -- move player
    if r.new_location and r.new_location ~= "" then
        state.player.location = r.new_location
    end

    -- advance time
    local mins = tonumber(r.time_passes) or 30
    if mins > 0 then advance_time(mins) end

    state.turn = state.turn + 1

    -- engagement control
    if type(r.engage_npc) == "table" then
        for name, flag in pairs(r.engage_npc) do
            if npcs[name] then npcs[name]:setEngaged(flag == true) end
        end
    end

    -- lore memory
    if type(r.push_npc_memory) == "table" then
        for name, text in pairs(r.push_npc_memory) do
            if npcs[name] and type(text) == "string" and text ~= "" then
                npcs[name]:pushMemory(text)
            end
        end
    end

    -- build player-triggered event (if any)
    local extra_events = {}
    if r.event_name and r.event_name ~= "" then
        table.insert(extra_events, {
            name   = r.event_name,
            info   = r.event_info or { type = "broadcast" },
            source = "player",
        })
    end

    run_npc_tick(extra_events)

    return {
        success          = true,
        narration        = r.narration,
        game_over        = r.game_over        or false,
        game_over_reason = r.game_over_reason or "",
    }
end

function process_player_input(input)
    local cmd = input:match("^/(%S+)")

    if cmd == "npcs" then
        local lines = { "NPC STATUS" }
        local ordered = { "Mira", "Gareth" }
        for _, name in ipairs(ordered) do
            local npc = npcs[name]
            if npc then
                local stats_str = {}
                for s, v in pairs(npc.stats) do
                    table.insert(stats_str, string.format("%s=%.2f", s, v))
                end
                local engaged_tag = npc:isEngaged() and " [ENGAGED]" or ""
                table.insert(lines, string.format(
                    "  %-8s  %-14s  [%s]%s  %s",
                    name,
                    state.npc_locations[name] or "?",
                    state.npc_outfits[name]   or "?",
                    engaged_tag,
                    table.concat(stats_str, "  ")))
                table.insert(lines, string.format("           %s",
                    state.npc_activities[name] or "—"))
            end
        end
        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    if cmd == "memory" then
        local parts = {}
        for w in input:gmatch("%S+") do table.insert(parts, w) end
        local target = parts[2]
        local lines  = {}
        local function dump(name)
            local npc = npcs[name]
            if not npc then return end
            local ctx = npc:getMemoryContext(20)
            table.insert(lines, name .. " memories:")
            if ctx ~= "" then
                table.insert(lines, ctx)
            else
                table.insert(lines, "  (none)")
            end
        end
        if target and npcs[target] then
            dump(target)
        else
            dump("Mira")
            table.insert(lines, "")
            dump("Gareth")
        end
        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    if cmd == "time" then
        return { success = true, handled = true,
                 output = string.format("%s, %s  (turn %d)",
                          state.time, state.day, state.turn) }
    end

    -- /event <name> [type] — fire a named event manually (useful for testing)
    if cmd == "event" then
        local parts = {}
        for w in input:gmatch("%S+") do table.insert(parts, w) end
        local ev_name = parts[2]
        if not ev_name then
            return { success = true, handled = true, output = "Usage: /event <name> [broadcast|location|direct]" }
        end
        local ev_type = parts[3] or "broadcast"
        local ev = { { name = ev_name, info = { type = ev_type,
                        target_location = state.player.location,
                        origin_location = state.player.location },
                       source = "player" } }
        local reactions = NPC.dispatch(ev, npcs, state.player.location, dist_fn)
        local lines = { string.format("Event '%s' (%s):", ev_name, ev_type) }
        local any = false
        for name, r in pairs(reactions) do
            table.insert(lines, "  " .. name .. ": " .. (r.activity or "—"))
            any = true
        end
        if not any then table.insert(lines, "  (no NPC reactions)") end
        return { success = true, handled = true, output = table.concat(lines, "\n") }
    end

    return { success = true, handled = false }
end

function get_display_state()
    local present = {}
    for name, loc in pairs(state.npc_locations) do
        if loc == state.player.location then table.insert(present, name) end
    end
    table.sort(present)
    return string.format("[ %s  |  %s %s  |  %s  |  present: %s ]",
        state.player.name, state.time, state.day,
        state.player.location,
        #present > 0 and table.concat(present, ", ") or "—")
end

function get_state_snapshot()
    return json.encode(state)
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then
        return { success = false, error = "Failed to parse snapshot: " .. tostring(data) }
    end
    state = data
    init_npcs()
    return { success = true }
end

function get_commands()
    return {
        { cmd = "/npcs",    desc = "Show NPC status (location, outfit, stats, activity, engaged)", exec = true },
        { cmd = "/memory",  desc = "Show NPC lore memory. Usage: /memory [Mira|Gareth]",           exec = true },
        { cmd = "/time",    desc = "Show current in-game time and day",                            exec = true },
        { cmd = "/event ",  desc = "Fire a named event. Usage: /event <name> [broadcast|location|direct]", exec = false },
    }
end

function get_scene_images()
    -- No image assets defined for this demo.
    return nil
end
