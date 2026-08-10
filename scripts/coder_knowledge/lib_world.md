# world.lua API

Procedural world expansion — generates locations, objects, and NPCs on-demand
via LLM (with structural validation + repair retries via `lib/llm_util.lua`).
In-memory; persisted via save JSON snapshot OR a world `.lua` file.
File: `scripts/lib/world.lua`

```lua
local world = require("lib/world")
```

## Init

```lua
world.init(context, opts)
-- context: string describing the world (injected into LLM generation prompts)
-- opts: optional {model, provider}
-- CLEARS all registries (locations/objects/npcs/situations/events) — safe
-- across script hot-swaps, no cross-adventure contamination.

world.init_file(path, context, opts)
-- File-backed persistence: loads world state from a Lua file (NOT JSON);
-- creates it on first save if missing. The file is the authoritative source.
-- Unreadable/invalid file → warning via lib/log + start fresh (path kept,
-- future saves recreate the file).

world.reload_file()   -- re-load from the file set by init_file (use in restore_state)
world.save_file()     -- force a save (writes are atomic: temp + rename)
```

## Locations

```lua
world.set_location(id, data)          -- register a static location (no save)
world.ensure_location(id, context, opts)
-- Generate via LLM if not exists. Validated + auto-repaired graph:
--   no self-edges/duplicates, bidirectional with existing locations.
-- opts.from = "loc_id" → guarantees a way back to that location
--   (ALWAYS pass the player's current location when generating where they go).
-- Returns table or nil after retries (failure logged via lib/log).

world.get_location(id)                → table | nil
world.format_location(id)             → string for system prompt injection
world.all_location_ids_json()         → JSON fragment of known IDs
world.neighbors(id)                   → array — SYMMETRIC direct neighbors in
                                         the generated graph (adventure.lua
                                         merges this with TRAVEL_MAP for you)
world.pending_locations(extra_known)  → ids referenced in connected_to but not
                                         generated yet (expansion seeds);
                                         pass LOCATIONS as extra_known
world.reachable_from(loc_id, extra)   → DIRECT neighbors only (1 hop, no BFS)
```

Navigation note: if the adventure uses `adventure.lua`, `move_player`, BFS,
`prompt_exits` and `/map` already see generated locations — no extra wiring.

## NPCs (world-tracked)

```lua
world.ensure_npc(id, context)         -- generate NPC data if not exists (validated, saved)
world.get_npc(id)                     → table | nil
world.format_npc(id)                  → string
world.set_npc(id, data)
```
NOTE: for file-backed NPC personas with dream growth use persona.lua instead.
Do NOT register both `world.as_tool_generate_npc()` AND `persona.as_tool_generate()`
— both tools are named `generate_npc`, tools.build rejects the duplicate.

## Objects

```lua
world.ensure_object(id, context)      -- generate object if not exists (validated)
world.get_object(id)                  → table | nil
world.format_object(id)               → string
world.object_patch(id, merge)         -- merge fields into object.data
world.object_append(id, field, entry) -- append to array field in object.data
world.object_action(id, action)       -- state machine: apply action
-- Read-only verbs (esamina/guarda/leggi/look/read/...) work without an
-- actions entry; an object-DEFINED action with the same name takes precedence.
world.set_object(id, data)
```

## Object possession (holder)

```lua
-- ONE canonical field answers "where is this object": obj.holder
-- "loc:<id>" | "npc:<id>" | "player" | "obj:<container id>" | nil (scenery)
world.set_holder(id, holder)      → {ok} | {ok=false, error}  (anti-cycle for obj:)
world.holder_of(id)               → holder | nil
world.objects_held_by(holder)     → sorted id array
world.format_held_by(holder, label) → "label: Name [id], ..." | ""
world.as_tool_move_object()       -- ToolDef "move_object": give/take/drop/store
```
Rule: only objects the main LLM touched via tools get a holder — do NOT
simulate every spoon. For descriptive NPC items use persona `carrying`.

## Systemic actions (keys) & containers

```lua
-- actions can gate on a held object:
actions.sblocca = { from={"bloccata"}, to="chiusa", requires="chiave_cantina" }
world.object_action(id, action, actor)   -- actor: "player" (default) | "npc:<id>"
-- → {ok=false, requires="chiave_cantina", error="serve '...'"} if actor lacks it
-- Generation schema includes 'requires'; validator enforces string type.

-- containers: holder "obj:<id>" puts an object inside another (anti-cycle).
-- esamina/look on a container shows `contains` ONLY when its state is not
-- chiuso/closed/locked/sigillato; format_object adds a "Contiene:" line.
```

## World bible (authoritative shared facts)

