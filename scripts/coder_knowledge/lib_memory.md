# memory.lua API

Cross-session persistent storage for NPC facts and player notes.
File: `scripts/lib/memory.lua`

```lua
local memory = require("lib/memory")
```

## Init

```lua
memory.init(script_name, base_path)
-- Loads/creates <base_path><script_name>_memory.json
-- Call once at startup (set_initial_state or module top-level)
-- Example: memory.init("my_adventure", "./scripts/")
```

## Write / Read

```lua
memory.write(entity, category, content)
-- entity: string key (e.g. "jenny", "player")
-- category: string key (e.g. "mood", "secret", "relationship_player")
-- content: string value
-- Persists to disk immediately.

memory.read(entity, category)     → string | nil
memory.read_all(entity)           → table {category → content}
```

## Forget

```lua
memory.forget(entity, category)   -- remove one entry
memory.forget_entity(entity)      -- remove all entries for entity
```

## Format for system prompt injection

```lua
memory.format_entity(entity)
-- Returns multi-line string ready to inject into system prompt.
-- Example output:
--   [jenny]
--   mood: è arrabbiata con Anon
--   secret: sa dove si nasconde il libro

memory.list_entities()  → array of entity strings
memory.dump()           → full dump as formatted string
```

## Design rules

- **Write authority**: only the main LLM writes (via `memory_write` tool).
- **Agents read only**: agents call `agent:recall(category)` which reads memory.
- **Confirmed facts only**: LLM should write to memory only when a fact is confirmed, not speculated.
