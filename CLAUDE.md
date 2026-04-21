# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Quick Start

### Build
```bash
# Standard release build
./build.sh

# Debug build (with symbols, no optimization)
./build.sh debug

# Clean rebuild (wipes build/ directory first)
./build.sh clean
```

The binary is placed at `build/rpgai`.

### Run
```bash
# Console mode — terminal gameplay
./build/rpgai \
  --provider ollama \
  --model llama3.2 \
  --path scripts/ \
  --script fantasy_demo.lua

# Web mode — browser UI at http://localhost:8080
./build/rpgai \
  --web \
  --provider openrouter \
  --or-key sk-or-YOUR_KEY \
  --or-model anthropic/claude-3.5-sonnet \
  --path scripts/ \
  --save-path saves/
```

### Verify Build
```bash
./build/rpgai --help
```

## Repository Overview

**RpgAi** is a C++17 game engine that bridges Lua game scripts and Large Language Models to create text-based RPG adventures.

### Core Architecture

The engine has four main layers:

1. **C++ Core** (`src/main.cpp`) — the engine heart
   - CLI parsing and argument handling
   - LuaJIT lifecycle and Lua state management
   - Crow web server with REST API (9 routes)
   - Game loop (console mode) and async job system (web mode)
   - Session persistence (LAST/FULL save modes)
   - RAG (Retrieval-Augmented Generation) for narrative style injection

2. **LLM Provider Abstraction** (`src/llm_query.h`) — provider-agnostic text generation
   - Supports: Ollama, OpenRouter, Gemini, OpenAI, Claude
   - Any OpenAI-compatible endpoint works
   - Handles retries, history management, JSON schema constraints
   - Single entry point: `query_llm(provider, sys_prompt, history, user_prompt, json_schema, model)`

3. **Image Generation System** (`src/llm_image.h`) — visual scene rendering
   - Asset composition (backgrounds + NPC portraits → collage)
   - Text-to-image (t2i) for missing assets
   - Image-to-image (i2i) for final scene rendering
   - Scene caching by asset composition key
   - Supports: stable-diffusion.cpp, OpenAI, OpenRouter, fal.ai, WaveSpeed, DashScope, AIMLAPI
   - Two independent pipelines: t2i and i2i can use different providers

4. **Web UI** (`src/web_page.h`) — embedded single-page application
   - Entire HTML/CSS/JS as C++ raw string literal
   - No build step, no static file serving
   - Crow serves directly from memory
   - Dark theme interface with game log, HUD, sidebar

### Data Flow — One Game Turn

```
Player input
    ↓
process_player_input() [Lua]  → handled by command? skip to display
    ↓ (if not handled)
get_status_for_ai() [Lua]     → world state as JSON
get_system_prompt() [Lua]     → LLM behavior constraints
get_json_schema() [Lua]       → allowed fields/values
    ↓
[RAG injection]               → inject example narrations if --rag enabled
    ↓
query_llm()                   → HTTP to LLM provider
    ↓
process_ai_response() [Lua]   → validate JSON, update state, return narration
    ↓
write_turn()                  → save to disk (LAST or FULL mode)
    ↓
Display: narration + HUD      → player sees result
```

### Lua Script Contract

Every game script implements these required functions:

**Initialization:**
- `get_welcome_message()` — intro text
- `set_initial_state(player_input)` — first turn setup
- `generate_initial_state()` — fallback if player presses Enter empty

**Per-turn lifecycle:**
- `get_status_for_ai()` — serialize world state to JSON
- `get_system_prompt()` — build current LLM system prompt (dynamic per turn)
- `get_json_schema()` — JSON schema the LLM must follow
- `process_ai_response(reply)` — validate LLM response, update state, return narration
- `process_player_input(input)` — handle `/commands` before LLM, return `{success, handled, output?}`
- `get_display_state()` — HUD text shown above chat

**Persistence:**
- `get_state_snapshot()` — full state as JSON (for save)
- `restore_state(snapshot)` — restore state from JSON (for load)

**Optional (image system):**
- `get_scene_images()` — assets for the current scene; supports two return formats:
  - **Format A** (simple): `{ {id="bg", path="..."}, {id="npc", path="..."} }`
  - **Format B** (with hint): `{ assets={...}, base_image="last"|"/abs/path.jpg"|nil }`
    - `"last"` — reuse the most recent cached render of this scene as i2i base (refine instead of rebuild)
    - `"/path"` — use a specific image file as i2i base
    - `nil` — default: build collage from assets and use that as i2i base
- `get_asset_path(id)` — path lookup
- `get_asset_prompt(id)` → {path, prompt} for text-to-image

C++ exposes three functions to Lua:
- `query_llm(sys, history_json, user, schema)` — call LLM directly from Lua
- `get_embedding(text)` → vector of floats (if `--embed-model` configured)
- `cosine_similarity(vec_a, vec_b)` → float [0, 1]