```lua
world.bible_set(key, fact)     -- e.g. ("palazzo", "3 piani, 2 appartamenti per piano.")
world.bible_get(key) / world.bible_remove(key) / world.bible_all()
world.format_bible()           → "WORLD FACTS (authoritative...)" block | ""

world.bible_claim(category, key, owner, opts)  → {ok, owner=member list}
-- Assignments, e.g. ("apartment", "203", "marco_203").
-- Default EXCLUSIVE: different owner rejected (catches accidental double
-- assignment). Same/listed member re-claim = ok (idempotent).
-- FAMILIES/FLATMATES: opts={share=true} APPENDS the member —
--   bible_claim("apartment","203","sofia_203",{share=true})
--   → owner becomes "marco_203, sofia_203". Sharing is an explicit choice.
world.bible_release(category, key)
world.claim_owner(category, key)         → owner | nil

world.as_tool_bible(description)   -- ToolDef "world_fact": main LLM records
-- canonical facts during narration ("il palazzo ha 3 piani") that then
-- constrain every future generation.
```

The bible block is injected AUTOMATICALLY into every generation prompt
(world.ensure_*, persona.generate, ambient events). Inject `format_bible()`
into the MAIN system prompt too so the narrator respects it.
Persisted in snapshot() and the world file.

## Events & situations

```lua
world.log_event(location_id, time_str, text, npcs)
-- Events are stamped with the current game turn (see set_turn below).
world.format_recent_events(location_id, n)  → string

world.set_situation(key, text)
world.clear_situation(key)
world.format_situations()             → string for system prompt
```

## NPC↔NPC ambient interactions

```lua
world.check_colocation(npc_location_map, min_npcs)
-- Raw detection: locations with >= min_npcs NPCs. NO rate limiting.

world.check_colocation_due(npc_location_map, time_str, day_index, opts)
-- Rate-limited version — USE THIS in the tick hook. opts:
--   { min_npcs=2, cooldown_min=180 }  (game minutes, per location+npc-set)
-- Returned groups are marked as fired. Cooldown persists in snapshot().

world.ambient_prompt(location_id, npc_ids, time_str, date_str, persona_lib)
-- → sys, user prompts for the ambient LLM call (includes NPC profiles and
--   their known_facts when persona_lib is passed).

world.AMBIENT_SCHEMA
-- Schema for the ambient call. Fields: event_summary, life_event_a/b,
-- mood_shift_a/b, stat_delta, relationship_updates [{from,to,description}],
-- shared_fact {from,to,fact} (gossip).

world.apply_ambient_result(result, npc_ids, env)
-- Applies a decoded AMBIENT_SCHEMA result end-to-end:
--   life events + relationship updates + shared_fact → persona files
--   stat_delta → live npc objects (clamped 0..1)
--   event_summary → location event log
-- env = { persona=persona_lib, npc_objects={id→npc_obj},
--         location_id=..., time=..., date="day N" }
-- Returns counters {life_events, relationships, stats, facts}.
```

Typical tick-hook pattern:
```lua
adv.set_tick_fn(function(time_str, day_str, gidx, step)
    NPC.tick(npc_objects, time_str, day_str, state.player.location)
    for _, g in ipairs(world.check_colocation_due(state.npc_locations,
                                                  time_str, gidx, {cooldown_min=180})) do
        local sys, user = world.ambient_prompt(g.location_id, g.npc_ids,
                                               time_str, "day "..gidx, persona)
        -- async preferred: query_llm_async + poll, then:
        -- world.apply_ambient_result(json.decode(reply), g.npc_ids,
        --     { persona=persona, npc_objects=npc_objects,
        --       location_id=g.location_id, time=time_str, date="day "..gidx })
        -- optional gossip: persona.gossip(g.npc_ids[1], g.npc_ids[2], 0.3, "day "..gidx)
    end
end)
```

## Turn stamping / undo reconciliation

```lua
world.set_turn(n)                 -- auto-wired by adv.before_turn()
world.prune_future_events(turn)   -- drop events with turn > n
-- adv.restore() calls prune automatically after an undo.
```

## Persistence

```lua
world.snapshot()     → string (assign to snap._world in get_state_snapshot)
world.restore(data)  -- call in restore_state with snap._world
-- File-backed worlds: prefer world.reload_file() in restore_state.
-- All file writes are atomic (temp + rename).
```

## as_tool helpers (return ToolDef)

```lua
world.as_tool_generate_location(description)  -- accepts optional "from" arg
world.as_tool_generate_npc(description)       -- name clash with persona, see above
world.as_tool_generate_object(description)
world.as_tool_object_action(description)
world.as_tool_log_event(description)
world.as_tool_object_write(description)
```
