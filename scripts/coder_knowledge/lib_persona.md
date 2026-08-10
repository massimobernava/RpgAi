# persona.lua API

File-backed NPC personas. Each NPC = one `.lua` file on disk (authoritative source).
File: `scripts/lib/persona.lua`

```lua
local persona = require("lib/persona")
```

## Init

```lua
persona.init(base_path, context, opts)
-- base_path: directory for NPC files, e.g. "./scripts/npcs_my_adventure/"
-- context: world description injected into generation prompts
-- opts: optional {
--   model, provider,              -- generation tier (see Model tiers in patterns.md)
--   critique = true,              -- semantic review pass on generated NPCs:
--   critique_model, critique_provider,
--                                 -- a reviewer LLM judges variety / secret
--                                 -- tension / distinctive voice against the
--                                 -- world bible and existing cast; a FAIL
--                                 -- re-prompts the generator with the reason.
--                                 -- One extra call per generated entity.
--                                 -- Reviewer outage → pass (never blocks).
--   variety_pool = {"worker","student","pensioner"},
--                                 -- template rotation: generate() without an
--                                 -- explicit template picks the LEAST
--                                 -- represented one (counted via the
--                                 -- 'template' field saved in each file).
--   default_template,             -- fallback when no pool/explicit template
--   max_needs, max_event_reactions,  -- caps on structure EARNED from real
--                                 -- occurrences during play (not dream-
--                                 -- generated anymore): max_needs bounds
--                                 -- total needs (one per stat crossing a
--                                 -- threshold — see check_pending_needs),
--                                 -- max_event_reactions bounds reactions
--                                 -- learned from a REAL event recurring
--                                 -- twice (see NPC's on_unhandled_event
--                                 -- hook / _track_unhandled_event).
--   max_stats,                    -- caps PERSONAL stats per NPC (stage 4 —
--                                 -- see common_stats below for the OTHER kind).
--   max_sequences,                -- still INERT (sequences bounded
--                                 -- indirectly via needs/event_reactions).
--   common_stats = {energy=0.7, mood=0.6, stress=0.3},
--                                 -- (stage 4) stats EVERY NPC gets, always,
--                                 -- backfilled deterministically (no LLM
--                                 -- call) at every point a persona enters
--                                 -- memory — register_static, M.load,
--                                 -- M.generate. Never overwrites an
--                                 -- existing value for the same name.
--                                 -- Override for a genre where energy/mood/
--                                 -- stress doesn't fit. Distinct from
--                                 -- PERSONAL stats (character-specific,
--                                 -- e.g. 'gelosia' for a jealous NPC) which
--                                 -- are still LLM-generated: generation is
--                                 -- NOT assumed reliable — an empty
--                                 -- stats_defaults forces a retry
--                                 -- (_generation_looks_valid), and
--                                 -- M._backfill_personal_stats fires a
--                                 -- one-shot async safety net whenever an
--                                 -- NPC becomes live (M.npc_object) if she
--                                 -- still has zero personal stats (covers
--                                 -- hand-authored configs and old files too).
-- }
```

Generation prompts also auto-inject: the world bible (if world.lua loaded)
and a CAST ALREADY PRESENT histogram (age bands + jobs taken) — data-driven
variety instead of "be varied" exhortation.

## Static NPCs (hand-crafted)

```lua
persona.register_static(id, config)
-- Idempotent: safe to call every set_initial_state() and restore_state().
-- Priority: 1) already in memory → return as-is
--           2) .lua exists on disk → load from disk
--           3) write from config → save to disk
-- CORRUPT FILE (syntax error): backed up as <id>.lua.broken-<timestamp>,
-- then rebuilt from config. Errors logged to lib/log + /tmp/persona_load_error.log.
```

Config fields:
```lua
{
    name              = "Valentina Greco",
    age               = 34,
    personality       = "...",
    secret            = "...",           -- knowledge fence (never revealed unless earned)
    family            = {...},           -- prevents hallucinated relatives
    agent_system      = "Sono Valentina...",
    short_term_goals  = {"..."},
    long_term_goals   = {"..."},
    routine           = {
        {time_from="08:00", time_to="17:00", location_id="ufficio", activity="lavora"},
    },
    conditions        = {},
    relationships     = {},
}
```

