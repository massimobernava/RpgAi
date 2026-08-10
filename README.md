# ⚔ RpgAi

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)]()
[![LuaJIT](https://img.shields.io/badge/Lua-JIT-purple.svg)](https://luajit.org/)

> *The dungeon master never sleeps. The story never ends.*

**RpgAi** is an open-source engine that turns any Large Language Model into a living, breathing game master for text-based RPGs. Write your world in Lua. Let the AI narrate it.

---

## What is this?

RpgAi is a C++ engine that bridges **Lua game scripts** and **LLM providers** to create immersive, dynamic text adventures. The game world — characters, locations, rules, relationships — lives entirely in a Lua script you write. The engine feeds the current game state to the LLM of your choice, which narrates the outcome, drives NPC behaviour and keeps the story coherent turn after turn.

Whether you want to recreate a classic D&D dungeon crawl, build an interactive mystery novel, or design a slice-of-life social simulator, RpgAi gives you the tools without locking you into any particular story structure or AI provider.

![RpgAi web UI — scene image, narration and HUD](docs/screenshot.png)

▶ **[Watch the demo on YouTube](https://youtu.be/zeZxTmyhi08)**

---

## ✨ Features

**For players and storytellers**
- 🎲 **Any genre, any setting** — fantasy, sci-fi, horror, romance, historical. If you can describe it in Lua, the AI can narrate it
- 🧠 **Persistent world state** — NPCs move, relationships evolve, time passes. The game remembers everything
- 🌐 **Beautiful web UI** — play in your browser with a clean dark interface; no terminal required
- 🖼 **AI-generated scene images** — automatic scene illustration via image models (WaveSpeed, fal.ai, OpenRouter and more)
- 🔊 **AI voice narration** — optional local TTS server (Coqui XTTS v2) for zero-shot voice cloning; pipeline playback in the browser so generation and audio overlap seamlessly
- 💾 **Save & load** — full session persistence with JSONL save files
- 📖 **RAG narrative style** — feed the engine example narrations to lock in the tone and prose style of your world

**For developers and world-builders**
- 🤖 **CoderAI** — in-browser AI coding assistant for writing, debugging and evolving adventure scripts without leaving the web UI (see [CoderAI](#-coderai))
- 🔌 **Multi-provider LLM support** — Ollama (local), OpenRouter, Gemini, OpenAI, Claude, or any OpenAI-compatible endpoint
- 🖊 **Lua scripting** — simple, readable scripts define everything: locations, NPCs, game rules, JSON schema, commands
- ⚡ **LuaJIT powered** — fast Lua execution with full access to the C++ engine via exposed functions
- 🔧 **Hackable C++ core** — clean header-based architecture designed to be extended
- 🗂 **Smart image cache** — scene images cached by composition key (asset set + mtime + size); unchanged scenes reuse existing renders, regenerations replace the cached entry; thread-safe with atomic writes
- 🔁 **In-game commands** — `/fix`, `/observe`, `/summary`, `/image` and custom Lua commands in both console and web mode
- 🖥 **Local AI servers** — optional Python servers for local image generation (FLUX + PuLID face conditioning) and TTS (XTTS v2), manageable from the web UI (install deps, start, stop)
- 🧠 **NPC agent system** — LLM-driven NPCs as first-class objects (`lib/agent.lua`): shared turn caps, structured fallback, idempotent caching
- 💭 **Persistent memory** — cross-session fact storage per entity/category (`lib/memory.lua`), readable by agents, writable by the main LLM via tool calls
- 🌍 **Procedural world expansion** — locations, objects and NPCs generated on-demand by the LLM and persisted to disk (`lib/world.lua`, `lib/persona.lua`)
- 🎭 **Autonomous NPCs with event-gated LLM** — NPCs run mostly on cheap scripted behaviour (routines, needs, probabilistic event variations, multi-step sequences); the LLM fires only on beats you mark, so a whole cast can live "off-screen" affordably (`agent.tick_and_log`)
- 📊 **Per-component token accounting** — the engine reads `usage` from every response and attributes tokens by component (narrator / agent / generation / ambient), so you can see exactly where tokens go and pick cheap models for the high-volume roles (`get_token_usage()`)
- 🧩 **Provider schema compatibility** — JSON schemas are auto-adapted per model (Google `additionalProperties`, Anthropic integer `min/max` & array `minItems`): write the constraints once, the engine strips what a given provider can't enforce
- ⏰ **Target-time skips** — a `sleep_until` tool jumps the clock to a wake time (not a duration), running the same off-screen simulation, so "sleep until morning" keeps narration and clock in sync

---

## 🚀 Quick start

```bash
# 1. Clone
git clone https://github.com/yourusername/RpgAi.git
cd RpgAi

# 2. Install system dependencies (macOS)
brew install luajit asio curl cmake

# 3. Place header-only dependencies in vendor/  (see docs/installation.md)

# 4. Build
./build.sh

# 5. Run with Ollama (local, no API key needed)
./build/rpgai --web --provider ollama --model llama3 --path scripts/
```

Then open **http://localhost:8080**, select a script and start playing.

For full installation instructions, troubleshooting and first-run examples, see **[docs/installation.md](docs/installation.md)**.

---

## 🏗 Architecture

> **A note on authorship:** roughly 90% of the C++ and Lua code in this repository was written by [Claude](https://claude.ai) (Anthropic's AI assistant) through an extended pair-programming session. Architecture, design decisions and feature roadmap were directed by the human author.

### Repository layout

```
RpgAi/
├── src/
│   ├── main.cpp          # Engine core, CLI, web server, game loop
│   ├── llm_query.h       # LLM provider abstraction (Ollama, OpenRouter, Gemini…)
│   ├── llm_image.h       # Image generation, collage builder, scene cache
│   └── web_page.h        # Embedded single-page web UI (HTML/CSS/JS)
├── scripts/
│   ├── fantasy_demo.lua  # Demo adventure — classic fantasy
│   ├── npc_demo.lua      # Demo adventure — code-driven NPCs with routines
│   ├── template.lua      # Full-featured adventure template (heavily commented)
│   └── lib/
│       ├── json.lua          # Pure-Lua JSON encoder/decoder
│       ├── json_repair.lua   # Auto-repair malformed LLM JSON
│       ├── tools.lua         # Pre-built tool definitions (dice, skill check, inventory…)
│       ├── npc.lua           # Code-driven NPC system (routines, needs, events)
│       ├── agent.lua         # LLM-driven NPC agents (composed with npc.lua)
│       ├── memory.lua        # Persistent structured memory (JSON-backed)
│       ├── world.lua         # Procedural location/object generation on demand
│       └── persona.lua       # File-backed procedural NPCs (grow over time)
├── tools/
│   ├── test_player.py    # Automated AI playtest driver (Playwright + OpenRouter)
│   ├── img2text.py/sh    # VLM scene description (Ollama vision models)
│   ├── t2i.py/sh         # Text-to-image generation helper
│   └── qwen_edit.py/sh   # Image-to-image editing helper
├── t2i_locale/
│   ├── server.py         # FastAPI image server (FLUX + PuLID face conditioning, port 8001)
│   └── requirements.txt  # Python deps for t2i_locale
├── tts_locale/
│   ├── server.py         # FastAPI TTS server (Coqui XTTS v2, port 8004)
│   ├── patch_tts_compat.py  # Compatibility patches for PyTorch 2.6+ / transformers 4.37+
│   └── test_tts.py       # CLI test script (--url for remote servers)
├── docs/                 # Extended documentation
├── vendor/               # Header-only dependencies (see docs/installation.md)
├── CMakeLists.txt
└── build.sh
```

### Core components

**`main.cpp`** — engine heart. CLI parsing, LuaJIT lifecycle, Crow web server (14 routes), console game loop, async image job system, RAG engine, session persistence.

**`llm_query.h`** — single abstraction for all text generation providers behind one function: `query_llm(provider, sys_prompt, history, user_prompt, json_schema, model)`. Supported providers: Ollama, OpenRouter, Gemini, OpenAI, Claude, any OpenAI-compatible endpoint.

**`llm_image.h`** — everything visual: asset collage builder, text-to-image, image-to-image, scene cache. Providers: stable-diffusion.cpp, OpenAI, OpenRouter, fal.ai, WaveSpeed, DashScope, AIMLAPI, Qwen local. t2i and i2i can use different providers independently. Every result is validated (HTTP status + image magic bytes) before being cached or returned; the cache db is mutex-protected and written atomically.

**`web_page.h`** — the entire single-page application as a C++ raw string literal. No build step, no bundler — Crow serves it directly from memory.

### Data flow — one game turn

```
Player input
     │
     ▼
process_player_input()    ← Lua: handles /commands
     │
     ▼
get_status_for_ai()       ← Lua: world state → JSON
get_system_prompt()       ← Lua: current system prompt
get_json_schema()         ← Lua: schema LLM must follow
     │
     ▼
[RAG injection]           top-N style examples added to system prompt
     │
     ▼
query_llm()               HTTP to LLM provider
     │
     ▼
process_ai_response()     ← Lua: validate JSON, update state, return narration
     │
     ▼
write_turn()              save to disk
     │
     ▼
narration + updated HUD → player
```

### Design philosophy

**The engine is dumb; the script is smart.** C++ handles IO, HTTP, LLM calls and persistence. Everything that makes your adventure *yours* lives in Lua — change it without recompiling.

**The LLM is a narrator, not a rules engine.** The JSON schema your script defines constrains what the LLM can do. It cannot invent new locations or ignore the time system — schema and Lua validation enforce the rules. The LLM's job is to write compelling prose within those constraints.

**Fail gracefully.** Every LLM call retries up to `--max-retries`. Image generation is async and non-blocking. Missing assets are generated on demand.

### Lua agents — secondary LLM calls from script

Because `query_llm` is exposed directly to Lua, your script can make **additional, independent LLM calls** at any point — not just inside the main GM turn. These secondary calls are called *agents* and follow a simple pattern: read `state`, call `query_llm` with a focused prompt and a tight JSON schema, then apply the result directly to `state`.

Agents run in the same Lua state as the rest of the script, so they have full access to all game data and can modify it freely. The engine imposes no structure — each agent decides for itself when to activate, what to ask, and what to change.

```lua
-- Example: a world-events agent that fires once per in-game day
local function world_events_agent(minuti_avanzati)
    if state.giorno == state._last_event_day then return end   -- already ran today
    state._last_event_day = state.giorno

    local schema = '{"type":"object","required":["evento","desc"],' ..
                   '"properties":{"evento":{"type":"string"},"desc":{"type":"string"}}}'
    local ok, reply = pcall(query_llm,
        "You are a world event generator for a summer Italian city RPG.",
        "[]",
        "Generate a small background event for today: a market, a street musician, "
     .. "a shop closure, a local festival. Keep it grounded and brief.",
        schema)
    if not ok then return end

    local ok2, data = pcall(json.decode, reply)
    if ok2 and data.evento ~= "" then
        state.evento_giorno = data
        push_log("📰 " .. data.evento)
    end
end

-- Called wherever makes sense — inside process_ai_response, a slash command, etc.
-- No central dispatcher required.
```

Common agent patterns:

| Agent | When to fire | LLM needed? |
|---|---|---|
| NPC needs (hunger, sleep, …) | every turn, deterministic | no |
| Relationship natural drift | day rollover | no |
| World events | day rollover | yes (small schema) |
| NPC gossip propagation | after a public action | yes |
| Dream generation | night / day rollover | yes |
| Rival NPC with own goals | every few turns | yes |
| Economy / shop prices | weekly | no |

**Performance note:** each `query_llm` call inside an agent is synchronous and blocks the Lua mutex in web mode. Keep LLM agents rare (day-boundary events, significant triggers) and prefer deterministic logic for per-turn updates.

### Tool calling — LLM-driven game mechanics

Tool calling lets the LLM invoke Lua functions mid-narration. Instead of having the GM invent a dice result, it calls `roll_dice` and gets a real one. Instead of guessing inventory, it calls `inventory_check`. The engine handles the back-and-forth automatically; the script just declares which tools are available.

Enable tool calling by implementing the optional `get_tools()` function:

```lua
local tools = require("lib/tools")

function get_tools()
    return tools.build({
        tools.roll_dice(state),
        tools.skill_check(state),
        tools.inventory_check(state),
        tools.buy_item(state, SHOP_PRICES),
    })
end
```

`scripts/lib/tools.lua` ships these pre-built tools:

| Tool | What it does |
|---|---|
| `roll_dice` | Roll N dice with S sides, return total + individual rolls |
| `skill_check` | d20 + player skill bonus vs difficulty, flags crit/fumble |
| `inventory_check` | How many of an item the player has |
| `buy_item` | Atomic purchase: deducts gold or returns failure |
| `sell_item` | Atomic sale: adds gold only if player has the item |
| `query_state` | Expose any Lua table field to the LLM on demand |

Custom tools follow the same `{ name, description, params, fn }` schema — `params` is a JSON Schema string, `fn` is a Lua function that receives a JSON args string and returns a JSON result string.

Scripts without `get_tools()` work exactly as before — the engine falls back to standard single-turn LLM calls.

---

## 🖥 Local servers

RpgAi can offload image generation and TTS to optional Python servers running on the same machine or a remote GPU box. All servers are managed from the **Settings → Local Servers** tab in the web UI — install dependencies, start and stop with one click.

### TTS locale server (`tts_locale/server.py`)

Provides zero-shot voice cloning using **Coqui XTTS v2**. Upload a 6–30 second reference WAV and the model synthesises new speech in that voice.

```bash
# Install deps (or use the web UI Install button)
pip install transformers==4.36.2 TTS torchaudio soundfile numpy fastapi uvicorn python-multipart

# Apply compatibility patches for PyTorch 2.6+ and transformers 4.37+
python tts_locale/patch_tts_compat.py

# Start
python tts_locale/server.py --host 0.0.0.0
```

Key options: `--temperature` (voice similarity, default 0.2), `--strip-silence` (clean reference WAV before embedding), `--clean-text` (strip markdown before synthesis), `--device cuda|mps|cpu`.

Once running, configure the narrator voice in Settings → Local Servers → TTS. The web UI fetches audio in sentence-sized chunks fired in parallel — sentence N+1 is generated while sentence N is playing, keeping latency low on long narrations.

### t2i locale server (`t2i_locale/server.py`)

Provides local text-to-image and image-to-image rendering using **FLUX** models with optional **PuLID** face conditioning. Designed for RTX 5090 / 32 GB VRAM; works on smaller cards with TorchAO quantization.

```bash
# Install deps
pip install -r t2i_locale/requirements.txt

# Start (basic)
python t2i_locale/server.py --host 0.0.0.0

# With PuLID face conditioning
python t2i_locale/server.py --host 0.0.0.0 --pulid
```

Set the server URL in **Settings → Image T2I / I2I → URL** (e.g. `http://192.168.1.x:8001`). When PuLID is enabled, store a reference face for an NPC once; subsequent generations use it automatically for consistent character appearance.

---

## 🤖 CoderAI

CoderAI is an in-browser AI coding assistant built into the web UI. It runs as a separate chat thread — independent from the active game session — and can read/write files, inspect the live game state, execute Lua in a sandbox, search the web, and generate or edit images, all from a single panel.

The primary workflow is **observe → diagnose → surgical fix → hot-reload**, without ever opening a terminal or editor.

### Enabling CoderAI

CoderAI uses a separate LLM provider/model from the game. A capable model with tool-calling support is strongly recommended (e.g. Claude or GPT-4o via OpenRouter, or a strong local model like Qwen through Ollama).

> **Supported coder providers:** `openrouter`, `openai`, `ollama`. CoderAI's tool loop speaks the OpenAI tool-calling wire format, so `--coder-provider claude` and `--coder-provider gemini` are **not** supported directly — run those models *through* OpenRouter instead (e.g. `--coder-model anthropic/claude-3.5-sonnet`). Selecting an unsupported coder provider returns a clear error in the CoderAI panel instead of failing silently.

```bash
# Run Claude as the coder model via OpenRouter, Ollama for the game
./build/rpgai --web \
  --provider ollama --model llama3.2 --path scripts/ \
  --coder-provider openrouter --coder-model anthropic/claude-3.5-sonnet --coder-key sk-or-...

# Use the same OpenRouter key for both game and CoderAI
./build/rpgai --web \
  --provider openrouter --or-key sk-or-... --or-model anthropic/claude-3.5-sonnet \
  --path scripts/ --save-path saves/ --save-mode full \
  --coder-provider openrouter --coder-model anthropic/claude-3.5-sonnet
```

Then open **http://localhost:8080** and click the **CoderAI** tab.

> **Tip:** use `--save-mode full` during development sessions. It persists every turn to disk, letting CoderAI's `load_save` tool restore the game to any prior point for deep undo.

### CLI flags

| Flag | Default | Description |
|------|---------|-------------|
| `--coder-provider` | inherits main | `ollama`, `openrouter`, `openai` (Claude/Gemini: route via OpenRouter) |
| `--coder-model` | inherits main | Model name for the coder LLM |
| `--coder-key` | inherits main key | API key override for the coder provider |
| `--coder-knowledge` | `scripts/coder_knowledge/` | Path to the read-only knowledge base |
| `--search-provider` | `duckduckgo` | `duckduckgo` or `brave` |
| `--search-key` | — | Brave Search API key |
| `--pixabay-key` | — | Pixabay API key for image search |

### Tool reference

**Tier: auto** — executed without confirmation

| Tool | What it does |
|------|-------------|
| `read_file(path)` | Read any file in `scripts/`, `saves/`, `images/`, `my_scripts/` |
| `list_files(pattern)` | Glob file listing, e.g. `scripts/lib/*.lua` |
| `find_definition(symbol)` | grep for function/variable definition |
| `find_usages(symbol)` | Recursive grep across codebase |
| `check_lua_syntax(code)` | Validate Lua snippet via `luajit` |
| `read_knowledge(topic)` | Read a knowledge base file (lua_api, lib_adventure, patterns…) |
| `update_coder_memory(content)` | Append a persistent note to `coder_memory.md` |
| `get_game_state()` | Live game state: `get_status_for_ai()` + `get_state_snapshot()` |
| `get_script_errors()` | Last 20 Lua errors captured during the session |
| `web_search(query)` | Web search via DuckDuckGo (or Brave with key) |
| `search_images(query)` | Image search; results shown as thumbnails inline |
| `analyze_image(path, question?)` | Vision LLM description of a local image or URL |
| `t2i_reference(action, char_id?, file?)` | List/check/add/build face references on t2i_locale server |

**Tier: confirm** — shown in an approval modal before execution

| Tool | What it does |
|------|-------------|
| `write_file(path, content)` | Create a new file (diff preview) |
| `str_replace(path, old, new)` | Surgical text replacement (colored diff preview) |
| `run_lua(code, timeout_s?)` | Execute Lua in a sandbox (separate state, no game access) |
| `eval_lua(code)` | Execute Lua on the **live game state** (surgical state fixes) |
| `call_undo(steps?)` | Undo last N game turns via in-memory undo stack (max 10) |
| `load_save(filename)` | Load a JSONL save file — enables deep undo with `--save-mode full` |
| `download_asset(url, path)` | Download an image from a URL and save as a local asset |
| `copy_file(src, dst)` | Copy a file (useful to reuse assets across adventures) |
| `generate_image(prompt, path)` | Text-to-image via configured t2i provider |
| `edit_image(input, instruction, output?)` | Image-to-image edit via configured i2i provider |
| `generate_portrait(prompt, path, char_id?, id_scale?)` | NPC portrait with face conditioning (t2i_locale server) |
| `generate_scene(prompt, chars[], path)` | Multi-NPC scene with face conditioning (t2i_locale server) |
| `reload_script(preserve_state?)` | Hot-reload active Lua script; optionally preserve game state |

**Tier: danger** — requires explicit confirmation

| Tool | What it does |
|------|-------------|
| `delete_file(path)` | Permanently delete a file |

### Typical workflow

**Fix a bug mid-session (without stopping the game):**

1. Player notices something wrong (wrong location, NPC ignoring time, tool never called)
2. Open CoderAI tab → describe the symptom
3. CoderAI calls `get_game_state` + `get_script_errors` → sees live state and recent errors
4. Reads relevant code with `read_file` / `find_definition`
5. Proposes a fix via `str_replace` (you see the diff, approve or deny)
6. Calls `reload_script(preserve_state=true)` → script reloaded, game continues

**Create a new NPC portrait:**

1. Ask CoderAI to check available references: `t2i_reference("list")`
2. Generate a portrait: `generate_portrait("Jenny, 22, barista, sorridendo", "my_scripts/images/jenny.png", char_id="jenny")`
3. Optionally refine: `edit_image("my_scripts/images/jenny.png", "make the background a café interior")`
4. Wire it into the script: `str_replace` in `get_asset_path("jenny")` to point to the new file

**Research and implement a new feature:**

1. Ask CoderAI how a library works → calls `read_knowledge("lib_adventure")` and `read_file("scripts/lib/adventure.lua")`
2. Search for examples online → `web_search("Lua RPG inventory system patterns")`
3. Writes the code, checks syntax → `check_lua_syntax(code)`
4. Writes to file → `write_file` or `str_replace`
5. Reloads → `reload_script`

### Knowledge base

`scripts/coder_knowledge/` is a read-only directory of Markdown files that CoderAI reads on demand via `read_knowledge(topic)`. It covers every Lua library API, mandatory patterns, the `§DECISIONS` checklist and the template reference. The knowledge base is versioned with the repo.

`scripts/coder_memory.md` is a writable, gitignored file where CoderAI stores persistent notes across sessions (preferred patterns, in-progress work, user preferences). CoderAI appends to it autonomously via `update_coder_memory`.

### Limitations

- **Hot-reload does not reload lib files** (`adventure.lua`, `persona.lua`, etc.) — `require` caches them. Lib changes need an engine restart.
- **`eval_lua` accesses globals only** — `state`, `agents` and similar locals in the script are not visible. Use `get_game_state()` (which calls the public Lua functions) or `run_lua` for analysis.
- **One CoderAI session per engine instance** — history resets on `[Reset Chat]` or engine restart.
- **`my_scripts/` in the path whitelist is hardcoded** — intended for private adventures not committed to the repo. Make it configurable before sharing a public deployment.

---

## 🔒 Security notes

The web server (`--web`) is built for **local single-user** use and binds to `localhost:8080`. Two guards protect it:

- **CSRF guard.** Some endpoints are powerful — `/api/servers/action` runs shell commands to install/start helper servers, the CoderAI endpoints write files and run Lua, and game turns can spend cloud LLM credits. Any web page open in your browser could otherwise POST to `localhost:8080` (cross-site request forgery). The engine rejects any state-changing `POST` whose `Origin`/`Referer` is not the RpgAi page itself (HTTP 403). The built-in UI is unaffected; command-line clients such as `curl` (which send no `Origin`) are also allowed, since they already have local machine access.
- **CoderAI sandbox.** `run_lua` executes in an isolated Lua state with no filesystem or process access (`io`/`os.execute`/`os.remove`/`loadfile` removed) and a wall-clock timeout that the executed code cannot disable. File tools (`read_file`, `write_file`, `delete_file`, …) are restricted to the `scripts/`, `saves/`, `images/`, and `my_scripts/` trees, with symlink-escape resolution.

This hardens the localhost setup but is **not** an authentication layer. Do not expose the port to an untrusted network without putting it behind a reverse proxy with real auth.

---

## 🧪 Testing your script

`tools/test_player.py` is an automated test player that uses an LLM (via OpenRouter) to drive playthroughs of any adventure script. Useful for QA testing and pre-generating scene images before release.

```bash
pip install playwright requests && playwright install chromium

# Basic test run
python tools/test_player.py --api-key sk-or-... --script my_adventure.lua

# Pre-generate images headless, save to img/ folder
python tools/test_player.py --api-key sk-or-... --script my_adventure.lua \
    --headless --image-every 2 --collect-images img/ --max-turns 40

# Targeted test with a specific objective
python tools/test_player.py --api-key sk-or-... --script my_adventure.lua \
    --objective "Find the hidden merchant and buy the rare sword" --max-turns 20
```

Key options: `--image-every N` (generate images every N turns), `--collect-images DIR` (save images to folder), `--objective` (guide the AI toward a specific path), `--headless` (no browser window), `--cps` (typing speed; use 9 for demo recordings).

---

## 📚 Documentation

| Doc | Contents |
|---|---|
| [docs/installation.md](docs/installation.md) | System deps, header-only libs, build, troubleshooting, first run |
| [docs/cli-reference.md](docs/cli-reference.md) | All CLI flags, in-game commands, full example config |
| [docs/scripting.md](docs/scripting.md) | Lua script guide, required functions, patterns, demo script |
| [docs/images.md](docs/images.md) | Image system, asset pipeline, render modes, provider details |

---

## 🗺 Roadmap

- [ ] `--port` flag to configure the web server port
- [ ] Multi-session web mode (multiple simultaneous players)
- [ ] Two-level scene cache (hard key + soft narrative key for smart i2i reuse)
- [ ] WebSocket streaming for real-time narration display
- [ ] Per-NPC voice cloning — Lua `get_npc_voices()` to assign voice profiles to characters
- [ ] Script hot-reload in web mode
- [ ] Issue templates for bug reports and feature requests
- [x] Narrator TTS — local Coqui XTTS v2 server; pipeline sentence playback; web UI voice selector
- [x] Local t2i server — FLUX + PuLID face conditioning; consistent NPC portraits across scenes
- [x] NPC agent system — `lib/agent.lua`; shared turn caps; idempotent caching; structured fallback
- [x] Persistent memory — `lib/memory.lua`; cross-session entity/category facts; tool-call writable
- [x] Procedural world — `lib/world.lua` + `lib/persona.lua`; on-demand generation; file-backed NPCs
- [x] Tool calling — `lib/tools.lua`; pre-built dice/skill/inventory tools; custom tool schema
- [x] CoderAI — in-browser coding assistant; file tools, game bridge, Lua sandbox, image generation/editing, web search

---

## 🤝 Contributing

Pull requests are welcome — whether you're fixing a C++ bug, adding a new LLM provider, improving the web UI, or sharing a Lua adventure script. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

## 📄 License

MIT License — see [LICENSE](LICENSE).

```
Copyright (c) 2025 Massimo Bernava
```

*Built with C++17 · LuaJIT · sol2 · Crow · nlohmann/json · libcurl · stb · ollama-hpp*

*~90% of the code written by [Claude](https://claude.ai) (Anthropic) · Architecture and direction by Massimo Bernava*
