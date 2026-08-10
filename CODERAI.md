# CoderAI — Reference

AI coding assistant integrato nel web UI di RpgAi. Permette di scrivere,
debuggare ed evolvere script Lua direttamente dal browser.

**Implementazione completa** (tutte le fasi completate).  
Provider: openrouter / openai / ollama (Claude e Gemini devono passare per
openrouter — il tool loop usa il wire format OpenAI).

---

## Architettura

```
Browser (CoderAI panel)
    │ REST
    ▼
main.cpp — CodingSession
    POST /api/coder/chat      → tool loop (max 20 iter, 120s budget)
    POST /api/coder/approve   → riprende loop dopo pending tool
    POST /api/coder/deny      → "user declined" → LLM sceglie alternativa
    POST /api/coder/reset
    GET  /api/coder/status
    GET  /api/coder/image?path=|url=  → serve immagine locale o proxy URL
    GET  /api/coder/files?path=       → directory listing lazy
    POST /api/reload          → hot-reload script (shared con game UI)
```

```cpp
struct CodingSession {
    std::vector<Message>          history;
    CoderPendingApproval          pending;   // tool in attesa di approvazione
    AIProvider                    provider;
    std::string                   model;
    bool                          active = false;
};
```

**Locking:** sempre `coder_mutex` → `lua_mutex`, mai invertito.  
**CSRF:** CsrfGuard middleware blocca POST cross-origin (browser); client non-browser senza Origin sono ammessi.  
**Pending guard:** `/api/coder/chat` ritorna HTTP 409 se `pending` è set — approvare o negare prima.

---

## Tool List

### Auto (nessuna conferma)

| Tool | Descrizione |
|------|-------------|
| `read_file(path)` | Legge file. Whitelist: scripts/, saves/, images/, my_scripts/ |
| `list_files(pattern)` | Glob ricorsivo |
| `find_definition(symbol)` | grep -n per definizione |
| `find_usages(symbol)` | grep -rn ricorsivo |
| `check_lua_syntax(code)` | luajit -bl su file temp |
| `get_game_state()` | Chiama get_status_for_ai() + get_display_state() sotto lua_mutex |
| `get_script_errors()` | Ring buffer ultimi 20 errori Lua |
| `reload_script(preserve_state?)` | Hot-reload; con preserve=true chiama get_state_snapshot/restore_state |
| `read_knowledge(topic)` | Legge scripts/coder_knowledge/<topic>.md |
| `update_coder_memory(content)` | Append a coder_memory.md (persiste tra sessioni) |
| `web_search(query)` | DuckDuckGo o Brave (--search-provider) |
| `search_images(query)` | Ricerca immagini (Pixabay + DuckDuckGo images) |
| `analyze_image(path, question?)` | Vision LLM su immagine locale o URL |
| `t2i_reference(action, char_id?, file?)` | t2i_locale refs API (list/health/build/add) |

### Confirm (modal UI)

| Tool | Preview mostrato |
|------|-----------------|
| `write_file(path, content)` | Diff vs file esistente o full content |
| `str_replace(path, old, new)` | Diff colorato |
| `run_lua(code, timeout_s?)` | Codice da eseguire |
| `eval_lua(code)` | Esegue su live game state (solo globali Lua) |
| `call_undo(steps?)` | Chiamata a /api/undo |
| `load_save(filename)` | Chiama /api/load |
| `copy_file(src, dst)` | Copia file (whitelist) |
| `download_asset(url, save_path)` | Scarica file da URL |
| `generate_image(prompt, save_path)` | text_to_image() dal provider t2i configurato |
| `edit_image(input, instruction, output?)` | image_to_image() bypass_cache=true |
| `generate_portrait(prompt, path, char_id?, id_scale?)` | FLUX + IP-Adapter face conditioning (richiede --img-url) |
| `generate_scene(prompt, chars[], path)` | Multi-NPC face conditioning |

### Danger (conferma esplicita)

| Tool | Conferma |
|------|---------|
| `delete_file(path)` | Digitare nome file |

---

## run_lua — Sandbox

Secondo `lua_State*` separato dal gioco. Timeout 30s (max 60), output cap 64KB.

