# agent.lua + npc.lua API

LLM-driven NPC reactions via composition with npc.lua.
Files: `scripts/lib/agent.lua`, `scripts/lib/npc.lua`

```lua
local agent_lib = require("lib/agent")
local NPC       = require("lib/npc")
```

## Turn counter (shared cap across all agents)

```lua
local tc = agent_lib.new_turn_counter(max)
-- max: total LLM calls allowed across all agents per turn (recommended: 2-3)
```

## agent_lib.new(id, config)

```lua
local jenny = agent_lib.new("jenny", {
    system           = "Sei Jenny, 22 anni...",
    model            = "llama3.2",      -- nil = engine default
    provider         = "ollama",        -- nil = engine default
    npc              = npc_jenny,       -- optional: links npc.lua object
    turn_counter     = tc,
    max_per_turn     = 1,               -- default 1
    max_history      = 20,              -- default 20
    memory_enabled   = true,            -- default true
    short_term_goals = {"vuole parlare con Anon"},
    long_term_goals  = {"scoprire il segreto di Debbie"},
})
```

## agent:decide(situation, schema)

Main call. Returns JSON string matching schema.
```lua
local result = jenny:decide(args.situation, adv.NPC_THINK_SCHEMA)
-- adv.NPC_THINK_SCHEMA = {intent, speech}
-- intent → narrate in 3rd person, never quote directly
-- speech → VERBATIM between «»; empty string = silence
```
Idempotent per turn: second call returns cached result. Reset with `reset_all_turns`.

## agent:recall(category)

Read memory for this agent's entity.
```lua
local mood = jenny:recall("mood_oggi")  -- returns string or nil
```

## agent:on_event(event_type, data, protagonist_loc)

Notify agent of a game event (called from `after_ai_turn`).
```lua
jenny:on_event("player_spoke", "Anon ha risposto con gentilezza")
```

## agent:set/add/clear goals

```lua
jenny:set_short_term_goal("vuole andarsene")
jenny:add_short_term_goal("è curiosa del libro")
jenny:clear_short_term_goals()
jenny:set_long_term_goal("trovare il padre")
```

## agent_lib.reset_all_turns(agents_table, turn_counter)

Call at start of every `before_ai_turn`.
```lua
agent_lib.reset_all_turns({jenny, debbie}, tc)
-- agents_table: array or {id=agent} map, both work
```

## Persistence

```lua
jenny:agent_snapshot()      → table (include in get_state_snapshot)
jenny:agent_restore(data)   -- call in restore_state
```

## NPC object (npc.lua)

Code-driven behavior: routines, needs, event reactions.
```lua
local npc_jenny = NPC.new("jenny", jenny_config, world_adapter)
NPC.tick({jenny=npc_jenny, debbie=npc_debbie}, time_str, day_str, player_loc)
```

NPC config fields: `name`, `personality`, `routine` (array of time slots), `needs`, `event_reactions`.

## agent:as_tool(tool_name, description)

Returns a ToolDef exposing this agent as a tool (follows the engine
convention: `params` JSON string, `fn(args_json)` with decode).

```lua
jenny:as_tool("think_as_jenny", "Consulta Jenny su come reagisce.")
```

Works, but creates ONE TOOL PER NPC. For MODE B/C prefer the generic
`think_as_npc` tool from `adv.get_tools()` (single tool, per-turn cache,
same-location validation).

## Error handling in decide()

- LLM call failures are caught internally: `decide()` returns the structured
  fallback (npc state summary, or `"[id: decidi tu]"`) instead of raising.
- The fallback is NOT cached — with `max_per_turn = 2` a failed first call
  can be retried in the same turn.
- When the shared turn counter is exhausted, the fallback is returned too.
  Note: the fallback is plain prose, NOT JSON matching the schema — the main
  LLM treats it as a "decide yourself" signal.
