# quickstart.lua — DEFAULT path for new adventures

**When asked to create a new adventure, use this path unless the adventure
needs a feature listed under "When NOT to use quickstart" below.**

One call — `quick.define(spec)` — installs the ENTIRE script contract
(`get_welcome_message`, `set_initial_state`, `get_status_for_ai`,
`get_system_prompt`, `get_json_schema`, `process_ai_response`,
`process_player_input`, `get_display_state`, `get_state_snapshot`,
`restore_state`, `get_tools`, `before_ai_turn`, `after_ai_turn`,
`get_commands`, image functions) with correct wiring. You write DATA plus
adventure-specific mechanics only.

Reference implementation: `scripts/template_min.lua` (read it before writing).
quickstart sits ON TOP of adventure.lua/persona.lua — same tools, same
`/validate`, same persona files, same save format.

## What you do NOT write (quickstart does it)

- NO `set_initial_state` / `restore_state` (persona register → reload →
  NPC_DATA rebuild → `adv.set_state` → `init_agents`, in the right order)
- NO `get_status_for_ai` / `get_system_prompt` assembly / `get_json_schema`
- NO `get_tools` wrapper, NO `before_ai_turn`/`after_ai_turn` wrappers
- NO `_tool_calls` reset, NO agent turn-counter reset
- NO `init_agents` / `rebuild_npc_data` / `adv.set_npc_data` calls
- NO generate_npc auto-position wrapper, NO image path tables

If you find yourself writing any of the above with quickstart, stop — it is
already handled, or belongs in a spec field.

## Minimal complete adventure

```lua
local quick = require("lib/quickstart")

quick.define{
    name    = "mia_avventura",          -- snake_case (persona dir, memory file)
    path    = "./my_scripts/",          -- private scripts always in my_scripts/
    context = "Una pensione a Napoli, estate 1987.",

    locations = {
        hall   = { name="Hall",   desc="Bancone in legno, ventilatore lento." },
        cucina = { name="Cucina", desc="Pentole appese, odore di ragù." },
    },
    travel = { hall={"cucina"}, cucina={"hall"} },   -- ALWAYS array format
    start  = "hall",

    npcs = {
        rosa = { name="Rosa", age=55, job="proprietaria", location="cucina",
                 personality="Burbera, materna, sa tutto di tutti.",
                 agent_system="Sono Rosa, proprietaria della pensione. "
                            .. "Burbera ma materna. Prima persona, MAX 3 frasi.",
                 short_term_goals={"capire chi è il nuovo ospite"} },
    },

    welcome = [[Benvenuto alla Pensione Riva.
Come ti chiami?]],

    prompt = {
        header = "Sei il narratore di «Pensione Riva». Protagonista: {player}.\n"
              .. "Seconda persona, presente, 2-4 frasi.",
        rules  = { "Il protagonista agisce SOLO su input del giocatore.",
                   "Non inventare luoghi oltre a quelli definiti." },
    },
}
```

This is a complete, runnable MODE C adventure with think_as_npc, move_player,
move_npc, advance_time, sleep_until, set_activity, remember, generate_npc,
npc_life_event, HUD, save/load, /map //npcs //debug //validate.

## Spec reference

Required: `name`, `locations`, `start`, `welcome`, `prompt.header`.
Everything else optional.

