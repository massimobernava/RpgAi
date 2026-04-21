-- =============================================================================
--  fantasy_demo_en.lua  —  The Tomb of the Forgotten King
--  A classic fantasy dungeon crawl demo for RpgAi
--
--  Genre:    Fantasy / Dungeon crawl
--  Language: English
--  Author:   RpgAi project
--
--  Recommended models:
--    anthropic/claude-3.5-sonnet  (OpenRouter)
--    qwen/qwen3-32b               (OpenRouter)
--    llama3.2                     (Ollama, local)
--
--  Launch (web mode):
--    ./build/rpgai --web --provider openrouter --or-key sk-or-... --path scripts/
--
--  Launch (console):
--    ./build/rpgai --provider ollama --model llama3.2 --path scripts/ \
--                  --script fantasy_demo_en.lua
--
--  HOW TO READ THIS FILE:
--    This script is heavily commented to serve as a learning reference.
--    Read it top to bottom alongside the "Writing a Lua Script" section
--    of the README. Every pattern used here is explained there.
-- =============================================================================

local json = require("json")

-- =============================================================================
-- WORLD DATA
-- All static data lives here: locations, NPCs, items.
-- Separating data from logic makes scripts easier to maintain and extend.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- Locations — each has an id (used as key), a display name, a description
-- for the LLM, and a list of connections.
-- ---------------------------------------------------------------------------
local locations = {
    tomb_entrance = {
        name        = "Tomb Entrance",
        description = "A crumbling stone archway half-buried in the hillside. "
                   .. "Crude warnings are carved into the lintel in an ancient script. "
                   .. "A cold draft rises from the darkness below.",
        connections = { "burial_hall", "outside" },
        outside     = true,  -- daylight here, no torch needed
    },
    burial_hall = {
        name        = "Burial Hall",
        description = "A vast rectangular chamber. Stone sarcophagi line the walls, "
                   .. "their lids carved with the faces of long-dead warriors. "
                   .. "Torchlight reveals passages to the north and east. "
                   .. "The air smells of dust and old bone.",
        connections = { "tomb_entrance", "guard_room", "ritual_chamber" },
    },
    guard_room = {
        name        = "Guard Room",
        description = "A smaller chamber that once housed the king's undead sentinels. "
                   .. "Rusted weapons hang on the walls. A heavy iron door leads deeper in. "
                   .. "Scratch marks on the floor suggest something was dragged through here.",
        connections = { "burial_hall", "treasury" },
        door_locked = true,   -- requires the iron key
    },
    ritual_chamber = {
        name        = "Ritual Chamber",
        description = "A circular room dominated by a stone altar stained dark with old offerings. "
                   .. "Strange symbols cover every surface. In the centre, a shallow bowl "
                   .. "holds the dried residue of some ancient ritual.",
        connections = { "burial_hall", "kings_tomb" },
        seal_active = true,   -- the magical seal blocks kings_tomb initially
    },
    kings_tomb = {
        name        = "King's Tomb",
        description = "The innermost sanctum. A massive sarcophagus of black granite "
                   .. "dominates the room, flanked by four stone pillars carved with battle scenes. "
                   .. "Atop the sarcophagus, catching what little light there is, "
                   .. "lies a sword of unmistakable quality.",
        connections = { "ritual_chamber" },
    },
    treasury = {
        name        = "Treasury",
        description = "A small vault stacked with offerings: rotted cloth, tarnished silver, "
                   .. "clay tablets covered in inscriptions. Most of value has already been taken. "
                   .. "A skeleton in the corner clutches something in its bony fingers.",
        connections = { "guard_room" },
    },
    outside = {
        name        = "Outside the Tomb",
        description = "A hillside clearing. Ancient oaks surround the tomb entrance. "
                   .. "The sun is setting — you have until dawn before the tomb seals forever.",
        connections = { "tomb_entrance" },
        outside     = true,
    },
}

-- ---------------------------------------------------------------------------
-- NPCs — skeleton_guard and ghost_scribe are the main characters.
-- Each has a personality string that the LLM uses to shape their dialogue.
-- ---------------------------------------------------------------------------
local NPC = {
    skeleton_guard = {
        name        = "Skeleton Guard",
        location    = "burial_hall",
        personality = "A mindless undead warrior, driven by an ancient compulsion to guard "
                   .. "the tomb. It attacks intruders on sight but can be distracted by offerings "
                   .. "placed at the sarcophagi. It cannot speak but makes grinding bone sounds.",
        hostile     = true,
        defeated    = false,
    },
    ghost_scribe = {
        name        = "Ghost of the Royal Scribe",
        location    = "ritual_chamber",
        personality = "The lingering spirit of the king's scribe, tormented by the fact that "
                   .. "the curse he helped create is still active. He is mournful, helpful to "
                   .. "those who treat him with respect, and speaks in formal archaic English. "
                   .. "He knows how to break the seal on the king's tomb.",
        hostile     = false,
        defeated    = false,
    },
}