**Disponibile:**
- Libs: `base, string, table, math, os, package` (no `io` — filesystem bloccato)
- `require("lib/json")`, `require("lib/json_repair")` da scripts/lib/
- `query_llm(sys, hist_json, user, schema, model?, provider?)` — default = engine narrator
- **`get_tier(name)`** — ritorna `{model, provider}` per `"gen"|"agent"|"ambient"`
  (scopre i tier configurati via CLI/Settings senza hardcoding)

**NON disponibile:** `state`, `agents`, variabili locali dello script di gioco,
`io.open`, `os.execute`, `io.popen`.

**eval_lua vs run_lua:**

| | run_lua | eval_lua |
|--|---------|----------|
| Lua state | sandbox isolato | live game state |
| Accesso a `state`/`agents` | ✗ | solo globali Lua¹ |
| `get_tier` | ✓ | ✓ |
| Sessione richiesta | no | sì (PLAYING) |
| Side effect su gioco | no | sì (attenzione) |

¹ `state`, `NPC_DATA`, `agents` sono locali allo script — invisibili a eval_lua.
Usa `get_game_state()` per leggere state in modo sicuro.

---

## Structural Change Pattern: save → reload

Per modifiche strutturali mentre il gioco è in corso (nuovo NPC, nuova
location, nuovo tool, modifica persona file):

```
1. check_lua_syntax(snippet)             — valida prima di toccare file
2. str_replace(path, old, new)           — applica modifica
   oppure write_file per un nuovo file npcs/<id>.lua
3. reload_script(preserve_state=true)
   — hot-swap del .lua
   — restore_state() al suo interno chiama persona.reload_all()
     → persona file changes attivi subito
4. Se result.success == false:
   a. str_replace(path, new, old)        — reverte
   b. reload_script(preserve_state=true) — ripristina stato funzionante
```

**Rollback automatico:** il snapshot in-memory catturato da preserve_state
è il safety net. Un backup su disco (`/api/save`) prima della modifica
aggiunge un secondo livello di sicurezza per cambi rischiosi.

**Limite fisso:** lib files (adventure.lua, persona.lua, world.lua, agent.lua,
npc.lua, etc.) NON vengono ricaricati — `require` li caccia in `package.loaded`.
Cambi alle lib richiedono restart engine (kill + relaunch). Non aggirabile.

---

## Model Tiers in CoderAI

CoderAI usa il proprio modello (`--coder-provider`/`--coder-model`) per il
suo tool loop. Per generazione di qualità (NPC, location, codice complesso)
può usare il tier "gen" senza sostituire il proprio modello:

```lua
-- In run_lua: genera un NPC con il modello forte configurato
local t = get_tier("gen")
local result = query_llm(
    "Sei un generatore di NPC per un RPG siciliano anni '90.",
    "[]",
    "Genera profilo per: Vito, 55 anni, venditore di ghiaccio",
    nil,
    t.model,
    t.provider
)
print(result)
```

Tiers disponibili: `"gen"` (forte, generazione one-shot), `"agent"` (economico,
agenti NPC), `"ambient"` (più economico, eventi off-screen).

Fallback: se `--gen-model` non è configurato, `get_tier("gen")` ritorna il
modello narrator di default — sempre sicuro da chiamare.

---

## System Prompt

```
[FISSO — sempre iniettato]
  Descrizione engine + lista tool
  Regole operative (str_replace, whitelist, DECISIONS, immagini)
  Pattern save→reload (struttura cambi strutturali)
  Pattern get_tier (come usare gen tier da run_lua)

[DINAMICO — se non vuoto]
  Contenuto di coder_memory.md (preferenze persistenti dell'utente)

[ON DEMAND via tool]
  scripts/coder_knowledge/<topic>.md — letto con read_knowledge
```

Knowledge base (read-only, in repo):
`lua_api`, `lib_adventure`, `lib_persona`, `lib_world`, `lib_agent`,
`lib_memory`, `lib_tools`, `patterns`, `template_ref`, `decisions_guide`

`scripts/coder_memory.md` — writable, gitignored. CoderAI scrive con
`update_coder_memory`. Iniettato in ogni system prompt.

---

## Hot-Reload (reload_script)

