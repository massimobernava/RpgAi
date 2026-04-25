# Writing a Lua Script

A RpgAi adventure is a single `.lua` file (or a main file that `require`s others) that defines your entire game world. The engine calls specific functions at specific moments — you implement them, the engine handles the rest.

---

## Minimal script structure

```lua
local json = require("json")

local state = {}

function get_welcome_message()
    return "You stand at the entrance of the dungeon. What is your name, adventurer?"
end

function set_initial_state(player_input)
    state.player = { name = player_input, location = "dungeon_entrance" }
    state.turn   = 1
end

function generate_initial_state()
    state.player = { name = "Adventurer", location = "dungeon_entrance" }
    state.turn   = 1
end

function get_status_for_ai()
    return json.encode(state)
end

function get_system_prompt()
    return "You are the dungeon master of a classic fantasy RPG. " ..
           "Narrate the outcome of the player's actions. " ..
           "Follow the JSON schema exactly."
end

function get_json_schema()
    return json.encode({
        narration        = "string — what happens, 3-5 sentences",
        new_location     = "string or null — if the player moved",
        game_over        = "boolean",
        game_over_reason = "string or null"
    })
end

function process_ai_response(reply)
    local ok, data = pcall(json.decode, reply)
    if not ok then
        return { success=false, error="Invalid JSON: " .. tostring(data) }
    end
    if data.new_location then
        state.player.location = data.new_location
    end
    state.turn = state.turn + 1
    return {
        success          = true,
        narration        = data.narration,
        game_over        = data.game_over == true,
        game_over_reason = data.game_over_reason or ""
    }
end

function get_display_state()
    return string.format("[ %s ]  Turn: %d", state.player.location, state.turn)
end

function get_state_snapshot()
    return json.encode(state)
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then return { success=false, error="Parse error" } end
    state = data
    return { success=true }
end

function process_player_input(input)
    return { success=true, handled=false }
end
```

---

## Required functions — reference

| Function | When called | Must return |
|---|---|---|
| `get_welcome_message()` | Once, on start | `string` — intro text |
| `set_initial_state(input)` | Once, after player's first response | nothing |
| `generate_initial_state()` | Once, if player pressed Enter with no input | nothing |
| `get_status_for_ai()` | Every turn, before LLM call | `string` — JSON of world state |
| `get_system_prompt()` | Every turn, before LLM call | `string` — system prompt |
| `get_json_schema()` | Every turn, before LLM call | `string` — JSON schema |
| `process_ai_response(reply)` | Every turn, after LLM responds | `table` — see below |
| `get_display_state()` | Every turn, after processing | `string` — HUD text |
| `get_state_snapshot()` | On save | `string` — full state JSON |
| `restore_state(snapshot)` | On load | `table {success, error?}` |
| `process_player_input(input)` | Every turn, before LLM | `table` — see below |

**`process_ai_response` return table:**
```lua
{
    success          = true,
    error            = "...",          -- only if success=false
    narration        = "string",
    game_over        = false,
    game_over_reason = "string or nil"
}
```

**`process_player_input` return table:**
```lua
{
    success = true,
    handled = false,   -- true = LLM is skipped this turn
    output  = "..."    -- optional text shown if handled=true
}
```

---

## Optional functions — image system

Implement these to enable AI scene illustration. See [images.md](images.md) for the full pipeline.

```lua
function get_scene_images()   -- returns asset list for current scene
function get_asset_path(id)   -- returns file path for asset id
function get_asset_prompt(id) -- returns {path, prompt} for text-to-image generation
```

---

## C++ functions exposed to Lua

#### `query_llm(sys, history_json, user, schema) → string`

Call the active LLM directly from Lua. Useful for NPC dialogue, item descriptions, dream sequences — anything that needs AI text outside the main turn loop.

```lua
local reply = query_llm(
    "You are a wise old oracle. Speak in riddles.",
    "[]",
    "The player asks: " .. player_question,
    ""
)
```

`history_json` must be a JSON array string. Use `"[]"` for no history.

#### `get_embedding(text) → table | nil`

Returns a vector of floats (Lua table with numeric keys) for semantic similarity. Returns `nil` if `--embed-model` not configured.

#### `cosine_similarity(vec_a, vec_b) → float`

Cosine similarity between two embedding vectors. Returns 0–1.

---

## Designing the JSON schema

The schema from `get_json_schema()` constrains what the LLM can produce.

**Tips:**
- Add a comment string per field — the LLM reads these
- Keep it small; every unused field wastes tokens
- Use `null`-able fields for things that don't happen every turn
- Add an `avanza_tempo` integer if your world tracks time