-- ---------------------------------------------------------------------------
-- Items — what the player can find and use.
-- ---------------------------------------------------------------------------
local ITEMS = {
    torch        = { name = "Torch",             description = "A lit torch. Keeps the dark away." },
    iron_key     = { name = "Iron Key",          description = "A heavy key, old but functional. Fits the guard room door." },
    silver_coin  = { name = "Silver Coin",       description = "Tarnished but real. Found in the treasury skeleton's hand." },
    ritual_text  = { name = "Ritual Tablet",     description = "A clay tablet with instructions for breaking the tomb seal." },
    kings_sword  = { name = "Sword of the Forgotten King", description = "A legendary blade, perfectly balanced despite its age. This is what you came for." },
}

-- =============================================================================
-- STATE
-- The entire mutable game state lives in this table.
-- It must be fully JSON-serialisable (no functions, no userdata).
-- =============================================================================

local state = {}

local function default_state()
    return {
        player = {
            name        = "Adventurer",
            location    = "tomb_entrance",
            inventory   = { "torch" },
            health      = 10,
            max_health  = 10,
        },
        turn          = 0,
        hour          = 18,   -- 6pm — dawn is at 6am (hour 30, i.e. +12)
        npc_locations = {
            skeleton_guard = "burial_hall",
            ghost_scribe   = "ritual_chamber",
        },
        npc_defeated  = {},
        location_flags = {
            guard_room_locked  = true,
            ritual_seal_active = true,
        },
        game_over     = false,
    }
end

-- =============================================================================
-- HELPERS
-- =============================================================================

local function has_item(item_id)
    for _, v in ipairs(state.player.inventory) do
        if v == item_id then return true end
    end
    return false
end

local function add_item(item_id)
    if not has_item(item_id) then
        table.insert(state.player.inventory, item_id)
    end
end

local function remove_item(item_id)
    for i, v in ipairs(state.player.inventory) do
        if v == item_id then
            table.remove(state.player.inventory, i)
            return true
        end
    end
    return false
end

local function hours_remaining()
    -- Dawn seals the tomb at hour 30 (6am next day)
    return math.max(0, 30 - state.hour)
end

local function inventory_string()
    if #state.player.inventory == 0 then return "nothing" end
    local names = {}
    for _, id in ipairs(state.player.inventory) do
        local item = ITEMS[id]
        table.insert(names, item and item.name or id)
    end
    return table.concat(names, ", ")
end

local function npcs_in_location(loc_id)
    local present = {}
    for npc_id, loc in pairs(state.npc_locations) do
        if loc == loc_id and not state.npc_defeated[npc_id] then
            local npc = NPC[npc_id]
            if npc then
                table.insert(present, {
                    id          = npc_id,
                    name        = npc.name,
                    personality = npc.personality,
                    hostile     = npc.hostile,
                })
            end
        end
    end
    return present
end

-- =============================================================================
-- REQUIRED FUNCTIONS — the engine calls these at specific moments.
-- You must implement all of them.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- get_welcome_message()
-- Called once at startup. Returns the intro text shown before the game begins.
-- This is your chance to set the tone and ask for the player's name.
-- ---------------------------------------------------------------------------
function get_welcome_message()
    -- LANG is set by the engine when --lang <code> is passed on the CLI.
    -- Use it to customise player-facing strings while keeping game logic in English.
    if LANG == "it" then
        return [[
═══════════════════════════════════════════════════
  LA TOMBA DEL RE DIMENTICATO
  Un dungeon crawl per RpgAi
═══════════════════════════════════════════════════

Si narra di un re guerriero sepolto con la sua leggendaria spada —
e della maledizione che colpisce chiunque tenti di impadronirsene.

Hai trovato l'ingresso della tomba al tramonto.
All'alba, la tomba si sigilla per un altro secolo.

Come ti chiami, avventuriero?
(Premi Invio per giocare come "Avventuriero")
]]
    end
    return [[
═══════════════════════════════════════════════════
  THE TOMB OF THE FORGOTTEN KING
  A dungeon crawl adventure for RpgAi
═══════════════════════════════════════════════════

Legends speak of a warrior-king buried with his legendary sword —
and of the curse that strikes down any who try to claim it.

You have found the tomb entrance as the sun begins to set.
By dawn, the tomb seals itself for another century.

What is your name, adventurer?
(Press Enter to play as "Adventurer")
]]
end

