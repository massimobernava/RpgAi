# Mandatory Patterns (MODE B/C)

These patterns are NON-NEGOTIABLE. Deviating creates bugs requiring full rewrites.

> **With quickstart (the default path for new adventures) most of this is
> automatic**: `quick.define` already implements before_ai_turn resets, agent
> init ordering, get_tools timing, save/restore with agents, display_state,
> notes scopes, /map. You still apply: the ToolDef hard rules (for
> `spec.tools`), the tool caps, the schema rule (no new_location/time_passes),
> and everything from "Adventure Events" down when on the template path.

## Architecture modes

- **MODE A**: schema-only, no tools. Simple linear adventures.
- **MODE B**: tools-only. LLM uses tools to act; schema only has `narration`.
- **MODE C**: mixed. Tools for actions, schema for narration + narrative flags.

## ToolDef hard rules (tools.build enforces with error() at startup)

- `params` MUST be a JSON Schema **string** (`[[{...}]]`), never a Lua table.
- `fn` receives `args_json` as a **string** — always `json.decode` it first.
- Tool names must be unique across the whole list (watch out: world and
  persona both export a tool named `generate_npc` — register only one).
- See lib_tools.md for the full ToolDef schema.

## Tool rules

| Tool | Rule |
|------|------|
| `think_as_npc(id, situation)` | ONE generic tool for ALL NPCs. Never per-NPC tools. Cache result in `_tool_calls["think_as_npc_"..id.."_result"]`. |
| `move_player(location)` | Validates against TRAVEL_MAP. MAX 1/turn. |
| `move_npc(id, location)` | MAX 1 per NPC per turn. |
| `advance_time(minutes)` | MAX 1/turn. |
| `set_activity(id, activity)` | Update NPC without moving. MAX 1/NPC/turn. |
| `cambia_inventario(add, remove, money)` | Free-form strings (no enum). Only on explicit player action. |
| `remember(note, scope)` | Scopes: player/public/npc:id. Max 2/turn. |
| `memory_write/read` | Cross-session facts. Write confirmed facts only. |

## Schema (MODE C)

Only: `narration` + narrative flags + `game_over`. **Never** `new_location` or `time_passes` — tools handle those.

## process_ai_response

NEVER handles location or time changes. Tools do that.
Only: validate JSON, update narrative state, return narration string.

## before_ai_turn

ALWAYS:
1. Reset `_tool_calls = {}` (or just call `adv.before_turn()` which does 1+2
   and propagates state.turn for event stamping)
2. Call `agent_lib.reset_all_turns(agents, turn_counter)`
3. Call `NPC.tick(...)` if using npc.lua

## Adventure Events (world-level scheduled events)

Events that affect multiple NPCs, world state, weather, or story flags.
Defined in a **separate file** `*_events.lua` — CoderAI edits that file
without touching the main script.

**Quick pattern:**
```lua
-- my_adventure_events.lua
local M = {}
local deps = {}
function M.bind(d) deps = d end
M.list = {
    {   id="nome", label="Nome leggibile",
        when={from="19:30", to="20:30"}, day=nil, prob=0.85,
        once=false, cooldown=22*60,
        condition=function(s) return true end,
        effect=function(s)
            -- modifica state, leggi deps.NPC_IDS, deps.agents_map, deps.persona
            return "Narrazione iniettata nel prompt."
        end },
}
return M
```

In main script — wire AFTER build_objects (agents ready):
```lua
EV_DEPS.NPC_IDS = NPC_IDS; EV_DEPS.agents_map = agents_map
adv.register_events(ev.list)
```

Inject in `get_system_prompt()`:
```lua
p = p .. adv.prompt_events()  -- one-shot, clears _pending_events
```

**LLM beat in effect:** only for `once=true` story beats (phone call, NPC arrival).
Never for mechanical repeating events (dinner, weather). Use `deps.agents_map[id]:react_live(...)`.

