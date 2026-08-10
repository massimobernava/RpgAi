# tools.lua API

Pre-built tool definitions for common RPG actions.
File: `scripts/lib/tools.lua`

```lua
local tools = require("lib/tools")
```

## ToolDef schema — READ THIS FIRST

```lua
{
    name        = "tool_name",           -- string, snake_case, UNIQUE
    description = "What this tool does", -- shown to LLM
    params      = [[{                    -- MUST be a JSON Schema STRING
        "type": "object", "required": ["param1"],
        "properties": {
            "param1": { "type": "string" },
            "param2": { "type": "integer" }
        }
    }]],
    fn          = function(args_json)    -- receives a JSON STRING, not a table
        local a = json.decode(args_json)
        -- execute tool using a.param1, a.param2 ...
        return json.encode({ ok=true })  -- return a string (JSON preferred)
    end,
}
```

**HARD RULES (enforced by tools.build, which raises error() at startup):**
- `params` MUST be a JSON Schema **string**. A Lua table silently becomes `"{}"`
  in the engine (the LLM sees no parameters), so tools.build rejects it.
- `fn` receives `args_json` as a **string** — always `json.decode` it first.
- Tool names MUST be unique. The engine keeps only the last duplicate, so
  tools.build rejects duplicates (e.g. `world.as_tool_generate_npc()` and
  `persona.as_tool_generate()` are BOTH named `generate_npc` — pick one).
- `fn` missing or non-function → rejected.

## tools.build(list)

Assembles and VALIDATES the tool list returned by `get_tools()`.
On any rule violation it raises `error()` with the full list of problems —
the engine logs it at startup as a `get_tools()` error. Fix and reload.

```lua
function get_tools()
    return tools.build({
        tools.remember(state),
        tools.forget(state),
        { name="my_tool", description="...", params=[[{...}]],
          fn=function(args_json) local a=json.decode(args_json) ... end },
    })
end
```

## Pre-built tools

### tools.remember(state)
Stores a note in `state.notes`. Scopes: `"player"`, `"public"`, `"npc:<id>"`.
NPC-scoped notes injected only into that NPC's `think_as_npc` situation.

### tools.forget(state)
Removes a note from `state.notes` by index.

### tools.roll_dice(state)
Rolls dice and returns result. Useful for combat/checks.

### tools.skill_check(state)
Performs a skill check against player stats.

### tools.inventory_check(state)
Checks if player has a specific item.

### tools.buy_item(state, prices) / tools.sell_item(state, prices)
Economy tools with configurable price tables.

### tools.query_state(field_name, description, field_fn)
Read-only tool that exposes a piece of state to the LLM.

## think_as_npc pattern

Prefer `adv.get_tools()` with `CFG.use_agents=true` — it builds this tool for
you with per-turn caching AND same-location validation. If you write it by
hand, follow the engine convention:

```lua
-- One generic tool for ALL NPCs. Never per-NPC tools.
{
    name = "think_as_npc",
    description = "Chiedi a un NPC come reagisce alla situazione.",
    params = [[{
        "type": "object", "required": ["id", "situation"],
        "properties": {
            "id":        { "type": "string" },
            "situation": { "type": "string" }
        }
    }]],
    fn = function(args_json)
        local a = json.decode(args_json)
        local key = "think_as_npc_" .. a.id .. "_result"
        if _tool_calls[key] then return _tool_calls[key] end
        local ag = agents[a.id]
        if not ag then return json.encode({intent="non risponde", speech=""}) end
        local result = ag:decide(a.situation, adv.NPC_THINK_SCHEMA)
        _tool_calls[key] = result
        return result
    end,
}
```

`agent:as_tool(name, desc)` also works (it follows the convention above), but
it creates one tool per NPC — the generic `think_as_npc` stays preferable.