-- ---------------------------------------------------------------------------
-- set_initial_state(player_input)
-- Called after the player's first response (their name, or any text).
-- Sets up the initial world state.
-- ---------------------------------------------------------------------------
function set_initial_state(player_input)
    state = default_state()
    if player_input and player_input ~= "" then
        state.player.name = player_input
    end
end

-- ---------------------------------------------------------------------------
-- generate_initial_state()
-- Called if the player pressed Enter with no input.
-- Same as set_initial_state but uses a default name.
-- ---------------------------------------------------------------------------
function generate_initial_state()
    state = default_state()
end

-- ---------------------------------------------------------------------------
-- get_status_for_ai()
-- Called every turn before the LLM. Returns the current world state
-- as a JSON string. Include everything the LLM needs to narrate accurately.
-- ---------------------------------------------------------------------------
function get_status_for_ai()
    local loc_id  = state.player.location
    local loc     = locations[loc_id]
    local present = npcs_in_location(loc_id)

    -- Build a context table — only include what's relevant for this turn
    local ctx = {
        player = {
            name      = state.player.name,
            location  = loc and loc.name or loc_id,
            health    = state.player.health .. "/" .. state.player.max_health,
            inventory = inventory_string(),
        },
        current_location = {
            id          = loc_id,
            name        = loc and loc.name or loc_id,
            description = loc and loc.description or "",
            exits       = loc and loc.connections or {},
        },
        npcs_present = present,
        world = {
            hour           = state.hour,
            hours_to_dawn  = hours_remaining(),
            guard_room_locked   = state.location_flags.guard_room_locked,
            ritual_seal_active  = state.location_flags.ritual_seal_active,
            tomb_sealed         = hours_remaining() == 0,
        },
        turn = state.turn,
    }

    return json.encode(ctx)
end

-- ---------------------------------------------------------------------------
-- get_system_prompt()
-- Called every turn. Returns the system prompt sent to the LLM.
-- This is the most important function for shaping narrative quality.
-- Rebuild it dynamically to inject current context.
-- ---------------------------------------------------------------------------
function get_system_prompt()
    local loc_id = state.player.location
    local loc    = locations[loc_id]
    local hours  = hours_remaining()

    local prompt = string.format([[
You are the dungeon master narrating "The Tomb of the Forgotten King",
a classic fantasy adventure. The player is %s.

WORLD RULES (enforce these strictly):
- The tomb seals at dawn. %d hours remain. Create urgency as dawn approaches.
- The guard room door is %s. It requires the Iron Key to open.
- The ritual seal on the king's tomb is %s. The ghost scribe knows how to break it.
- The sword can only be claimed after the seal is broken.
- The skeleton guard is hostile and will attack unless defeated or distracted.
- The ghost scribe is sorrowful but helpful if treated respectfully.

NARRATIVE STYLE:
- Write in second person ("you see", "you feel"), present tense.
- Keep narration to 3-5 sentences. Atmospheric, slightly ominous.
- Describe sensory details: sounds, smells, the weight of darkness.
- When combat occurs, make it feel dangerous even if the enemy is weak.
- Do not invent new locations, items or NPCs beyond what is defined.

CURRENT LOCATION: %s
%s

Respond ONLY with the JSON object specified in the schema. No preamble.
]],
        state.player.name,
        hours,
        state.location_flags.guard_room_locked and "LOCKED" or "OPEN",
        state.location_flags.ritual_seal_active and "ACTIVE (blocks entry)" or "BROKEN",
        loc and loc.name or loc_id,
        loc and loc.description or ""
    )

    return prompt
end

