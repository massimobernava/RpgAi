# §DECISIONS Guide

Before writing ANY adventure script, ask the user these questions in order.
Do not generate code until all answers are collected.

> **Which path after the answers?** DEFAULT = declarative quickstart
> (read_knowledge `quickstart`, reference `scripts/template_min.lua`).
> Use the full `scripts/template.lua` path ONLY when the answers require
> world.lua procedural locations/objects, npc.lua code-driven routines,
> adventure events files, or session-isolated personas
> (see quickstart.md §"When NOT to use quickstart").

## 1. Genre, tone, content level, language

- Genre: fantasy / contemporary / sci-fi / horror / slice-of-life / other
- Tone: serious / comedic / noir / romantic / thriller
- Content level: all-ages / mature themes / explicit adult
- Language: Italian / English / other (affects system prompt language; do NOT add "rispondi in italiano" — automatic translation is handled by the engine)

## 2. Architecture mode

- **MODE A** — schema only. Simple, linear. No tool calling. Good for: simple demos, single-location scenes.
- **MODE B** — tools only. LLM acts via tools. Schema only has `narration`. Good for: exploration, NPC interaction.
- **MODE C** — mixed. Tools for actions + schema for narration flags. Good for: complex adventures with time, inventory, agents.

Recommend MODE C for anything non-trivial. MODE C is quickstart's default;
MODE A = `config.mode="schema"` in the quickstart spec.

## 3. Features checklist

Ask which features are needed:
- [ ] Time system (advance_time tool, day/night cycle)
- [ ] Inventory (cambia_inventario tool)
- [ ] NPC agents (LLM-driven reactions via agent.lua)
- [ ] **Persistent NPC memory** — two independent systems, ask about each:
  - `memory.lua` (`use_memory=true`): short cross-session facts per entity
    (e.g. "cettina knows player's real name"). LLM writes via `memory_write` tool.
    Inject into system prompt via `memory.format_entity(id)` for NPCs present.
    Rule for system prompt: write only confirmed facts that change future behaviour.
  - `npc_life_event` tool (always available with persona.lua): permanent changes
    to NPC file — new condition, relationship, goal, routine update.
    Rule for system prompt: only for stable/confirmed changes, not speculation.
  If NPCs have agents → enable both; if NPC-light (static data only) → skip.
- [ ] Procedural world (generate locations/objects on-demand via world.lua)
- [ ] Procedural NPCs (generate NPCs on-demand via persona.lua)
- [ ] Images (get_scene_images, get_asset_prompt, get_pin_key)
- [ ] Notes system (remember tool with player/public/npc scopes)

## 4. NPC list

For each NPC:
- Name, age, personality (2-3 sentences)
- Which get LLM agents (agent.lua) vs code-only (npc.lua) vs static data
- Secret / knowledge fence (what they would never reveal)
- Daily routine (if using time system)
- Short-term and long-term goals (if using agents)

## 5. World structure

- Static locations (hand-coded) vs procedural (generated on-demand)
- TRAVEL_MAP: which locations connect to which
- Starting location

## 6. Provider

- Local (Ollama): fast, free, private. Best for: testing, simple scripts
- Cloud (OpenRouter/Claude/OpenAI): higher quality. Best for: final product, complex agents
- Split: code-focused LLM for tools, narrative LLM for `query_llm` narration calls

## 7. Private script?

If yes → place in `my_scripts/`, never reference in commits.
If using persona.lua → use per-adventure subfolder: `my_scripts/npcs_<adventure_name>/`

## After collecting answers

**Default (quickstart path):**
1. `read_knowledge("quickstart")` + `read_file("scripts/template_min.lua")`
2. Read relevant lib docs via `read_knowledge` for each feature enabled
3. Write the `quick.define{...}` spec in one shot (data first, mechanics after)
4. Check syntax: `check_lua_syntax(code)`
5. Write file: `write_file("my_scripts/my_adventure.lua", code)` (my_scripts/ if private)
6. After loading: run `/validate` in game and fix reported ids

**Advanced (template path — only when quickstart can't cover the features):**
1. Read template.lua: `read_file("scripts/template.lua")` + `read_knowledge("template_ref")`
2. Same steps 2-6 as above, writing the full script contract by hand