## Reload (call in restore_state)

```lua
persona.reload_all()
-- Reloads all NPC .lua files from disk (authoritative source after save/restore)
```

## Routine validation (USE DURING DEVELOPMENT)

```lua
persona.validate_routine(id)  → array of issue strings (empty = OK)
-- Checks: HH:MM format, missing locations, 24h coverage per day
-- (15-min grid; handles midnight wrap and day=[...] weekday slots).
-- adv.validate() / the /validate command runs this for every known persona.
```

## Create objects from file data

```lua
local npc_obj   = persona.npc_object(id, world_adapter)
local agent_obj = persona.agent_object(id, opts)
-- opts: {npc=npc_obj, turn_counter=tc, ...} (same as agent_lib.new config)
-- agent_object() auto-injects: npc_summary, secret, family, conditions,
-- goals, state_phrases, known_facts (with provenance), knowledge-limits block
```

## Referencing the player character in agent_system — {player_name} pattern

Persona files are static — they don't know the player's name at write time.
**Use the placeholder `{player_name}` in `agent_system` text.**
Substitute it in `init_agents()`, which runs AFTER `state.player.name` is set.

```lua
-- In the persona file (cettina.lua etc.):
agent_system = "...Vedo nel {player_name} una pedina fondamentale...",

-- In the adventure's init_agents(), after state.player.name is known:
local function init_agents()
    local pname = state.player.name or "il Protagonista"
    cettina_agent = persona.agent_object("cettina", {
        npc          = npc_cettina,
        turn_counter = tc,
        system       = persona.get("cettina").agent_system:gsub("{player_name}", pname),
    })
    -- repeat for every NPC that references {player_name}
end
```

**Why not just write "Protagonista"?** Acceptable as a fallback, but the LLM
may invent a name instead of waiting for context. `{player_name}` + gsub
guarantees the real name is in the system prompt from turn 0.

The agent also learns the player name naturally from conversation history
(the narrator uses it in every turn), so `{player_name}` in `agent_system`
is for turn-0 context — not the only channel.

## Format for system prompt injection

```lua
persona.format(id)           → identity + conditions + goals
persona.format_routine(id)   → daily schedule
persona.format_history(id, max_entries) → last N life events as bullet list
```

## Generated NPCs (LLM on-demand)

```lua
-- LLM calls generate_npc(id, context) tool → writes npcs/<id>.lua
persona.as_tool_generate(description)   → ToolDef
-- Generation is validated with repair retries (rejection reasons are fed
-- back to the LLM). nil after retries = real failure, logged.
-- NAME CLASH: world.as_tool_generate_npc() is ALSO named generate_npc —
-- register only one of the two.
```

## Life events (LLM writes patch to .lua file)

```lua
-- LLM calls npc_life_event(id, patch) tool → rewrites .lua file immediately
persona.as_tool_life_event(description) → ToolDef
persona.patch(id, patch)                -- direct Lua call: merge patch into NPC file
-- File writes are ATOMIC (temp + rename). Life events are stamped with the
-- current game turn (see Turn stamping below).
```

## Known facts & gossip (knowledge propagation)

```lua
persona.add_known_fact(id, fact, source, date_str)  → bool
-- Capped at 12 (FIFO), dedup by exact text. Injected into the agent prompt
-- with provenance ("sentito da Anna") — grounds what the NPC knows.

persona.known_facts(id)  → array of {fact, source, date}

persona.gossip(from_id, to_id, prob, date_str)  → fact | nil
-- With probability prob (default 0.3) to_id learns one random fact from
-- from_id. Call on colocation (tick hook). Dedup automatic.
-- world.apply_ambient_result() also feeds shared_fact entries here.
```

## Outfit / appearance

```lua
persona.current_outfit(id, time_str, day_str)  → string | nil
-- Resolution: outfit_override (runtime) > active routine slot's outfit.
-- Day-specific slots beat generic slots; midnight wrap handled.
persona.set_outfit_override(id, outfit_or_nil)  -- persists; nil/"" clears
persona.format_appearance(id, time, day)  → "Name indossa: ..." | ""
-- USE IN: image prompts (visual consistency!), narrator system prompt.
-- think_as_npc auto-injects "[Stai indossando: ...]" when state.time exists.
-- Generation schema now produces an outfit per routine slot.
-- npc_life_event tool: outfit_override param ("none" clears).
```