-- ---------------------------------------------------------------------------
-- get_json_schema()
-- Called every turn. Returns a proper JSON Schema the LLM must follow.
-- Providers that support structured output (OpenAI, OpenRouter, Claude)
-- use this schema for strict enforcement.
-- ---------------------------------------------------------------------------
function get_json_schema()
    return [[{
        "type": "object",
        "required": ["narration", "game_over"],
        "properties": {
            "narration":        { "type": "string",
                                  "description": "3-5 sentences, second person present tense" },
            "new_location":     { "type": "string",
                                  "enum": ["", "tomb_entrance", "burial_hall", "guard_room",
                                           "ritual_chamber", "kings_tomb", "treasury", "outside"],
                                  "description": "location_id if the player moved, empty string otherwise" },
            "avanza_tempo":     { "type": "integer", "minimum": 0, "maximum": 2,
                                  "description": "hours that pass: 0=quick action, 1=exploration, 2=long ritual" },
            "picked_up":        { "type": "array",
                                  "items": { "type": "string",
                                             "enum": ["iron_key", "silver_coin", "ritual_text", "kings_sword"] },
                                  "description": "item ids found this turn, empty array if none" },
            "npc_defeated":     { "type": "string",
                                  "description": "npc_id if an NPC was defeated this turn, empty string otherwise" },
            "seal_broken":      { "type": "boolean",
                                  "description": "true only if the player successfully performed the ritual to break the tomb seal" },
            "game_over":        { "type": "boolean" },
            "game_over_reason": { "type": "string",
                                  "description": "win or loss condition description, empty string if game continues" }
        }
    }]]
end

-- ---------------------------------------------------------------------------
-- process_ai_response(reply)
-- Called after the LLM responds. Validate the JSON, update state,
-- return the narration.
-- ---------------------------------------------------------------------------
function process_ai_response(reply)
    -- Parse JSON — return error if invalid (engine will retry)
    local ok, data = pcall(json.decode, reply)
    if not ok or type(data) ~= "table" then
        return { success=false, error="Invalid JSON: " .. tostring(data) }
    end

    local narration = data.narration
    if not narration or narration == "" then
        return { success=false, error="Missing narration field" }
    end

    -- Update location
    if data.new_location and locations[data.new_location] then
        -- Check movement restrictions
        if data.new_location == "guard_room" and state.location_flags.guard_room_locked then
            -- LLM tried to move through a locked door — override and correct
            narration = narration .. "\n\n[The iron door doesn't budge — it's locked.]"
            data.new_location = nil
        elseif data.new_location == "kings_tomb" and state.location_flags.ritual_seal_active then
            narration = narration .. "\n\n[An invisible force prevents you from entering the tomb.]"
            data.new_location = nil
        else
            state.player.location = data.new_location
        end
    end

    -- Advance time
    if type(data.avanza_tempo) == "number" and data.avanza_tempo > 0 then
        state.hour = state.hour + data.avanza_tempo
        -- Check for dawn (tomb seals)
        if state.hour >= 30 and not state.game_over then
            if not has_item("kings_sword") then
                return {
                    success          = true,
                    narration        = narration .. "\n\nThe first grey light of dawn filters through the tomb entrance. "
                                    .. "A deep rumble shakes the ground as ancient mechanisms engage. "
                                    .. "The tomb seals — you are trapped inside, or forced out empty-handed.\n"
                                    .. "The sword remains with its king.",
                    game_over        = true,
                    game_over_reason = "DEFEAT — Dawn sealed the tomb before you claimed the sword.",
                }
            end
        end
    end

    -- Pick up items
    if type(data.picked_up) == "table" then
        for _, item_id in ipairs(data.picked_up) do
            if ITEMS[item_id] then
                add_item(item_id)
                -- Unlock door if key was picked up
                if item_id == "iron_key" then
                    state.location_flags.guard_room_locked = false
                end
            end
        end
    end

    -- NPC defeated
    if data.npc_defeated and NPC[data.npc_defeated] then
        state.npc_defeated[data.npc_defeated] = true
        state.npc_locations[data.npc_defeated] = nil
    end

    -- Ritual seal broken
    if data.seal_broken == true then
        state.location_flags.ritual_seal_active = false
    end

    -- Win condition: player has the sword and exits
    if has_item("kings_sword") and
       (state.player.location == "outside" or state.player.location == "tomb_entrance") then
        return {
            success          = true,
            narration        = narration .. "\n\nYou step into the cool morning air, the legendary sword "
                            .. "at your hip. Behind you, the tomb rumbles and seals itself once more. "
                            .. "The curse is yours to bear now — but so is the glory.",
            game_over        = true,
            game_over_reason = "VICTORY — You claimed the Sword of the Forgotten King!",
        }
    end

    -- Game over from LLM (e.g. death in combat)
    if data.game_over == true then
        return {
            success          = true,
            narration        = narration,
            game_over        = true,
            game_over_reason = data.game_over_reason or "Your adventure ends here.",
        }
    end

    state.turn = state.turn + 1

    return {
        success          = true,
        narration        = narration,
        game_over        = false,
        game_over_reason = "",
    }
