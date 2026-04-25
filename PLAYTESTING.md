# RpgAi Playtesting Log

This document records automated playtesting sessions run against the public demo
script (`fantasy_demo.lua`). Each session is followed by a list of findings and
the fixes applied. The goal is to catch bugs and design gaps that are invisible
from code review alone.

---

## Session 1 — `fantasy_demo.lua` baseline (2026-04-22)

**Setup:** console mode, `google/gemini-3-flash-preview` via OpenRouter, ~9 turns.

**Scenario:** natural exploration run — enter tomb, look around, search for items,
move deeper, fight an enemy, use `/observe`.

### Findings

| # | Severity | Description |
|---|---|---|
| 1 | **High** | No HP damage system. Player could be hit by the skeleton guard multiple times with HP remaining at 10/10. Combat had narrative tension but zero mechanical consequence. Death was only possible via `game_over: true` directly from the LLM. |
| 2 | Medium | `avanza_tempo` — an Italian field name — appeared in the JSON schema returned to the LLM. In English-only sessions this works, but it is inconsistent with the rest of the codebase. |
| 3 | Low | `print_thinking()` default message was `"Elaborazione..."` (Italian). Visible to every console-mode player. |
| 4 | — | *(not a bug)* Empty input appeared to quit the game. Confirmed it was the explicit `/quit` in the test; empty input is correctly handled with `continue` in the game loop. |

### Fixes applied

**1. HP damage system** (`scripts/fantasy_demo.lua`)
- Added `hp_change` field to `get_json_schema()`:
  ```json
  "hp_change": { "type": "integer", "minimum": -5, "maximum": 0 }
  ```
- Added damage rule to system prompt: LLM must set `hp_change` to a negative
  value when the player is hit; ghost scribe explicitly marked as non-hostile.
- Added damage application in `process_ai_response()`: subtracts from
  `state.player.health`; triggers `game_over` when health reaches 0.

**2. Renamed `avanza_tempo` → `time_advance`** (`scripts/fantasy_demo.lua`)
- All three occurrences updated (schema definition, `process_ai_response` read,
  state application).

**3. Fixed Italian spinner** (`src/main.cpp`)
- `print_thinking()` default changed from `"Elaborazione..."` to `"Thinking..."`.

### Verification session

Re-run with the same model, forcing 3 consecutive combat turns against the
skeleton guard:

```
Turn 1 — "I fight the skeleton guard"       HP: 10 → 9  (-1)
Turn 2 — "I keep fighting, pressing attack" HP: 9  → 8  (-1)
Turn 3 — "I fight on, refusing to retreat"  HP: 8  → 7  (-1)
```

Damage applied correctly. HUD updates live. Combat now has mechanical weight.

---

## Session 2 — engine bugs found during internal testing (2026-04-22)

**Context:** internal script test (script not published).

### Findings

| # | Severity | Description |
|---|---|---|
| 1 | **High** | Dream schema constant undefined — `generate_dreams()` failed silently, `/sogni` always returned "no dreams yet" even after a day change. |
| 2 | **Medium** | `--save-path` ignored in console mode. `cfg.savePath` was never prepended to `cfg.saveFile`; file was written to CWD instead of the specified directory. |

### Fixes applied

**1. Dynamic dream schema** (`scripts/<script>.lua` pattern)
- Build the JSON schema for dream generation dynamically at call time, enumerating
  only the NPCs with `confidenza > 0` as explicit required properties.
- Never rely on a module-level schema constant for LLM calls that depend on
  runtime state.

**2. Console save path** (`src/main.cpp`)
- After arg parsing: `if (!cfg.savePath.empty()) cfg.saveFile = cfg.savePath + cfg.saveFile;`
- `--save-path saves/` now correctly writes `saves/session_log.jsonl`.

---

## How to run a playtesting session

1. Build the release binary: `./build.sh`
2. Pipe a sequence of player inputs:

```bash
{
  sleep 3;  echo "PlayerName"
  sleep 8;  echo "first action"
  sleep 12; echo "second action"
  # ...
  echo "/quit"
} | ./build/rpgai \
    --path ./scripts \
    --script fantasy_demo.lua \
    --provider openrouter \
    --or-key YOUR_KEY \
    --or-model google/gemini-3-flash-preview 2>&1 | tee session.log
```

3. Review `session.log` for: Lua errors, HP never changing during combat,
   locations not updating, items not appearing in inventory, unexpected `/quit`.

## What to look for

- **Mechanical gaps** — game state fields that never change (HP, flags, inventory)
  despite narrative events that should trigger them.
- **Schema drift** — LLM inventing field names not in the schema, or refusing
  to return required fields.
- **Logic exploits** — actions that should be blocked (locked door, sealed tomb)
  but aren't enforced by `process_ai_response`.
- **Narrative inconsistencies** — LLM placing the player in a location they
  didn't move to, referencing defeated NPCs as still present, etc.
- **Untranslated strings** — any visible text in a language other than the
  script's target language.