## Carrying (descriptive NPC items)

```lua
-- persona file field `carrying` (max 10, dedup) — characterization, no
-- mechanics. For systemic objects use world.set_holder instead.
persona.patch(id, { carrying_add={"borsa di pelle"}, carrying_remove={...} })
-- npc_life_event tool has carrying_add/carrying_remove params.
-- Injected into persona.format() and the agent system prompt.
```

## Dream growth (call from after_ai_turn at night)

```lua
local result = persona.dream_tick(state.time, state.giorno_index, state.last_dream, npc_objects)
-- Picks one NPC that hasn't dreamed today (01:00-05:00 window), runs LLM.
-- result = nil | {id, result={narrative, aspect, life_event, addition_type, additions}}
-- state.last_dream = { [npc_id] = day_index }  -- track who dreamed.
-- If you pass nil as last_dream, a module-level fallback table still prevents
-- repeat dreams within the session — but persist state.last_dream properly.
--
-- DECAY: when all caps are full, the dream RETIRES the oldest addition
-- (tracked via dream_log) and fills the freed slot — evolution never stalls.
-- Removing a sequence also removes needs referencing it; the live npc object
-- gets the removal mirrored by dream_tick. dream_log entry records
-- replaced="type:name"; result includes .replaced = {type, name}.
--
-- ANTI-DRIFT: the dream prompt anchors npc_summary_update to the ORIGINAL
-- personality (injected as anchor + current summary as context); every 7th
-- dream is a CONSOLIDATION night (summary rebuilt from personality + recent
-- events, ignoring the previous summary).
```

## Session isolation — prevent cross-game NPC bleed

By default, persona files are shared across games. Events from game A persist
into game B. Fix: fork NPC files into a per-session folder at the start of
each new game. Each session works in its own folder; saves store the path.

### Using adventure.lua (MODE B/C — automatic)

`adv.restore()` handles `use_path` + `reload_all` automatically when
`state._persona_path` is present in the snapshot.

**In `set_initial_state()`** — call before `generate_all()`:
```lua
local PERSONA_TEMPLATE = "./my_scripts/npcs_my_adventure/"

function set_initial_state(input)
    -- Fork NPC templates → new session folder (e.g. npcs_my_adventure_sessions/20260622_143052/)
    -- Copies all hand-authored .lua files from the template folder.
    -- Generated NPCs (no file in template) are re-generated fresh by generate_all().
    local session_npc_path = persona.new_session(PERSONA_TEMPLATE)

    state = {
        ...
        _persona_path = session_npc_path,   -- stored in snapshot → used by restore_state
    }
    generate_all()
    ...
end
```

**`restore_state()` — no changes needed.** `adv.restore()` detects `_persona_path`
and calls `persona.use_path(path)` + `persona.reload_all()` automatically, then
your `init_agents_fn` runs `generate_all()` which finds NPCs already in memory.

### Without adventure.lua (MODE A — manual)

```lua
local PERSONA_TEMPLATE = "./my_scripts/npcs_my_adventure/"

function set_initial_state(input)
    local session_npc_path = persona.new_session(PERSONA_TEMPLATE)
    state = { ..., _persona_path = session_npc_path }
    -- register NPCs as usual — they load from the freshly forked session folder
    persona.register_static("anna", ANNA_CONFIG)
    ...
end

function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then return { success=false, error=tostring(data) } end
    -- Switch to the session folder recorded in the save, then reload files
    if data._persona_path then
        persona.use_path(data._persona_path)
        persona.reload_all()
    end
    state = data
    -- re-register NPCs: register_static finds them already in _npcs → no-op
    persona.register_static("anna", ANNA_CONFIG)
    init_agents()
    return { success=true }
end
```

### API