end

-- ---------------------------------------------------------------------------
-- get_display_state()
-- Called every turn. Returns the HUD text shown above the chat.
-- Keep it compact — single line or a few lines.
-- ---------------------------------------------------------------------------
function get_display_state()
    local loc  = locations[state.player.location]
    local name = loc and loc.name or state.player.location
    local hp   = state.player.health .. "/" .. state.player.max_health
    local h    = hours_remaining()
    local dawn = h > 0 and (h .. "h to dawn") or "DAWN — TOMB SEALED"

    return string.format("[ %s ]  HP: %s  Inventory: %s  %s  Turn: %d",
        name, hp, inventory_string(), dawn, state.turn)
end

-- ---------------------------------------------------------------------------
-- get_state_snapshot()
-- Called on save. Must return the full state as a JSON string.
-- ---------------------------------------------------------------------------
function get_state_snapshot()
    return json.encode(state)
end

-- ---------------------------------------------------------------------------
-- restore_state(snapshot)
-- Called on load. Must restore state from the snapshot string.
-- ---------------------------------------------------------------------------
function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then
        return { success=false, error="Failed to parse snapshot: " .. tostring(data) }
    end
    state = data
    return { success=true }
end

-- ---------------------------------------------------------------------------
-- process_player_input(input)
-- Called every turn before the LLM. Handle custom /commands here.
-- Return handled=true to skip the LLM for this turn.
-- Return handled=false to let the turn proceed normally.
-- ---------------------------------------------------------------------------
function process_player_input(input)
    local cmd = input:lower():match("^(/[%w_]+)")

    -- /inventory — show what the player is carrying
    if cmd == "/inventory" or cmd == "/inv" or cmd == "/i" then
        return {
            success = true,
            handled = true,
            output  = "Inventory: " .. inventory_string(),
        }
    end

    -- /map — show available exits from current location
    if cmd == "/map" or cmd == "/exits" then
        local loc = locations[state.player.location]
        local exits = loc and loc.connections or {}
        local lines = { "Exits from " .. (loc and loc.name or state.player.location) .. ":" }
        for _, exit_id in ipairs(exits) do
            local dest = locations[exit_id]
            local note = ""
            if exit_id == "guard_room" and state.location_flags.guard_room_locked then
                note = " [LOCKED]"
            elseif exit_id == "kings_tomb" and state.location_flags.ritual_seal_active then
                note = " [SEALED]"
            end
            table.insert(lines, "  • " .. (dest and dest.name or exit_id) .. note)
        end
        return {
            success = true,
            handled = true,
            output  = table.concat(lines, "\n"),
        }
    end

    -- /time — how long until dawn
    if cmd == "/time" then
        local h = hours_remaining()
        local msg = h > 0
            and string.format("%d hours remain before dawn seals the tomb.", h)
            or  "The tomb is sealed. There is no escape."
        return { success=true, handled=true, output=msg }
    end

    -- Unknown command — pass to LLM
    return { success=true, handled=false }
end

-- =============================================================================
-- OPTIONAL — IMAGE SYSTEM
-- Implement these three functions to enable AI scene illustration.
-- If omitted, /image will report that the script does not support images.
-- =============================================================================

local IMAGE_DIR   = "images/fantasy_demo/"
local IMAGE_STYLE = "dark fantasy illustration, dramatic torchlight, painterly style, "
                 .. "detailed, atmospheric, slightly ominous, 8k quality"

-- Map each asset id to its file path (relative to --path directory)
local ASSET_PATHS = {
    -- Backgrounds
    tomb_entrance    = IMAGE_DIR .. "bg_tomb_entrance.jpg",
    burial_hall      = IMAGE_DIR .. "bg_burial_hall.jpg",
    guard_room       = IMAGE_DIR .. "bg_guard_room.jpg",
    ritual_chamber   = IMAGE_DIR .. "bg_ritual_chamber.jpg",
    kings_tomb       = IMAGE_DIR .. "bg_kings_tomb.jpg",
    treasury         = IMAGE_DIR .. "bg_treasury.jpg",
    outside          = IMAGE_DIR .. "bg_outside.jpg",
    -- NPCs
    skeleton_guard   = IMAGE_DIR .. "npc_skeleton_guard.jpg",
    ghost_scribe     = IMAGE_DIR .. "npc_ghost_scribe.jpg",
}