### Key Design Principles

- **Engine is dumb; script is smart** — C++ handles IO/HTTP/LLM. All game logic lives in Lua. Recompile never needed.
- **LLM is narrator, not rules engine** — JSON schema constrains valid output. Lua validation enforces rules.
- **Fail gracefully** — All LLM calls retry up to `--max-retries`. Image generation is async/non-blocking. Missing assets generate on demand.

## Dependencies

### System (installed via package manager, not in `vendor/`)
- LuaJIT (with headers)
- libcurl
- Asio (standalone, not Boost.Asio)
- CMake 3.16+
- C++17 compiler

### Header-only (in `vendor/`)
- **sol2** — Lua/C++ bindings
- **Crow** — HTTP server framework
- **nlohmann/json** — JSON parsing
- **ollama-hpp** — Ollama client
- **stb** — Image load/write/resize (stb_image.h, stb_image_write.h, stb_image_resize2.h)

### Troubleshooting Dependencies

CMake caches `find_path` results. If you add a vendor header after a failed build:
```bash
./build.sh clean
```

If LuaJIT not found on Linux, install dev package:
```bash
# Debian/Ubuntu
sudo apt install libluajit-5.1-dev

# Or pass path explicitly
cmake .. -DLUAJIT_INCLUDE_DIR=/usr/include/luajit-2.1 \
         -DLUAJIT_LIBRARIES=/usr/lib/x86_64-linux-gnu/libluajit-5.1.so
```

## Important Files

- `src/main.cpp` — engine core, CLI, web server, game loop (2400+ lines)
- `src/llm_query.h` — provider abstraction (Ollama, OpenRouter, Gemini, OpenAI, Claude)
- `src/llm_image.h` — image generation, collage builder, scene cache (1600+ lines)
- `src/web_page.h` — embedded web UI (1200+ lines of HTML/CSS/JS as C++ string)
- `CMakeLists.txt` — build configuration
- `scripts/fantasy_demo.lua` — demo adventure (heavily commented, use as template)
- `scripts/lib/json.lua` — pure Lua JSON encoder/decoder
- `scripts/lib/json_repair.lua` — JSON repair utility for LLM responses

## Code Patterns

### Parsing CLI Arguments (main.cpp)

Arguments are checked in a series of `if (arg == "--flag")` blocks. Key config variables:
- `Config cfg` struct holds parsed settings
- Provider enums: `AIProvider`, `ImageProvider`
- Sensible defaults for all flags; `--help` prints reference

### Exposing C++ to Lua (main.cpp)

Uses sol2:
```cpp
lua.set_function("query_llm", [&](const std::string& sys, ...) { /* impl */ });
lua.set_function("get_embedding", [&](const std::string& text) { /* impl */ });
```

### Handling LLM Responses (main.cpp)

1. Call provider (with retry loop up to `--max-retries`)
2. Parse JSON response
3. Call Lua's `process_ai_response(reply)` for validation
4. If validation fails, retry LLM
5. If all retries exhausted, display error and continue
6. Otherwise, display narration and save

### RAG Pattern (main.cpp)

If `--rag <file>` enabled:
1. Load JSONL file of past turns at startup
2. Per turn: score examples by location/NPC/semantic similarity
3. Inject top-N into system prompt before LLM call
4. Lua can call `get_embedding(text)` if `--embed-model` configured

### Web Server (main.cpp + web_page.h)

Crow serves 9 routes:
- `GET /` — web UI (HTML/CSS/JS from memory)
- `GET /api/scripts` — list `.lua` files in `--path`
- `GET /api/saves` — list `.jsonl` files in `--save-path`
- `GET /api/status` — current HUD + snapshot
- `POST /api/start` — load script, show welcome
- `POST /api/init` — first player response
- `POST /api/load` — restore from save file
- `POST /api/chat` — normal turn
- `POST /api/command` — `/fix`, `/observe`, `/summary`, custom commands
- `POST /api/save` — manual save
- `POST /api/image` — async scene image generation (returns `job_id`)
- `GET /api/image/job/<id>` — poll image job status
- `POST /api/generate_asset` — generate/regenerate asset
- `GET /api/show_asset` — return asset as base64

All web routes share the Lua state with a `std::mutex` for thread safety.

### Scene Image Generation Pipeline (llm_image.h)

```
/image command
    ↓
get_scene_images() [Lua]
    → Format A: [{id, path}, ...]
    → Format B: { assets=[...], base_image="last"|"/path"|nil }
    ↓
Check disk: missing assets?
    ├─ yes: get_asset_prompt(id) [Lua]
    │       → text_to_image() → save to disk
    │
build_collage() → PNG bytes (background first, then NPCs side by side)
    ↓
Cache key check (script + sorted asset ids + max asset mtime)
    ├─ hit AND file on disk → return cached image immediately (skip generation)
    │
    └─ miss → resolve i2i source from base_image hint:
                  "last"  → scene_cache::lookup_last() finds the most recent
                            cached render with same asset set → use as i2i base
                  "/path" → load that file as i2i base
                  nil     → use collage as i2i base  (default)
                ↓
              LLM generates visual prompt from last narration + scene context
                ↓
              image_to_image(i2i_source, visual_prompt) → PNG bytes
                ↓
              scene_cache::upsert() → save to images/scene_cache/
                ↓
              Display in web UI
```