```lua
persona.new_session(template_path)
-- Creates <template_path>_sessions/<YYYYMMDD_HHMMSS>/
-- Copies hand-authored .lua files from template_path into the new folder.
-- Switches _base_path to new folder, clears in-memory NPCs.
-- Returns the new path. Call ONLY from set_initial_state().

persona.use_path(path)
-- Switches _base_path to an existing folder, clears in-memory NPCs.
-- Call ONLY from restore_state() before persona.reload_all().

persona.get_path()  → current _base_path
```

### Session folder layout

```
my_scripts/
  npcs_my_adventure/          ← templates (hand-authored, never modified at runtime)
    rossana.lua
    marco.lua
  npcs_my_adventure_sessions/ ← all game sessions
    20260622_143052/           ← session folder stored in save A
      rossana.lua              ← copy of template, modified by events during game A
      marco.lua
    20260623_091530/           ← session folder stored in save B (clean start)
      rossana.lua              ← fresh copy from template
      marco.lua
```

Generated NPCs (via `generate_npc` tool) are NOT copied from templates —
they don't exist there. `generate_all()` re-creates them in the session folder.

Session folders accumulate on disk. Clean up manually or add a `/cleanup sessions`
command to delete folders older than N days.

## Turn stamping / undo reconciliation

```lua
persona.set_turn(n)                 -- auto-wired by adv.before_turn()
persona.prune_future_events(turn)   -- drops life_events/known_facts with turn > n
-- adv.restore() calls prune automatically after an undo (persona files do
-- not rewind with the save — this removes the "future" entries).
```

## Get NPC data

```lua
persona.get(id)         → table | nil
persona.all()           → {id → table}
persona.known_ids()     → array of strings
```

## CRITICAL: adv.set_npc_data() after reload

After `persona.reload_all()` or after rebuilding NPC_DATA, always call:
```lua
adv.set_npc_data(NPC_DATA, LOCATIONS, TRAVEL_MAP)
```
`get_tools()` is called once at startup — tool closures must read NPC data via module-level vars.

---

## Hand-authored vs dream-grown fields — TWO-TIER SYSTEM

Persona files have TWO tiers of behaviour fields. NEVER confuse them.

| Field | Tier | Who writes | Priority |
|-------|------|-----------|---------|
| `needs` | Hand-authored | Master/CoderAI | HIGH — verbatim to npc.lua, authoritative |
| `sequences` | Hand-authored | Master/CoderAI | HIGH — verbatim to npc.lua, authoritative |
| `event_reactions` | Hand-authored | Master/CoderAI | HIGH |
| `npc_needs` | Dream-grown | Dream system | LOW — merged UNDER hand-authored |
| `npc_sequences` | Dream-grown | Dream system | LOW — merged UNDER hand-authored |
| `npc_event_reactions` | Dream-grown | Dream system | LOW |
| `npc_stats` | Dream-grown | Dream system | LOW |

**Rule:** When writing or editing a persona file by hand, ALWAYS use the
NON-prefixed names (`needs`, `sequences`, `event_reactions`).

The `npc_*` fields must always be present as EMPTY tables (`npc_needs = {}`)
to give the dream system a place to grow into. Never put hand-authored data
in them — it will be treated as dream output and may be overwritten.

```lua
-- CORRECT — hand-authored persona:
needs = {
    { stat="desiderio", threshold=0.8, options={ ... } },
},
sequences = {
    sfogo_solitario = { ... },
},
event_reactions = {
    antonio_barbecue = { stats = { desiderio = 0.12 }, activity = "..." },
},
-- Dream placeholders — leave empty:
npc_needs        = {},
npc_sequences    = {},
npc_event_reactions = {},
npc_stats        = {},
npc_summary      = "",   -- dream will fill this
dream_count      = 0,
dream_log        = {},

-- WRONG — CoderAI anti-pattern:
npc_needs = {            -- ← this is the dream bucket, NOT for hand-authored data
    { stat="tensione_sessuale", ... },
},
```

## Routine slot — full field reference

A routine slot is not just `{time_from,time_to,location_id,activity}` — those
are the required ones, but real persona files (hand-authored AND generated)
also use these OPTIONAL fields. Don't improvise a different shape; these are
the only valid keys:

```lua
routine = {
    {
        time_from = "08:00", time_to = "17:00",   -- required, "HH:MM"
        location_id = "ufficio",                   -- required, must exist in this adventure
        activity    = "lavora",                     -- required, short present-tense description
        outfit      = "tailleur grigio",             -- optional, what she's wearing this slot
        stats       = { energia = -0.1 },            -- optional, per-tick stat delta while in this slot
        day         = {"lun","mar","mer","gio","ven"}, -- optional, weekday-specific (absent = every day)
        location_label = "Ufficio al terzo piano",   -- optional, richer label for narration/image prompts
        narrative_hint  = "Controlla le mail con aria stanca.", -- optional, flavor hint for the narrator
        condition   = { { target="antonio", at="ufficio" } },  -- optional, gates the WHOLE slot (world-adapter conditions)
        variations  = {
            -- fires instead of the base slot when prob/condition match
            { prob = 0.3, activity = "Salta il turno, giornata di malumore" },
            -- stat-gated: fires more often when the stat crosses a threshold
            { prob = 0.1, activity = "Resta a casa, troppo stressata",
              prob_boost_when = { stat="stress", min=0.7, boosted_prob=0.6 } },
        },
    },
},
```

`prob_boost_when` WITHOUT a base `prob` defaults to firing ~always once the
condition is met — always give it a low base `prob` so the stat actually
governs the odds. `variations.condition` uses the same world-adapter shape as
the slot-level `condition`. None of these optional fields are required —
omit what you don't need — but don't invent a DIFFERENT field name for the
same idea (e.g. no `mood_hint`, no `outfit_override` inside a routine slot —
that's a separate runtime mechanism, see `set_outfit_override` above).

## Routine 24h coverage rule

`adv.validate()` / `/validate` checks that routine slots cover all 24h.
Every persona file must have NO gaps. Coverage check: slots sorted by
`time_from`, each `time_to` must equal the next `time_from` (midnight wrap).

Common mistake: forgetting work/school slots. Use a "giorno libero" variation
on the work slot instead of omitting it:

```lua
{ time_from="09:30", time_to="13:00",
  location_id = "bar_centrale",
  activity    = "Turno al bar",
  variations  = {
    -- free day: stays home instead
    { prob=0.35, activity=" — giorno libero, gira per la Villa" },
  } },
```

## Variation cooldown (rate-limiting LLM beats)

By default a variation can fire every tick it passes the prob/condition check.
For LLM-flagged beats, add `cooldown_turns` to prevent re-firing:

```lua
variations = {
  -- fires at most once every 3 ticks after it fires
  { prob=0.3, llm=true, cooldown_turns=3,
    situation="..." },

  -- one-shot: fires once, then permanently blocked (cooldown_turns <= 0)
  { prob=0.4, llm=true, cooldown_turns=0,
    situation="Scene che non deve mai ripetersi." },
}
```

Rules:
- `cooldown_turns > 0` → blocked for N ticks after firing, then re-eligible
- `cooldown_turns <= 0` → one-shot (fires once, blocked forever)
- Cooldown state persisted in `NPC:snapshot()` / `NPC:restore()` → survives save/load/undo
- Only applies to routine **variations** (not sequence steps; those are rate-limited by needs stats)
- Typical values: social/manipulation beats → 2-3; physical contact → 4-5; dramatic events → 0 (one-shot)

**Sequence steps** with `llm=true` (inside `sequences = {}`) are NOT covered by
this mechanism — their repetition is controlled by the need threshold + stat decay.

## needs format (full)

```lua
needs = {
    { stat      = "nome_stat",
      threshold = 0.75,            -- triggers when stat >= threshold
      time      = {"08:00","23:59"}, -- optional: only active in this range
      options   = {
        -- conditions checked in order, first match wins:
        { condition   = { { target="npc_id", at="current" } },
          sequence    = "nome_sequenza",
          description = "breve descrizione" },
        { condition   = { { target="self", at="current", is="alone" } },
          sequence    = "sfogo_solitario",
          description = "sola, si sfoga da sé" },
      } },
},
```

## Phase-based character progression (escalation arcs)

When an NPC has a multi-phase arc driven by stats + player actions, the
system already supports this. **Do NOT invent new mechanisms** (`on_update`,
dynamic agent_system, inline goal writes). Use what exists:

### Phase A — stat-driven behaviour change

Use `needs` with a threshold. When `stat >= threshold`, the need fires a
`sequence`. The sequence beats with `llm=true` carry a `situation` string
that describes the phase-appropriate behaviour. This is already the mechanism.

```lua
-- In federica.lua:
stats_defaults = { desiderio_anale = 0.4, ... },

needs = {
    { stat="desiderio_anale", threshold=0.7, time={"08:00","23:00"},
      options = {
        { condition = { {target="player", at="current"} },
          sequence  = "esposizione_fase_a",
          description = "desiderio alto: apparecchia la situazione" },
      } },
},

sequences = {
    esposizione_fase_a = {
        { activity = "si china verso l'armadio con la porta socchiusa" },
        { activity = "passandogli accanto indugia un secondo di troppo",
          stats    = { desiderio_anale = -0.15 },
          llm      = true,
          situation = "Il tuo desiderio è alto e insopportabile. Non cerchi il sesso diretto: 'apparecchi' la situazione. Sei sarcastica ma fisica, ti esponi senza ammetterlo." },
    },
},
```

### Phase B — discrete flag triggered by player action

The flag lives in `state` in the adventure script, NOT in the persona file.
The `think_as_npc` situation string injects the current phase per-turn.
When the flag flips, call `persona.patch` or the `npc_life_event` tool to
update `short_term_goals` permanently in the .lua file.

```lua
-- In the adventure script (process_ai_response or after_ai_turn):
if reply.federica_complicity then
    state.federica_sedotta = true
    persona.patch("federica", {
        short_term_goals = {"Farsi aiutare da " .. state.player.name .. " a calmare la tensione"},
        conditions       = {"sedotta"},
    })
end

-- In get_tools() / think_as_npc situation builder:
local phase = ""
if state.federica_sedotta then
    phase = "\n[FASE B ATTIVA: Federica ha rotto il ghiaccio. È più diretta, cerca contatto fisico.]"
end
-- pass phase into the situation string
```

### Phase C — permanent goal rewrite via npc_life_event

Once Phase B is confirmed, the main LLM (or after_ai_turn) calls the
`npc_life_event` tool to permanently update the persona file:

```lua
-- Via tool (main LLM or CoderAI via run_lua):
persona.patch("federica", {
    long_term_goals = {"Farsi possedere da {player_name}, disposta a rischiare lo scandalo"},
    conditions      = {"sedotta", "ossessiva"},
})
```

### What NOT to do

```lua
-- WRONG: on_update doesn't exist in persona files
on_update = function(state) ... end,

-- WRONG: agent_system is static — can't change it at runtime from within the file
agent_system = function() return phase_a_or_b() end,

-- WRONG: writing goals from game logic inline without persona.patch
federica.long_term_goals = {"nuovo obiettivo"}  -- this modifies the in-memory
                                                  -- table but never saves to disk
                                                  -- and is lost on reload/undo
```

**Summary:** stat threshold → `needs`/`sequences`. Discrete flag → `state` +
situation injection + `persona.patch` on transition. Permanent rewrite →
`npc_life_event` tool or `persona.patch`. Never add callbacks to persona files.

---

## sequences format (full)

```lua
sequences = {
    nome_sequenza = {
        -- Each step: location_id?, outfit?, activity, stats?, condition?, llm?, situation?, event?, info?
        { activity = "primo passo fisico" },
        { activity = "passo clou",
          stats    = { stat_a = -0.3, stat_b = 0.1 },
          llm      = true,          -- this beat gets LLM narration (only on INTERACTION beats)
          situation = "Prompt per l'agente LLM che descrive cosa sta succedendo.",
          event     = "nome_evento", -- fires this event to co-located NPCs
          info      = { type="location", target_location="loc_id" } },
        { activity = "passo finale, ricomposizione",
          stats    = { superiorita = 0.05 } },
    },
}
```

Solo i beat di INTERAZIONE (con un'altra persona presente) vanno marcati `llm=true`.
I beat SOLITARI restano CODED in Fase 1 — l'effetto stat è reale, la prosa è spreco
senza un osservatore.