**Generated NPC in event:** call `persona.generate(id, ctx)` + `deps.on_npc_generated(id)` callback
(defined in main script's build_objects — creates npc_object + agent + updates NPC_DATA).
Add id to `DYNAMIC_NPCS` in main script for restore handling.

Full API: see lib_adventure.md §Adventure Events.

## Async LLM jobs (lib/jobs.lua)

Never block the turn for off-screen LLM work. Handlers are registered per
kind at script load (no stale closures after restore); adv.before_turn()
polls automatically.

```lua
local jobs = require("lib/jobs")

-- module level (re-registered on every load):
jobs.on("ambient", function(data, meta, err)
    if not data then return end
    world.apply_ambient_result(data, meta.npc_ids, {
        persona=persona, npc_objects=npc_objects,
        location_id=meta.location_id, time=meta.time, date=meta.date })
end)

-- fire (inside the tick hook):
jobs.submit("ambient", sys, user, { schema=world.AMBIENT_SCHEMA, tier="ambient",
    meta={ npc_ids=g.npc_ids, location_id=g.location_id, time=time_str, date=date } })
-- jobs.poll_all() / jobs.pending() / jobs.clear()
-- Engines without query_llm_async fall back to a synchronous call.
-- Jobs are runtime-only: pending jobs are lost on engine restart.
```

## Off-screen life (tick hook)

If the adventure has time + living NPCs, register the tick hook so the world
simulates during time jumps instead of teleporting:

```lua
adv.set_tick_fn(function(time_str, day_str, gidx, step)
    NPC.tick(npc_objects, time_str, day_str, state.player.location)
    -- ambient NPC↔NPC events, rate-limited per pair+location:
    for _, g in ipairs(world.check_colocation_due(state.npc_locations,
                                                  time_str, gidx, {cooldown_min=180})) do
        -- build prompt with world.ambient_prompt, call LLM (async preferred),
        -- apply with world.apply_ambient_result (life events, relationships,
        -- gossip, stats, event log). See lib_world.md.
    end
end)
```

`advance_time(480)` then runs 16 simulated steps (CFG.tick_minutes=30), not one jump.

## Model tiers (cost goes where output is amortized or visible)

Tiers are ENGINE-LEVEL: configure from CLI (`--gen-model/--gen-provider`,
`--agent-model/--agent-provider`, `--ambient-model/--ambient-provider`) or
the web Settings panel (LLM tab → Model tiers). Settings changes apply live.

The libs fall back to tiers AUTOMATICALLY when the script passes no explicit
model/provider:
- "gen"   → persona.generate/dream/critic, world.ensure_* (strong model:
            one call per entity, output lives forever)
- "agent" → agent:decide / think_as_npc (cheap: frequent, short output)
- "ambient" → use explicitly for NPC↔NPC calls (cheapest: never shown):

```lua
local llm_util = require("lib/llm_util")
local am, ap = llm_util.tier("ambient")
query_llm_async(sys, "[]", user, world.AMBIENT_SCHEMA, am, ap)
```

Explicit opts in `persona.init`/`world.init`/`agent.new` still win over
tiers. Narrator stays on the engine default (--provider/--model). CoderAI is
separate (--coder-provider/--coder-model, also in the Settings panel now).

## World bible (shared truth)

For any adventure with generated entities, establish canonical facts early:

```lua
world.bible_set("palazzo", "3 piani, 2 appartamenti per piano: 101,102,201,202,301,302.")
world.bible_claim("apartment", "203", "marco_203")  -- exclusive by default
world.bible_claim("apartment", "203", "sofia_203", {share=true})  -- family member appended
-- expose to the main LLM: world.as_tool_bible() in get_tools()
-- inject world.format_bible() into the MAIN system prompt
```
Generation prompts (world + persona + ambient) inject the bible automatically.
This is the primary defense against incoherent generation (two NPCs in the
same apartment, contradictory building layout).

## Validate during development

After building or editing LOCATIONS/TRAVEL_MAP/NPC data, run `adv.validate()`
(or type `/validate` in game). It mechanically catches: dangling/one-way
travel edges, unreachable locations, NPCs never placed or at unknown
locations, agents without NPC_DATA, persona routines with 24h gaps.
Check `/debug` for runtime warnings from the libs (lib/log ring buffer).

## get_display_state

ALWAYS shows:
- Line 1: current location name + NPCs present at player location (filter npc_locations by player.location)
- Line 2: navigation hint + inventory/stats summary

## Notes scope system

```lua
state.notes = { {date, content, scope}, ... }
-- scope="player"  → injected into master system prompt
-- scope="public"  → injected into master system prompt
-- scope="npc:id"  → injected ONLY into think_as_npc situation for that NPC
```
Agents NEVER write memory. Main LLM uses `memory_write` tool.

## Save/restore with agents

```lua
function get_state_snapshot()
    local snap = {}; for k,v in pairs(state) do snap[k]=v end
    snap._agents = {}
    for id, ag in pairs(agents) do snap._agents[id] = ag:agent_snapshot() end
    return json.encode(snap)
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then return {success=false, error=tostring(data)} end
    local agent_data = data._agents; data._agents = nil
    -- Session-isolated persona: switch to the saved session folder before reloading.
    -- (Omit this block only if the adventure does NOT use persona.new_session.)
    if data._persona_path then
        persona.use_path(data._persona_path)
        persona.reload_all()
    end
    state = data; init_agents()
    if agent_data then
        for id, s in pairs(agent_data) do
            if agents[id] then agents[id]:agent_restore(s) end
        end
    end
    return {success=true}
end
```

> **NPC bleed across games** — without session isolation, persona files modified
> in game A persist into game B. Use `persona.new_session(template_path)` in
> `set_initial_state()` to fork files into a per-session folder. See
> `lib_persona.md §Session isolation` for full pattern.

## /map command

Always shows current location first, then exits with `[location_id]` labels.

## Agent init ordering

All NPCs get agents in `init_agents()`, called AFTER `state.player.name` is set.
Agent system prompts may reference `state.player.name` because of this ordering.

## get_tools() timing

`get_tools()` is called ONCE at engine startup, BEFORE `set_initial_state()`.
Tool closures must NOT capture NPC data upvalues at definition time.
Use module-level vars updated by `adv.set_npc_data()`.

## Private scripts

Always in `my_scripts/`. Never referenced in commits or README.

---

# CoderAI Workflows

## Editing persona files — CRITICAL rules

**NEVER use `write_file` to rewrite an entire persona file.**
Whole-file rewrites require the model to reconstruct 100% of the structure from
memory, which always introduces errors (wrong field names, invented location IDs,
missing sections). The persona format is long and non-obvious.

**ALWAYS use `str_replace` on targeted sections only:**

```
# To add or replace one routine slot:
str_replace(path, old_slot_block, new_slot_block)

# To add a new need/sequence/event_reaction:
str_replace(path, "    needs = {", "    needs = {\n        { NEW_NEED },")

# To change one stat default:
str_replace(path, "controllo = 0.7", "controllo = 0.85")
```

**Two-tier field names — the #1 source of errors:**

| Purpose | Correct field | WRONG (dream-grown, low-priority) |
|---------|--------------|-----------------------------------|
| Hand-authored behaviour | `needs` | ~~`npc_needs`~~ |
| Hand-authored behaviour | `sequences` | ~~`npc_sequences`~~ |
| Hand-authored reactions | `event_reactions` | ~~`npc_event_reactions`~~ |
| Physical appearance | `appearance` | ~~`body`~~ |
| Character mind | `personality` | ~~`mind`~~ |

`npc_*` fields are ONLY written by the dream system. Never write them manually.

**Location IDs — always use THIS adventure's actual ids, never invent one:**

There is no fixed global prefix scheme — every adventure defines its own
location ids in `LOCATIONS`/`spec.locations`. Before writing a `location_id`
into any routine/sequence/need, `read_file` or grep the adventure script
(or `get_game_state()`'s location list) to confirm the id actually exists.
A routine slot pointing at an invented id silently breaks `/validate`'s
24h-coverage check and NPC.tick placement.

External/off-map locations (an office, "outside", a street the player can't
navigate to) may still be used as a routine `location_id` — NPC.tick places
the NPC there for tracking purposes, the player just can't walk in — but
they still must be REAL ids the adventure recognizes, not made up.

**Persona file section ORDER — non-negotiable:**

```
id / name / age / job / home / workplace
appearance / personality / secret
life_events / known_facts
relationships / family
conditions
stats_defaults
routine = { ... },
needs = { ... },
sequences = { ... },
-- ── EVENT_REACTIONS ──────
event_reactions = { ... },
agent_system / npc_summary
short_term_goals / long_term_goals
state_phrases / dream_count / npc_stats /
npc_sequences / npc_needs / npc_event_reactions / dream_log
```

`routine` always comes BEFORE `event_reactions`. NEVER insert a new `event_reactions = {`
block BEFORE `routine`. There is EXACTLY ONE `event_reactions` in every persona file,
always after `sequences` and before `agent_system`, always marked with the header comment
`-- ── EVENT_REACTIONS`. To add a new reaction, `str_replace` INTO that block.

```lua
-- CORRECT: str_replace INTO the existing event_reactions block
str_replace(path,
  "    event_reactions = {\n",
  "    event_reactions = {\n        new_event = { stats={...}, activity=\"...\" },\n")

-- WRONG: creating a SECOND event_reactions block anywhere in the file
-- This embeds the routine inside the fake key and breaks Lua parsing.
```

**Before editing a persona file:**
1. `read_file(path)` to see the ACTUAL current content
2. Identify the exact section to change (use the field names above)
3. `check_lua_syntax` the replacement block in isolation
4. `str_replace` the specific section only
5. `reload_script(preserve_state=true)` to pick up the change

**`relationships` and `family` exact formats — the #2 source of errors:**

```lua
-- CORRECT:
relationships = {
    antonio  = "La moglie. Lo adora ma...",   -- key = NPC id, value = plain string
    rossana  = "Sua sorella. La trova...",
    player   = "Cognato. Lo vede come...",    -- player key is always "player"
},
family = {
    { name = "Antonio Sciglio",  relation = "marito",  notes = "Sposati da 12 anni." },
    { name = "Rossana Cucinotta",relation = "sorella", notes = "La trova bellissima." },
    { name = "Il marito di Rossana", relation = "cognato", notes = "..." },
},

-- WRONG — Gemini anti-patterns:
relationships = {
    { id = "antonio", sentiment = 0.8, notes = "..." },  -- array with id/sentiment: WRONG
},
family = {
    { id = "antonio", relation = "marito" },              -- id instead of name: WRONG
},
```

**Before editing `family` or `relationships`:**
1. `read_file` one or two existing correct persona files (e.g. federica.lua, gaia.lua)
   to verify the actual format in use.
2. `read_file` the adventure script (or grep it) to know which NPCs actually exist
   and their real family relations (sorella vs cognata, madre vs suocera).
   **Never invent NPC names** not present in the adventure's NPC_DATA or existing
   persona files — invented characters in `family` corrupt the knowledge-fence
   that prevents the agent from hallucinating relatives.

**Never call `persona.init()` inside a persona file.** Persona files are plain
data tables: `return { id="...", name="...", routine={...}, ... }`. The `persona`
module is not available in that scope.

---

## Wiring in-game NPC evolution from the adventure script

Sometimes an NPC should change permanently when the player does something
(a seduction arc, a betrayal, a confession). This is a two-part pattern:

**Part 1 — state flag in the adventure script**

Add the flag to `state` so it survives save/load/undo:

```lua
-- In default_state() or set_initial_state():
state.flags = state.flags or {}
-- e.g. state.flags.federica_sedotta = false

-- In get_state_snapshot() — already included if state is serialized wholesale:
-- json.encode(state) captures state.flags automatically.
```

**Part 2 — detect the trigger and patch the NPC**

In `process_ai_response(reply)` or `after_ai_turn(narration, reply)`,
check the LLM's response for the signal and act:

```lua
function after_ai_turn(narration, reply)
    -- LLM signals the event via a schema field or a tool call result:
    if reply.federica_reacts_to_player and not state.flags.federica_sedotta then
        state.flags.federica_sedotta = true

        -- Patch the persona FILE on disk — persists across save/load/reload:
        persona.patch("federica", {
            short_term_goals = { "Trovare un momento solo con " .. state.player.name },
            conditions       = { "sedotta" },
        })
        -- persona.patch writes atomically (temp+rename).
        -- After any reload/undo, restore_state → persona.reload_all() re-reads
        -- the updated file → the new goals are there.
    end
end
```

**Part 3 — inject the flag into think_as_npc per-turn**

The NPC agent needs to know what phase it's in. Inject it via the `situation`
string in the `think_as_npc` tool — NOT via a persona file rewrite:

```lua
-- In get_tools(), when building the think_as_npc situation:
local function federica_situation(base_situation)
    local phase = ""
    if state.flags.federica_sedotta then
        phase = "\n[FASE B ATTIVA: hai già rotto il ghiaccio con il cognato. Ora sei più diretta, cerchi contatto fisico, la maschera di ghiaccio si incrina.]"
    end
    return base_situation .. phase
end
```

**What persists where**

| Data | Storage | Survives undo? | Survives reload? |
|------|---------|---------------|-----------------|
| `state.flags.*` | save JSON (via snapshot) | Yes (undo restores snapshot) | Yes |
| `persona.patch()` changes | .lua file on disk | NO — undo restores state, not the file | Yes |
| `agent_system` in-memory override | memory only | No | No |

**Undo caveat**: `persona.patch` writes to disk immediately. An undo restores
`state` (including `state.flags.federica_sedotta = false`) but does NOT revert
the `.lua` file. If this matters, either: (a) re-patch on restore when the flag
is false, or (b) accept that persona growth is one-way (like dream growth).
`adv.restore()` calls `persona.prune_future_events(turn)` for life_events —
for goals/conditions there is no automatic revert, by design.

---

## Structural change: save → reload

For any structural code edit while a game is running (add NPC, add location,
add tool, change schema/prompt, modify persona file), use this flow:

```
1. check_lua_syntax(new_code_snippet)        -- validate before touching files
2. str_replace(path, old, new)               -- apply the change
   -- or write_file for a brand-new persona file: scripts/npcs/<id>.lua
3. reload_script(preserve_state=true)
   -- hot-swaps the .lua; restore_state() inside also runs persona.reload_all()
   -- so persona file changes are picked up immediately
4. If result.success == false:
   a. str_replace(path, new, old)            -- revert
   b. reload_script(preserve_state=true)     -- back to working state
```

**Lib files (adventure.lua, persona.lua, world.lua, agent.lua, npc.lua, etc.)
are NOT reloaded** by reload_script — they are cached in package.loaded.
Changes to lib files require engine restart (kill + relaunch).

**What reload_script triggers:**
- Full re-execution of the adventure .lua file
- With preserve_state=true: calls restore_state(snapshot) which in turn calls
  persona.reload_all() → picks up ALL persona file changes from disk

## Using the gen/agent/ambient tier from run_lua

The `get_tier(name)` function is available in the run_lua sandbox:

```lua
-- Use strong gen model for quality NPC or location generation:
local t = get_tier("gen")
local result = query_llm(
    "You are an NPC generator for a 1990s Sicilian RPG.",
    "[]",
    "Generate a detailed persona for: " .. description,
    nil,   -- no schema constraint
    t.model,
    t.provider
)
print(result)
```

Tiers: "gen" (strong, amortized — NPC/world generation), "agent" (cheap —
NPC agents), "ambient" (cheapest — off-screen NPC↔NPC events).

`eval_lua` (runs on live game state, needs PLAYING session) also has get_tier.
Use eval_lua when you need to read live state (e.g. current NPC positions)
combined with a tier LLM call in the same operation.

## Discovering configured providers without hardcoding

```lua
-- In run_lua: discover what's configured without guessing
local gen = get_tier("gen")
print("gen tier: " .. gen.model .. " @ " .. gen.provider)
-- Falls back to engine default if --gen-model not set
```

For the narrator (main game model): query_llm with no model/provider args uses
cfg.activeModel() / cfg.providerName automatically.
