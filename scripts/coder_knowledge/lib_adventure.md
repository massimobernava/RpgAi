# adventure.lua API

Framework eliminating boilerplate for MODE B/C adventures.
Implements all mandatory patterns.
File: `scripts/lib/adventure.lua`

> New adventures normally do NOT call these APIs directly: `lib/quickstart.lua`
> (read_knowledge `quickstart`) wires them all from one declarative spec.
> This reference matters when writing template.lua-style scripts, custom tools
> (adv.tool_called/mark_tool), or overrides on top of quickstart.

```lua
local adv = require("lib/adventure")
```

## Setup

```lua
adv.set_config(CFG)
-- CFG fields (all optional, default false/nil):
-- { use_agents, use_time, use_inventory, use_notes, use_memory, days,
--   tick_minutes,   -- off-screen simulation step (default 30, see Clock)
--   debug_log }

adv.set_state(state)        -- call in set_initial_state and restore_state
adv.get_state()             → state table

adv.set_npc_data(NPC_DATA, LOCATIONS, TRAVEL_MAP)
-- CRITICAL: call after EVERY rebuild of NPC_DATA (e.g. after persona.reload_all())
-- get_tools() closures read via these module-level vars
```

## Standard wrappers (1-3 lines each in your script)

```lua
function get_tools()
    return adv.get_tools(NPC_DATA, LOCATIONS, TRAVEL_MAP, memory, extra_tools)
end

function before_ai_turn(player_input)
    return adv.before_turn()
    -- resets _tool_calls, resets agent turn counters, and propagates
    -- state.turn to world/persona for event turn-stamping
end

function get_display_state()
    return adv.display_state(NPC_DATA, LOCATIONS, extra_line)
end

function get_state_snapshot()
    return adv.snapshot()      -- includes agent snapshots
end

function restore_state(s)
    local data, res = adv.restore(s, init_agents_fn)
    state = data; return res
    -- restore also prunes "future" persona/world events after an undo
    -- (file-backed data does not rewind with the save)
end

function process_player_input(input)
    return adv.handle_input(input, NPC_DATA, LOCATIONS, TRAVEL_MAP)
        or { success=true, handled=false }
end
```

## TRAVEL_MAP — required format

**Array format only** — each location maps to an ordered list of destination IDs:

```lua
local TRAVEL_MAP = {
    sala        = { "cucina", "corridoio", "giardino" },
    cucina      = { "sala" },
    corridoio   = { "sala", "camera_a", "camera_b" },
    camera_a    = { "corridoio" },
    camera_b    = { "corridoio" },
    giardino    = { "sala", "piscina" },
    piscina     = { "giardino" },
}
```

**WRONG — hash format breaks BFS, navigation, /map, prompt_exits:**
```lua
-- NEVER do this:
sala = { cucina=true, corridoio=true }   -- ← ipairs returns nothing, all navigation breaks
```

`_neighbors()` in adventure.lua uses `ipairs` → only array (integer-keyed) entries are read.
Hash format `{loc=true}` produces empty neighbor lists, making every location unreachable.

## Unified location graph

Navigation (move_player, BFS pathfinding, prompt_exits, /map, suggestions)
runs on the MERGED graph: static TRAVEL_MAP + world.lua generated
`connected_to`. Rooms generated at runtime via `world.ensure_location` are
immediately reachable and visible — no extra wiring needed.

## Clock / off-screen simulation

```lua
adv.set_tick_fn(function(time_str, day_str, giorno_index, minutes_step)
    -- called once per simulated step (CFG.tick_minutes, default 30)
    -- every time game time advances — typical body:
    NPC.tick(npc_objects, time_str, day_str, state.player.location)
    -- + world.check_colocation_due(...) for ambient events (see lib_world.md)
end)

adv.advance_clock(mins)  → ticks executed
-- Public stepped clock — use from script commands (/sleep, /settime forward).
-- The advance_time TOOL uses this internally and returns {ok, time, day, ticks}.
-- Without a tick_fn, time still advances correctly (single arithmetic result).
-- Errors inside tick_fn are logged (lib/log) and never block the clock.
```

## World linter