The **cache key** (script name + sorted asset IDs + max asset mtime) acts as the first gate:
if the key matches a file that still exists on disk, the image is returned instantly with no
generation. The `base_image` hint only comes into play on a cache miss and controls what the
i2i model receives as its starting image — the collage of static assets (default) or the
previous render of the same scene (`"last"`), allowing models like Qwen-Edit to refine rather
than fully regenerate.

The script decides which mode to use: typically `base_image="last"` when only the narration
changed (same location, same NPCs), and `nil` when the scene composition changed.

### Lua Script Pattern (fantasy_demo.lua)

Standard structure:
```lua
local json = require("json")

-- World data (locations, NPCs, items) — static
local locations = { ... }
local NPC = { ... }
local ITEMS = { ... }

-- Mutable state
local state = {}
local function default_state() return { ... } end

-- Initialization
function get_welcome_message() ... end
function set_initial_state(input) state.player.name = input; ... end

-- Per-turn
function get_status_for_ai() return json.encode(state) end
function get_system_prompt() return "..." end
function get_json_schema() return [[{ "type":"object", "required":[...], "properties":{...} }]] end
function process_ai_response(reply) ... return {...} end
function process_player_input(input) ... return {...} end
function get_display_state() return string.format(...) end

-- Persistence
function get_state_snapshot() return json.encode(state) end
function restore_state(snapshot) ... end

-- Optional: images
function get_scene_images() ... end
function get_asset_path(id) ... end
function get_asset_prompt(id) ... end
```

Separate `require("lib/...")`  for helper modules (JSON repair, data files).

## Modifying the Engine

### Adding a New LLM Provider

Edit `src/llm_query.h`:
1. Add enum value to `enum class AIProvider { ... }`
2. Implement a `xxx_query()` function matching the signature
3. Add case in `query_llm()` dispatcher
4. Update `parse_args()` in main.cpp to recognize `--provider xxx` and `--xxx-key`, `--xxx-model` flags

### Adding a New Image Provider

Edit `src/llm_image.h`:
1. Add enum value to `enum class ImageProvider { ... }`
2. Implement `text_to_image_xxx()` and/or `image_to_image_xxx()`
3. Update the dispatcher functions to call your implementation
4. Update `parse_args()` in main.cpp for new flags

### Adding a Web Route

Edit `src/main.cpp`:
1. Add route to Crow: `CROW_ROUTE(app, "/api/newroute").methods("POST"_method)([&](const crow::request& req){ ... })`
2. If it calls Lua, lock `lua_mutex` first
3. Remember to serialize Lua exceptions gracefully

### Modifying the Web UI

Edit `src/web_page.h`:
- Entire UI is C++ raw string literal (`std::string main_page = R"HTML(...)HTML"`)
- No build step — changes take effect immediately after recompile
- CSS is embedded; JavaScript is inline
- REST API surface is fixed (9 routes above)

## Testing

No formal test suite exists. Manual testing approaches:

1. **Console mode with local Ollama** (fast feedback loop)
   ```bash
   ./build/rpgai --provider ollama --model llama3.2 \
     --path scripts/ --script fantasy_demo.lua
   ```

2. **Web mode with demo script**
   ```bash
   ./build/rpgai --web \
     --provider openrouter --or-key sk-or-YOUR_KEY \
     --or-model anthropic/claude-3.5-sonnet \
     --path scripts/ --save-path saves/
   # Then open http://localhost:8080
   ```

3. **Lua script validation**
   - Load script, check for missing functions
   - Inspect `get_status_for_ai()` JSON output for well-formedness
   - Verify `process_ai_response()` handles both valid and invalid responses

4. **Image generation (if modified)**
   - Test asset generation with `/generate_asset <id>`
   - Test scene rendering with `/image`
   - Check cache hits with repeated `/image` calls
   - Verify cache invalidation when assets change

## Notes

- The project acknowledges that roughly 90% of the C++ and Lua code was written by Claude (Anthropic) through extended pair-programming. Architecture and direction by Massimo Bernava.
- Lua scripts are the primary extension point. Most game customization happens without touching C++.
- RAG examples are loaded once at startup; for a different corpus, restart the engine.
- Web mode can handle multiple concurrent requests (Crow is async), but they all share one Lua state protected by a mutex — no true multi-session yet.
- Image generation is always async in web mode; console mode blocks on the LLM call but image generation is non-blocking.