1. Lock `lua_mutex`
2. Chiudi e ricrea Lua state
3. Registra tutte le funzioni C++ (query_llm, get_tier, ecc.)
4. Esegui `require(current_script_path)`
5. Con preserve_state=true: chiama `get_state_snapshot()` prima (snapshot
   in-memory), poi `restore_state(snapshot)` dopo il reload
6. Senza preserve: reset completo → `get_welcome_message()`
7. Errore di sintassi Lua: Lua state precedente viene ripristinato

**NON ricarica:** adventure.lua, persona.lua, world.lua e tutte le lib in
`package.loaded`. Per queste serve restart engine.

---

## Image Tools

| Tool | Backend | Note |
|------|---------|------|
| `analyze_image(path, q?)` | vision LLM (coder_vision_query) | path locale (whitelist) o URL esterno |
| `generate_image(prompt, dst)` | text_to_image() da llm_image.h | qualsiasi provider t2i configurato |
| `edit_image(src, instr, dst?)` | image_to_image() bypass_cache=true | default output = stem_edited.ext |
| `generate_portrait(prompt, path, char_id?, scale?)` | t2i_locale /generate_portrait | FLUX + PuLID face conditioning |
| `generate_scene(prompt, chars[], path)` | t2i_locale /generate_scene | multi-NPC face conditioning |
| `t2i_reference(action, char_id?, file?)` | t2i_locale refs API | list/health=GET, build=POST, add=multipart |

---

## CLI Flags

```bash
--coder-provider   ollama|openrouter|openai   # (claude/gemini → via openrouter)
--coder-model      <model>
--coder-key        <api key>                  # eredita --or-key se stesso provider
--coder-knowledge  <path>                     # default: scripts/coder_knowledge/
--search-provider  duckduckgo|brave           # default: duckduckgo
--search-key       <brave key>
--pixabay-key      <pixabay key>
```

Tutti i tier (gen/agent/ambient/coder) sono modificabili live dal pannello
Settings (tab LLM → Model tiers) senza restart.

---

## Flusso tipico: diagnosi e fix in partita

1. Master nota anomalia (tool mai usato, prompt sbagliato, location errata)
2. Apre tab CoderAI → chiede analisi
3. CoderAI: `get_game_state` → stato live, `get_script_errors` → errori recenti
4. `read_file` / `find_definition` → identifica il problema
5. Fix chirurgico via `str_replace` (richiede approvazione)
6. `reload_script(preserve_state=true)` → script aggiornato, partita continua

## Flusso tipico: aggiungere NPC con profilo quality

1. CoderAI discute con master: nome, età, ruolo, personalità
2. `run_lua` con `get_tier("gen")` → chiama LLM forte → genera profilo completo
3. `write_file("scripts/npcs/vito.lua", ...)` → scrive persona file
4. `str_replace` su avventura → aggiunge vito a NPC_DATA e init_agents
5. `reload_script(preserve_state=true)` → NPC live senza restart

## Flusso tipico: aggiungere stanza

1. `read_file` su avventura → legge LOCATIONS e TRAVEL_MAP correnti
2. `str_replace` → aggiunge entry in LOCATIONS e connessioni in TRAVEL_MAP
3. `check_lua_syntax` (opzionale ma raccomandato prima del reload)
4. `reload_script(preserve_state=true)` → stanza disponibile
5. adv.prompt_exits() la trova automaticamente nel BFS

---

## Limitazioni Note

- **Lib non ricaricabili:** adventure.lua, persona.lua etc. necessitano restart engine
- **eval_lua vede solo globali:** `state`, `NPC_DATA`, `agents` sono locali allo script
- **Una sola CodingSession:** nessun multi-session; history persa a restart engine
- **my_scripts/ in whitelist è hardcoded:** da rendere configurabile via flag prima di rilascio pubblico
- **`list_available_models` non implementato:** CoderAI deve conoscere modelli da CLI o chiedere all'utente

---

## TODO (non implementati)

- `list_available_models()` → ritorna provider/model configurati (gen/agent/ambient/narrator/coder)
- `patch_game_state(patch_json)` → deep-merge patch su live state (bypass per variabili locali)
  [save→reload lo rende meno urgente per la maggior parte dei casi]
- `--coder-scripts-path` flag per rendere configurabile la whitelist my_scripts/
- Persist CodingSession history su file (ora si perde a restart engine)