| Field | Type | Purpose |
|---|---|---|
| `path` | string | base dir, default `"./my_scripts/"` |
| `context` | string | world context for persona/NPC generation |
| `config` | table | CFG overrides. Defaults: `use_time=true`, `use_notes=true`, `use_agents`/`use_persona`=true when npcs given, `use_inventory=false`, `use_memory=false`, `generate_npcs=use_persona`, `use_npc_tick`=true when any npc has a routine (living NPCs — set false for prompt-only agents), `session_npcs`=true with persona (every NEW game forks persona files into `npcs_<name>_sessions/<ts>/` — NPC evolution never bleeds into the next game; saves restore into their own session; set false for one shared evolving dir). `mode="schema"` = MODE A (no tools, for local models). `log_file="saves/x.log"` enables gamelog. |
| `days` / `time` | list / "HH:MM" | day names, starting clock (default 09:00) |
| `locations` | `{id={name,desc,zone?}}` | static world |
| `travel` | `{id={ids}}` | array format ONLY (`{loc=true}` hash breaks BFS) |
| `npcs` | `{id={...}}` | `name` required; `age, job, relationship, personality, agent_system, short/long_term_goals, location, appearance, secret, routine` + any persona.register_static field. Ids MUST be ascii snake_case (no accents — `sasa`, not `sasà`). `"%s"` in agent_system → player name. Persona file on disk becomes authoritative after first run — the ENGINE writes it at first game start; NEVER write persona files by hand. |
| `npcs.<id>.routine` | list | `{ {time_from="HH:MM", time_to="HH:MM", location_id="...", activity="...", stats?, outfit?, day?}, ... }` covering all 24h. When ANY npc has a routine, `use_npc_tick` turns on automatically: NPCs get npc.lua objects + composed agents, move on their own, and time jumps simulate step by step instead of teleporting. GIVE EVERY MAIN NPC A ROUTINE — this is what makes the world feel alive. `location_id` must exist in `locations`. |
| `player` / `player_role` | string | default name / role shown to LLM |
| `character_questions` | list | pre-game questionnaire `{field,prompt,type,options?}`; answers build `state.player.appearance` |
| `arrival` | true \| string | LLM arrival scene as turn 0 (string = custom sys prompt) |
| `prompt.rules` | string \| list | numbered automatically when list |
| `prompt.blocks` | `fn(state)→string` | dynamic adventure blocks each turn |
| `prompt.workflow_extra_tools/rules` | string | extra lines in the WORKFLOW block |
| `schema_extra` | `{field={type,enum?,description?,required?,on_set=fn(v,state)}}` | extra MODE C schema fields. `on_set` fires on non-nil, non-`""` values. Reserved: narration, game_over, game_over_reason. |
| `state_init` | `fn(state)` | add adventure-specific state fields |
| `status_extra` | `fn(state)→table` | merged into get_status_for_ai output |
| `on_response` | `fn(r,state)` | per-turn logic after schema_extra (e.g. multi-turn timers); return a table to override the result |
| `commands` | `{{cmd="/x [arg]",desc,fn(rest,state)→string\|table}}` | custom /commands (framework ones are automatic) |
| `tools` | `{ToolDef,...}` | adventure-specific tools. Same hard rules as always: `params` = JSON Schema STRING, `fn(args_json)` gets a STRING, unique names. Use `adv.tool_called(key)`/`adv.mark_tool(key)` for per-turn caps. |
| `hud` | `fn(state)→string` | extra HUD line |
| `hooks` | `{before_turn=fn(input,state), after_turn=fn(narr,reply,state)}` | extra hooks (framework part is automatic) |
| `tick` | `fn(time,day,gidx,step)` | off-screen simulation hook (adv.set_tick_fn) |
| `debug_fn` | `fn(state)→string` | extra /debug output |
| `images` | `{style, dir?, prompts={id=...}?, paths={id=...}?}` | paths auto-derived `dir/bg_<loc>.jpg`, `dir/npc_<id>.jpg`; `paths` overrides |
| `inventory` / `money` | list / int | initial values (when use_inventory) |
| `max_agent_calls` | int | shared agent LLM budget per turn (default 3) |
| `generate_npc_desc` / `life_event_desc` | string | override tool descriptions |
| `persona_path` | string | override persona dir (default `path.."npcs_<name>/"`) |

Returns a handle: `{ adv, persona, state=fn, npc_data=fn, rebuild_npc_data=fn }`.
Access live state in your helpers via `require("lib/adventure").get_state()`
or the handle.

## Escape hatch

`quick.define()` OVERWRITES the globals it installs. To customize ONE
function, define your own version AFTER the `quick.define()` call — later
definition wins, everything else keeps working. Never define contract
functions BEFORE the define call.

## When NOT to use quickstart (→ template.lua path)

Use `scripts/template.lua` + read_knowledge `template_ref` instead when the
adventure needs:
- **world.lua** procedural locations/objects (generate_location, object_action,
  world bible, possession) — quickstart has no world.lua wiring
- **hand-authored needs/sequences/event_reactions on npc.lua objects** —
  quickstart's `use_npc_tick` covers routines (and needs/sequences fields pass
  through to the persona file), but complex multi-step behaviour wiring,
  off-screen families via agent.tick_and_log, custom world adapters stay on
  the template path
- **Adventure events file** (`*_events.lua` + adv.register_events)
- Custom VN wiring beyond the universal `adv.vn_scene` scaffold
- Session-isolated personas (`persona.new_session`)

Mixed approach is fine: start with quickstart, override single globals after
the define call. But if MOST of the contract needs overriding, use template.lua.

## Errors at load are intentional

`quick.define` validates the spec and FAILS FAST with an actionable message
("spec.start deve essere una location esistente. Disponibili: bar, hall, ...").
Fix the spec field it names — do not wrap define in pcall to silence it.
After the script loads, run `/validate` in game for the world linter.

## CoderAI workflow for a new adventure

1. Ask the §DECISIONS questions (read_knowledge `decisions_guide`)
2. `read_file("scripts/template_min.lua")` — the reference spec
3. Write the spec: data tables first, then mechanics (schema_extra, tools,
   on_response) only if the adventure has real rules
4. `check_lua_syntax(code)` → `write_file("my_scripts/<name>.lua", code)`
5. Load it, then `/validate` and fix reported ids