-- Descriptions used by get_asset_prompt() to generate missing assets
local ASSET_DESCRIPTIONS = {
    tomb_entrance  = "Stone archway entrance to an ancient tomb, carved warning glyphs, "
                  .. "cold draft, overgrown hillside, sunset light, wide establishing shot",
    burial_hall    = "Vast underground chamber, rows of stone sarcophagi with warrior faces, "
                  .. "flickering torchlight, dark passages leading north and east, "
                  .. "dust motes in the air, wide shot",
    guard_room     = "Small underground chamber, rusted weapons on walls, heavy iron door, "
                  .. "scratch marks on stone floor, single torch, oppressive atmosphere",
    ritual_chamber = "Circular underground room, stone altar with dark stains, "
                  .. "ancient symbols covering walls and ceiling, shallow bowl in centre, "
                  .. "eerie greenish light, wide shot",
    kings_tomb     = "Innermost tomb chamber, massive black granite sarcophagus, "
                  .. "four carved stone pillars, legendary sword on top of sarcophagus, "
                  .. "dusty shafts of light, grand and ominous",
    treasury       = "Small vault filled with rotted cloth and tarnished offerings, "
                  .. "skeleton in corner clutching something, dim torchlight, close shot",
    outside        = "Forest clearing at dusk, ancient stone tomb entrance in hillside, "
                  .. "old oak trees, last light of day, atmospheric, wide shot",
    skeleton_guard = "Undead skeleton warrior in ancient armour, rusty sword, "
                  .. "glowing eye sockets, menacing stance, "
                  .. "half-body portrait, dark background, front-facing",
    ghost_scribe   = "Translucent ghost of an elderly scribe, worn robes, mournful expression, "
                  .. "holding a ghostly quill, softly glowing, "
                  .. "half-body portrait, dark background, front-facing",
}

-- get_scene_images() — what assets make up the current scene
-- Returns Format B: { assets={...}, base_image=... }
-- base_image is set to "last" for minor actions (no composition change)
-- base_image is nil for movements (composition changes, rebuild from scratch)
function get_scene_images()
    local loc_id  = state.player.location
    local assets  = {}

    -- Background first
    if ASSET_PATHS[loc_id] then
        table.insert(assets, { id=loc_id, path=ASSET_PATHS[loc_id] })
    end

    -- NPCs present
    for npc_id, npc_loc in pairs(state.npc_locations or {}) do
        if npc_loc == loc_id and not state.npc_defeated[npc_id] then
            if ASSET_PATHS[npc_id] then
                table.insert(assets, { id=npc_id, path=ASSET_PATHS[npc_id] })
            end
        end
    end

    -- base_image hint:
    -- The Lua script sets this based on whether the scene composition changed.
    -- "last" = same location, same NPCs → refine the cached image
    -- nil   = composition changed (new location or NPC change) → rebuild
    --
    -- Here we use a simple heuristic: store the last rendered composition
    -- and compare. In practice, process_ai_response() can set a flag on state
    -- to signal composition changes more precisely.
    local hint = nil
    if state._last_image_loc == loc_id then
        -- Same location as last /image — suggest reusing cached scene as base
        hint = "last"
    end
    -- Update for next call
    state._last_image_loc = loc_id

    return { assets=assets, base_image=hint }
end

-- get_asset_path(id) — return file path for a given asset id
function get_asset_path(id)
    return ASSET_PATHS[id]
end

-- get_asset_prompt(id) — return {path, prompt} for generating a missing asset
function get_asset_prompt(id)
    local path = get_asset_path(id)
    if not path then return nil end

    local desc = ASSET_DESCRIPTIONS[id]
    if not desc then return { path=path, prompt=IMAGE_STYLE } end

    -- Ask the LLM to produce a proper txt2img prompt from the raw description
    local sys = "You are a professional image generation prompt engineer. "
             .. "Convert the description into a detailed txt2img prompt. "
             .. "Include: subject, composition, lighting, style, quality tags. "
             .. "Max 80 words. English only. Output the prompt text only, no explanation."

    local ok, prompt = pcall(query_llm, sys, "[]",
        "Description: " .. desc .. "\nStyle to incorporate: " .. IMAGE_STYLE, "")

    return {
        path   = path,
        prompt = ok and prompt or (desc .. ", " .. IMAGE_STYLE),
    }
end

-- =============================================================================
-- END OF SCRIPT
-- =============================================================================