```lua
adv.validate(NPC_DATA, LOCATIONS, TRAVEL_MAP)  → { errors={}, warnings={} }
adv.format_validation(report)                  → printable string
-- errors:   dangling travel_map edges, unknown sources
-- warnings: one-way passages, self-edges, unreachable locations, NPCs at
--           unknown locations or never placed, agents without NPC_DATA,
--           persona routine gaps (24h coverage via persona.validate_routine)
-- Exposed as the /validate player command. RUN IT after building or editing
-- world data — it catches the most common adventure bug classes mechanically.
```

## Warnings (lib/log.lua)

All libs route warnings through `require("lib/log")` (`wlog.warn(src, msg)`).
`/debug` shows the recent entries — in web mode this is the ONLY place
designers see them (console print is invisible). `log.recent(n)`, `log.count()`.

## Prompt helpers (inject into get_system_prompt)

```lua
adv.prompt_exits(travel_map, locations)
-- Full BFS map from current location grouped by distance (merged graph;
-- depth >= 5 collapsed into the "5+" bucket).
-- ALWAYS inject this — prevents LLM from inventing location names.

adv.prompt_npc_positions(npc_data)
-- All NPC positions with [id:x] labels.

adv.prompt_npc_personalities(npc_data)
-- Personalities of NPCs present in current room only.

adv.prompt_notes()
-- Player/public notes. NPC-scoped notes filtered out here.

adv.prompt_workflow(extra_tools, extra_rules)
-- WORKFLOW block auto-built from CFG. Always include.

adv.prompt_pending_event()
-- Any scheduled pending event text (cleared after read).
```

## Tool cap helpers

```lua
adv.tool_called(key)    → bool | number
adv.mark_tool(key)      -- set to true
adv.count_tool(key)     → number
adv.inc_tool(key)       -- increment counter
-- Use for enforcing MAX 1/turn limits on adventure-specific tools.
-- Safety net: if the adventure forgets adv.before_turn(), caps auto-reset
-- when state.turn changes (so a stale cap never blocks tools forever).
```

## Agent management

```lua
adv.init_agents(npc_agents_table, max_calls_per_turn)
adv.snapshot_agents()               → table
adv.restore_agents(data)
-- If a snapshot contains agent histories but no agents are initialized,
-- restore logs a warning instead of dropping them silently — pass
-- init_agents_fn to adv.restore() or call adv.init_agents() first.
```

## Default state

```lua
adv.default_state(CFG)
-- Returns standard state table with player, notes, time, inventory, etc.
-- based on CFG features enabled.
```

## NPC_THINK_SCHEMA

```lua
adv.NPC_THINK_SCHEMA
-- Pass to agent:decide(). Schema: {intent, speech}
-- intent → 3rd person narration, never quoted
-- speech → VERBATIM in «»; empty = silence
```

## Debug

```lua
adv.set_debug_fn(fn)   -- hook for /debug command output
adv.debug_tools        -- tool-call log (capped at 100 entries; CFG.debug_log)
```

## Adventure Events

World-level events distinct from NPC routine/sequence events. Affect multiple
NPCs, world state, weather, story flags. Declared once at script load; fired
automatically by `before_turn()`. Persist via `state._events_fired`.

### External file pattern (recommended)

Keep events in a separate file so designers/CoderAI can edit them without
touching the main script. Pattern uses a `deps` table passed by reference so
effect closures access NPC lists and agent maps at call time (not load time).

**`my_adventure_events.lua`:**
```lua
local M = {}
local deps = {}

function M.bind(d) deps = d end   -- called from main script after build_objects()

M.list = {
    {   id       = "nome_evento",
        label    = "Nome leggibile",
        when     = { from="19:30", to="20:30" },  -- finestra oraria (nil=qualsiasi)
        day      = nil,       -- nil=ogni giorno | 3 | {2,3,5}
        prob     = 0.85,      -- probabilità 0.0-1.0
        once     = true,      -- false=ripetibile
        cooldown = nil,       -- minuti cooldown se once=false (es. 22*60)
        condition = function(s)    -- guard opzionale; return true per permettere
            return true
        end,
        effect   = function(s)     -- modifica stato; return narrazione string o nil
            for _, id in ipairs(deps.NPC_IDS) do
                s.npc_locations[id]  = "villa_veranda"
                s.npc_activities[id] = "a cena"
            end
            return "Cettina chiama tutti a tavola."
        end,
    },
}

return M
```

