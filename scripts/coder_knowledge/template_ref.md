# template.lua Reference — ADVANCED path

> **This is the advanced path.** New adventures start from the declarative
> quickstart by default (read_knowledge `quickstart`, reference
> `scripts/template_min.lua`). Use template.lua only for features quickstart
> does not wire: world.lua procedural locations/objects, npc.lua code-driven
> routines, adventure events files, session-isolated personas.

When on this path: read `scripts/template.lua` fully with
`read_file("scripts/template.lua")` before generating any script.
This file summarizes the key sections. The actual source is authoritative.

## File structure

```
§DECISIONS header      ← checklist to fill before coding (see decisions_guide.md)
require statements     ← json, adventure, persona, agent_lib, memory, tools, world
Static data tables     ← LOCATIONS, TRAVEL_MAP, NPC_DATA (static NPCs), ITEMS
State management       ← state table, default_state(), init_agents()
Script contract        ← all required + optional functions
```

## Required requires

```lua
local json     = require("lib/json")
local adv      = require("lib/adventure")
-- Add as needed:
local persona  = require("lib/persona")
local agent_lib= require("lib/agent")
local memory   = require("lib/memory")
local tools    = require("lib/tools")
local world    = require("lib/world")
```

## State table minimum fields

```lua
local state = {
    player = { name="", location="start_location" },
    npc_locations = {},       -- {npc_id → location_id}
    notes = {},               -- {date, content, scope} entries
    time = "09:00",           -- current time string
    giorno = 1,               -- day number
    inventory = {},           -- {item_id → count}
    -- gen_npc_locations = {} -- if using procedural NPCs
}
```

## init_agents() pattern

```lua
local agents = {}
local turn_counter

local function init_agents()
    turn_counter = agent_lib.new_turn_counter(3)
    agents = {}
    for id, npc_data in pairs(NPC_DATA) do
        if npc_data.has_agent then
            local npc_obj = persona.npc_object(id, world_adapter)
            agents[id] = persona.agent_object(id, {
                npc          = npc_obj,
                turn_counter = turn_counter,
            })
        end
    end
end
```

## set_initial_state pattern

```lua
function set_initial_state(player_input)
    state = adv.default_state(CFG)
    state.player.name = player_input ~= "" and player_input or "Anonimo"
    -- register static NPCs
    for id, cfg_npc in pairs(NPC_STATIC_CONFIGS) do
        persona.register_static(id, cfg_npc)
    end
    -- place NPCs in starting locations
    for id, npc in pairs(NPC_DATA) do
        state.npc_locations[id] = npc.start_location
    end
    adv.set_state(state)
    adv.set_npc_data(NPC_DATA, LOCATIONS, TRAVEL_MAP)
    init_agents()
end
```

## restore_state pattern

```lua
function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then return {success=false, error=tostring(data)} end
    local agent_data = data._agents; data._agents = nil
    state = data
    persona.reload_all()
    adv.set_state(state)
    adv.set_npc_data(NPC_DATA, LOCATIONS, TRAVEL_MAP)
    init_agents()
    if agent_data then
        for id, s in pairs(agent_data) do
            if agents[id] then agents[id]:agent_restore(s) end
        end
    end
    return {success=true}
end
```

## get_system_prompt pattern (MODE C)

```lua
function get_system_prompt()
    local sys = "## RUOLO\n..."  -- identity + setting
    sys = sys .. "\n\n## MAPPA\n"          .. adv.prompt_exits(TRAVEL_MAP, LOCATIONS)
    sys = sys .. "\n\n## NPC PRESENTI\n"   .. adv.prompt_npc_personalities(NPC_DATA)
    sys = sys .. "\n\n## POSIZIONI NPC\n"  .. adv.prompt_npc_positions(NPC_DATA)
    sys = sys .. "\n\n## NOTE\n"           .. adv.prompt_notes()
    sys = sys .. "\n\n"                    .. adv.prompt_workflow(extra_tools, extra_rules)
    return sys
end
```

## get_json_schema (MODE C minimum)

```lua
function get_json_schema()
    return [[{
      "type": "object",
      "required": ["narration", "game_over"],
      "properties": {
        "narration": {"type": "string"},
        "game_over": {"type": "boolean"}
      },
      "additionalProperties": false
    }]]
end
```

## process_ai_response (MODE C)

```lua
function process_ai_response(reply)
    local ok, data = adv.parse_reply(reply)
    if not ok then return {success=false, error=data} end
    -- apply narrative flags from data (e.g. data.reveal_secret)
    return adv.response_ok(data.narration, data.game_over)
end
```
