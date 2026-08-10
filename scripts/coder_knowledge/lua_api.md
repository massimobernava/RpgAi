# C++↔Lua Bridge API

Functions exposed by the engine to all Lua scripts via sol2.

## lib/llm_util.lua — validated LLM calls (USE THIS for structured generation)

When you need the LLM to produce a STRUCTURED Lua table (entity generation,
patches, decisions), do not call query_llm + json.decode by hand. Use:

```lua
local llm_util = require("lib/llm_util")

local data, err = llm_util.validated_call(sys, user, schema, function(d)
    -- validate_fn: return true, or false + a SPECIFIC error message
    if type(d.name) ~= "string" or d.name == "" then
        return false, "field 'name' must be a non-empty string"
    end
    return true
end, { retries = 2, model = nil, provider = nil, history = "[]" })

if not data then --[[ all attempts failed; err = last error ]] end
```

What it does: query_llm → decode (json_repair as second chance) → validate →
on rejection, the error is FED BACK to the LLM ("YOUR PREVIOUS ATTEMPT WAS
REJECTED: ...") and it retries. Helpers for validators: `llm_util.is_str(v,
field)`, `llm_util.is_array(v, field, item_fn)`, `llm_util.decode(raw)`.
world.lua and persona.lua already use this internally.

## get_tier

```lua
local t = get_tier("gen")   -- "gen" | "agent" | "ambient"
-- t.model, t.provider — "" = unset (fall back to main model)
```

Engine-level per-role model defaults, configurable from CLI (`--gen-model`,
`--agent-model`, `--ambient-model` + `--*-provider`) AND the web Settings
panel (LLM tab → Model tiers). Read LIVE — a settings change applies to the
next call without restart. Prefer `llm_util.tier(name)` which returns
`model, provider` with nil for unset.

The libs already use tiers as fallback when no explicit override is passed:
persona/world generation + dream + critic → "gen"; agent.decide → "agent".
For ambient calls use it explicitly:
```lua
local am, ap = llm_util.tier("ambient")
query_llm_async(sys, "[]", user, world.AMBIENT_SCHEMA, am, ap)
```

## query_llm

```lua
local reply = query_llm(sys, history_json, user, schema, model, provider)
```

- `sys` (string) — system prompt
- `history_json` (string) — JSON array of `[{role,content}, ...]`, pass `"[]"` for no history
- `user` (string) — user/current prompt
- `schema` (string|nil) — JSON Schema the LLM must follow; nil = free text
- `model` (string|nil) — override model; nil = engine default
- `provider` (string|nil) — override provider: `"ollama"`, `"openrouter"`, `"gemini"`, `"openai"`, `"claude"`

Returns: string (LLM response, or JSON if schema provided).

**Example — use a different model for narration:**
```lua
local narration = query_llm(sys, "[]", prompt, nil, "mistral-nemo", "ollama")
```

## get_embedding

```lua
local vec = get_embedding(text)
```

Returns: array of floats (requires `--embed-model` flag). Returns empty table if not configured.

## cosine_similarity

```lua
local score = cosine_similarity(vec_a, vec_b)
```

Returns: float [0, 1].

## Script contract functions (called BY the engine FROM Lua)

The engine calls these functions on your script. All are required unless marked optional.

### Initialization
```lua
function get_welcome_message() → string
function set_initial_state(player_input)      -- player typed their name/choice
function generate_initial_state()             -- player pressed Enter empty (fallback)
```

### Per-turn lifecycle
```lua
function get_status_for_ai()     → string (JSON of world state)
function get_system_prompt()     → string
function get_json_schema()       → string (JSON Schema)
function process_ai_response(reply) → {success, narration?, error?}
function process_player_input(input) → {success, handled, output?}
function get_display_state()     → string (HUD text)
```

### Persistence
```lua
function get_state_snapshot()    → string (JSON)
function restore_state(snapshot) → {success, error?}
```

### Optional hooks
```lua
function before_ai_turn(player_input) → nil | {skip_llm=true, narration="..."}
function after_ai_turn(narration, raw_reply)
```

### Optional tools
```lua
function get_tools() → list of ToolDef tables (see lib_tools.md)
```

### Optional images
```lua
function get_scene_images() → Format A or B (see CLAUDE.md)
function get_asset_path(id) → string
function get_asset_prompt(id) → {path, prompt}
function get_image_style() → string
```