**In main adventure script:**
```lua
local ev = require("my_adventure_events")

-- EV_DEPS è la tabella condivisa per riferimento — aggiornabile in-place.
local EV_DEPS = { OUTDOOR_LOCS = { giardino=true, piscina=true } }
ev.bind(EV_DEPS)

local function build_objects()
    -- ... build NPC objects, agents_map ...
    -- Aggiorna deps DOPO che agents_map è pronto, poi registra.
    EV_DEPS.NPC_IDS    = NPC_IDS
    EV_DEPS.agents_map = agents_map
    EV_DEPS.persona    = persona
    adv.register_events(ev.list)   -- idempotente, chiama ogni rebuild
end
```

### Event fields

| Campo | Tipo | Default | Descrizione |
|-------|------|---------|-------------|
| `id` | string | — | identificatore unico (obbligatorio) |
| `label` | string | id | nome mostrato in `/events` e nel prompt |
| `when` | `{from,to}` | nil | finestra oraria "HH:MM"-"HH:MM" |
| `day` | number\|table | nil | giorno_index o lista; nil=ogni giorno |
| `prob` | number | 1.0 | probabilità 0.0-1.0 per finestra |
| `once` | bool | true | false=ripetibile con cooldown |
| `cooldown` | number | nil | minuti minimi tra ripetizioni |
| `condition` | fn(state) | nil | guard booleano aggiuntivo |
| `effect` | fn(state) | nil | mutazione stato; ritorna narrazione o nil |

### LLM beat in effect (solo per story-beat once=true)

`react_live` bypassa il turn counter — usare solo per eventi unici,
mai per eventi meccanici quotidiani (cena, meteo):

```lua
effect = function(s)
    s.flags.amica_attesa = true
    local adv = require("lib/adventure")
    if deps.agents_map and deps.agents_map["rossana"] then
        local ok, beat = pcall(function()
            return deps.agents_map["rossana"]:react_live(
                "Riceve telefonata da amica in zona.", adv.NPC_THINK_SCHEMA)
        end)
        if ok and beat and beat.speech ~= "" then
            s.family_log = s.family_log or {}
            table.insert(s.family_log, { time=s.time, npc="rossana",
                                         kind="event", text=beat.speech })
        end
    end
    return "Il telefono di Rossana squilla."
end
```

### Prompt injection

Fired event narrations vengono iniettate nel system prompt tramite
`adv.prompt_events()`. Chiama in `get_system_prompt()`:

```lua
p = p .. adv.prompt_events()   -- one-shot: svuota state._pending_events
```

### Weather pattern

```lua
-- In effect: imposta state.weather
s.weather = { condition="temporale", since_turn=s.turn }

-- In get_system_prompt():
if s.weather then
    local meteo = { temporale="temporale in corso — esterni impraticabili.",
                    sereno="sereno dopo la pioggia." }
    if meteo[s.weather.condition] then
        p = p .. "\n\nMETEO: " .. meteo[s.weather.condition]
    end
end

-- In get_image_style():
if state.weather and state.weather.condition == "temporale" then
    return base_style .. ", dramatic storm light, dark clouds, rain on windows"
end
```

### state._events_fired

Persiste automaticamente nello snapshot (incluso in `adv.snapshot()`).
Struttura: `{ [event_id] = { turn=N, abs_min=M } }`.
`abs_min` = (giorno_index - 1) × 1440 + minuti_dall'inizio_giornata —
usato per il cooldown dei ripetibili.
`state._pending_events` (narrations del turno corrente) NON persiste.

### Debug

`/events` — mostra tutti gli eventi con stato (✓ fired turn / ○ non ancora),
finestra oraria, giorno, modalità ripetizione.

## Built-in /commands (handled by adv.handle_input)

`/map` (alias `/exits`), `/time`, `/npcs`, `/inv` (alias `/inventario`),
`/notes` (alias `/note`), `/debug`, `/validate`, `/sim [N]`, `/events`

**Debug teleport** (no LLM):
- `/setloc <location_id>` — move player to location
- `/setloc <location_id> <npc_id>` — move NPC to location

`eval_lua` cannot access `state` (local var in adventure script). Use `/setloc`
from the game chat instead — it's handled by `adv.handle_input()` which has
direct access to `_state`.