```lua
function get_json_schema()
    return json.encode({
        narration        = "string, 3-5 sentences, second person",
        new_location     = "string location_id or null if player did not move",
        avanza_tempo     = "integer minutes 0-60, 0 if no time passed",
        cambia_relazioni = "object {npc_id: {field: delta}} or null",
        game_over        = "boolean",
        game_over_reason = "string or null"
    })
end
```

---

## NPC scheduling pattern

```lua
local NPC_ROUTINES = {
    guard    = { mattina="gate", pomeriggio="barracks", sera="tavern", notte="gate" },
    merchant = { mattina="market", pomeriggio="market", sera="home",   notte="home" },
}

local function advance_npc_routines()
    local ora = state.time_of_day
    for npc_id, routine in pairs(NPC_ROUTINES) do
        if routine[ora] then
            state.npc_locations[npc_id] = routine[ora]
        end
    end
end
```

Call `advance_npc_routines()` inside `process_ai_response()` whenever `avanza_tempo > 0`.

---

## Custom slash commands

```lua
function process_player_input(input)
    local cmd = input:lower():match("^(/[%w_]+)")

    if cmd == "/inventory" then
        local lines = {}
        for _, item in ipairs(state.player.inventory) do
            table.insert(lines, "• " .. item)
        end
        return { success=true, handled=true,
                 output="Inventory:\n" .. table.concat(lines, "\n") }
    end

    return { success=true, handled=false }
end
```

---

## Dynamic system prompt

`get_system_prompt()` is called fresh every turn — use it to inject current state:

```lua
function get_system_prompt()
    local base = BASE_SYSTEM_PROMPT
    if state.time_of_day == "night" then
        base = base .. "\n\nIt is deep night. Describe sounds and shadows more than sights."
    end
    if #get_present_npcs() == 0 then
        base = base .. "\n\nThe player is alone. Emphasise atmosphere and internal thoughts."
    end
    return base
end
```

---

## Best practices

- **Keep state serialisable.** Everything in `state` must be JSON-serialisable (no functions, no userdata).
- **Validate LLM output defensively.** Use `or` fallbacks and type checks before updating state from LLM fields.
- **Use `pcall` around `query_llm` calls.** Network calls can fail.
- **Separate data from logic.** Put world data (locations, NPCs, items) in a `require`d file. Keep the main script focused on game logic.
- **`io.stderr` is unavailable.** `sol::lib::io` is not opened. Use `print()` for debug output inside Lua error handlers.
- **`local function` scoping.** A `local function foo()` is only visible to code defined *after* its line. Reorder definitions accordingly.

---

## Demo script — `fantasy_demo.lua`

*The Tomb of the Forgotten King — classic fantasy dungeon crawl.*

You are a lone adventurer who discovers an ancient tomb. Navigate crumbling corridors, outwit the undead guardians, find the artefacts that break the curse, and claim the legendary blade before the tomb seals itself forever.

**Features demonstrated:**
- Multi-room dungeon with connected locations
- NPC guardians with patrol routines
- Inventory system with key items
- Time pressure mechanic (the tomb seals at dawn)
- Win/lose conditions
- Full image system support

```bash
# Console mode — local Ollama
./build/rpgai \
  --provider ollama --model llama3.2 \
  --path scripts/ --script fantasy_demo.lua

# Web mode — OpenRouter
./build/rpgai --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --or-model anthropic/claude-3.5-sonnet \
  --path scripts/ --save-path saves/
```

### Playing in a different language — `--lang`

```bash
./build/rpgai --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --or-model qwen/qwen3-32b \
  --path scripts/ --save-path saves/ \
  --lang it
```

Language codes and phrases are defined in `lang.txt` (seven languages ship out of the box: `it`, `fr`, `de`, `es`, `pt`, `ja`, `zh`). Scripts can read the `LANG` global to localise welcome messages:

```lua
function get_welcome_message()
    if LANG == "it" then return "Bentornato, avventuriero..." end
    return "Welcome, adventurer..."
end
```

### Using the demo as a template

The script is heavily commented. To start your own adventure:

1. Copy `fantasy_demo.lua` to a new file (e.g. `my_world.lua`)
2. Replace location definitions, NPC roster and system prompt
3. Adjust the JSON schema to match your mechanics
4. Run it — the engine handles the rest

### Community scripts

Have a script to share? Open a pull request adding your `.lua` file to `scripts/community/`. Include a short description at the top (title, genre, recommended model, author).

Looking for:
- Urban / noir — city investigations, social intrigue
- Sci-fi — space exploration, cyberpunk, post-apocalyptic
- Experimental — non-linear, puzzle-heavy, comedy
- Non-English adventures
- Literary — public-domain setting adaptations
