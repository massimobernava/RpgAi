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

Whether you want to **recreate a classic D&D dungeon crawl**, build an **interactive mystery novel**, or design a **slice-of-life social simulator**, RpgAi gives you the tools without locking you into any particular story structure or AI provider.

![RpgAi web UI — scene image, narration and HUD](docs/screenshot.png)

---

## ✨ Features

**For players and storytellers**
- 🎲 **Any genre, any setting** — fantasy, sci-fi, horror, romance, historical. If you can describe it in Lua, the AI can narrate it
- 🧠 **Persistent world state** — NPCs move, relationships evolve, time passes. The game remembers everything
- 🌐 **Beautiful web UI** — play in your browser with a clean dark interface; no terminal required
- 🖼 **AI-generated scene images** — automatic scene illustration via image models (WaveSpeed, fal.ai, OpenRouter and more)
- 💾 **Save & load** — full session persistence with JSONL save files
- 📖 **RAG narrative style** — feed the engine example narrations to lock in the tone and prose style of your world

**For developers and world-builders**
- 🔌 **Multi-provider LLM support** — Ollama (local), OpenRouter, Gemini, OpenAI, Claude, or any OpenAI-compatible endpoint
- 🖊 **Lua scripting** — simple, readable scripts define everything: locations, NPCs, game rules, JSON schema, commands
- ⚡ **LuaJIT powered** — fast Lua execution with full access to the C++ engine via exposed functions
- 🔧 **Hackable C++ core** — clean header-based architecture (`llm_query.h`, `llm_image.h`, `web_page.h`) designed to be extended
- 🗂 **Smart image cache** — scene images are cached by composition key; unchanged scenes reuse existing renders
- 🔁 **In-game commands** — `/fix`, `/observe`, `/summary` and custom Lua commands work in both console and web mode

---

## 🎬 How it works

```
┌─────────────────────────────────────────────────────┐
│                    Your Lua Script                   │
│  locations · NPCs · rules · schema · commands        │
└───────────────────────┬─────────────────────────────┘
                        │  game state JSON
                        ▼
┌─────────────────────────────────────────────────────┐
│                   RpgAi C++ Engine                   │
│                                                      │
│  ┌──────────┐   ┌─────────────┐   ┌──────────────┐  │
│  │ Web UI   │   │  llm_query  │   │  llm_image   │  │
│  │ (Crow)   │◄──│  (providers)│   │  (scene gen) │  │
│  └──────────┘   └──────┬──────┘   └──────────────┘  │
└─────────────────────────┼───────────────────────────┘
                          │  prompt + history
                          ▼
              ┌───────────────────────┐
              │   LLM of your choice  │
              │  Ollama · OpenRouter  │
              │  Gemini · OpenAI · …  │
              └───────────────────────┘
                          │  narration JSON
                          ▼
              ┌───────────────────────┐
              │    process_ai_response│  ← back to Lua
              │    update world state │
              │    return narration   │
              └───────────────────────┘
```

1. **You write a Lua script** that defines your world: locations, NPCs with personalities and schedules, game rules, the JSON schema the LLM must follow
2. **The engine reads the state** each turn by calling `get_status_for_ai()` in your script
3. **The LLM narrates** the outcome of the player's action within the constraints of your schema
4. **Your script processes** the response via `process_ai_response()`, updating positions, relationships, flags and time
5. **The web UI or terminal** shows the narration and the updated HUD

---

## 🚀 Quick start

```bash
# 1. Clone the repo
git clone https://github.com/yourusername/RpgAi.git
cd RpgAi

# 2. Install system dependencies
#    macOS:
brew install luajit asio curl cmake

#    Ubuntu/Debian:
sudo apt install luajit libluajit-5.1-dev libasio-dev libcurl4-openssl-dev cmake build-essential

# 3. Place header-only dependencies in vendor/
#    (see Installation for details)

# 4. Build
./build.sh

# 5. Run with Ollama (local, no API key needed)
./build/rpgai --web --provider ollama --model llama3 --path scripts/
```

Then open **http://localhost:8080** in your browser, select a script, and start playing.

---

## 📜 License

MIT — see [LICENSE](LICENSE). Use it, fork it, build worlds with it.

---

## 🤝 Contributing

Pull requests are welcome — whether you're fixing a C++ bug, adding a new LLM provider, improving the web UI, or sharing a Lua adventure script. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

*Built with C++17, LuaJIT, sol2, Crow, nlohmann/json, libcurl and a lot of narrative ambition.*

---

## 🏗 Architecture

> **A note on authorship:** roughly 90% of the C++ and Lua code in this repository was written by [Claude](https://claude.ai) (Anthropic's AI assistant) through an extended pair-programming session. The architecture, design decisions, feature roadmap and Lua adventure scripts were directed by the human author; Claude handled the implementation. We think that's a pretty fitting way to build an AI storytelling engine.

---

### Repository layout

```
RpgAi/
├── src/
│   ├── main.cpp          # Engine core, CLI, web server, console loop
│   ├── llm_query.h       # LLM provider abstraction (Ollama, OpenRouter, Gemini…)
│   ├── llm_image.h       # Image generation, collage builder, scene cache
│   └── web_page.h        # Embedded single-page web UI (HTML/CSS/JS)
├── scripts/
│   ├── fantasy_demo.lua      # Demo adventure — classic fantasy
│   └── …                     # Your adventures go here
├── vendor/               # Header-only dependencies (see Installation)
├── cmake/
│   └── FindLuaJIT.cmake  # LuaJIT discovery module
├── CMakeLists.txt
└── build.sh
```

---

### Core components

#### `main.cpp` — The engine

The heart of RpgAi. It owns the main loop, the Lua state and the Crow web server. Its responsibilities are:

- **CLI parsing** — reads all `--provider`, `--img-*`, `--rag`, `--save-path` and other flags at startup
- **Lua lifecycle** — initialises LuaJIT via sol2, loads the game script, exposes C++ functions to Lua (`query_llm`, `get_embedding`, `cosine_similarity`)
- **Console loop** — the classic terminal game loop with readline-style input history, `/fix`, `/observe` and `/summary` built-in commands
- **Web server** — a full REST API served by Crow with nine routes, supporting multiple concurrent sessions via mutex-protected Lua state
- **Job system** — asynchronous image generation jobs (`POST /api/image` returns a `job_id` immediately; the client polls `GET /api/image/job/<id>`)
- **RAG engine** — loads a JSONL file of past narrations, scores them by location/NPC/semantic similarity and injects the top-N examples into the system prompt each turn
- **Session persistence** — two save modes: `LAST` (atomic rename, always one turn on disk) and `FULL` (append-only JSONL, suitable for RAG corpus building)

#### `llm_query.h` — LLM provider abstraction

A single header that abstracts all text generation providers behind one function:

```cpp
std::string query_llm(AIProvider provider,
                       const std::string& sys_prompt,
                       const std::vector<Message>& history,
                       const std::string& user_prompt,
                       const std::string& json_schema,
                       const std::string& model);
```

Supported providers:

| Provider | Flag | Notes |
|---|---|---|
| Ollama | `--provider ollama` | Local, no API key. Default |
| OpenRouter | `--provider openrouter` | Access to 200+ models |
| Gemini | `--provider gemini` | Google AI Studio |
| OpenAI | `--provider openai` | GPT-4o and compatible |
| Claude | `--provider claude` | Anthropic API |

Any OpenAI-compatible endpoint works with `--provider openai --oai-url https://your-endpoint`.

#### `llm_image.h` — Image generation

Handles everything visual: building reference collages from individual asset images, calling image-to-image models to render the final scene, and caching results to avoid redundant generation.

**Providers:**

| Provider | Flag | t2i | i2i |
|---|---|---|---|
| stable-diffusion.cpp | `sdcpp_local` | ✓ | ✓ async |
| OpenAI / DALL-E | `openai` | ✓ | ✓ multipart |
| OpenRouter | `openrouter` | ✓ | ✓ |
| fal.ai | `fal` | — | ✓ |
| WaveSpeed | `wavespeed` | — | ✓ async |
| DashScope | `dashscope` | ✓ | ✓ |
| AIMLAPI | `aimlapi` | ✓ | ✓ |

**Scene pipeline:**

```
get_scene_images()        ← Lua returns [{id, path}…]
        │
        ▼
Missing assets?  ──yes──► get_asset_prompt(id) → text_to_image() → save to disk
        │
        ▼
build_collage()           background first, then NPCs side by side
        │
        ▼
LLM generates visual prompt  (scene context + last narration + asset tags)
        │
        ▼
image_to_image()          collage → AI image model → scene image
        │
        ▼
scene_cache::upsert()     saved to images/scene_cache/ + cache_db.json
```

The cache key is computed from the script name, sorted asset IDs and the most recent asset modification timestamp — so cached images are reused as long as the composition and assets are unchanged.

#### `web_page.h` — Embedded web UI

The entire single-page application lives as a C++ raw string literal inside this header. No build step, no bundler, no static file serving — Crow serves it directly from memory at `GET /`.

**REST API surface:**

| Method | Route | Description |
|---|---|---|
| `GET` | `/` | Web UI |
| `GET` | `/api/scripts` | List `.lua` files in `--path` |
| `GET` | `/api/saves` | List `.jsonl` files in `--save-path` |
| `GET` | `/api/status` | Current display state + snapshot |
| `POST` | `/api/start` | Load script, show welcome |
| `POST` | `/api/init` | First player response (name/enter) |
| `POST` | `/api/load` | Restore session from save file |
| `POST` | `/api/chat` | Normal game turn |
| `POST` | `/api/command` | `/fix`, `/observe`, `/summary`, Lua commands |
| `POST` | `/api/save` | Manual save |
| `POST` | `/api/image` | Generate scene image (async, returns `job_id`) |
| `GET` | `/api/image/job/<id>` | Poll image job status |
| `POST` | `/api/generate_asset` | Generate/regenerate a single asset |
| `GET` | `/api/show_asset` | Return existing asset as base64 |

---

### Data flow — one game turn

```
Player input
     │
     ▼
process_player_input()    ← Lua: handles /commands, movement, inventory
     │
     ├── handled? ──yes──► cmd_handled = true  (state already updated)
     │
     ▼
get_status_for_ai()       ← Lua: serialises world state to JSON string
get_system_prompt()       ← Lua: builds the full system prompt
get_json_schema()         ← Lua: JSON schema the LLM must follow
     │
     ▼
[RAG injection]           top-N style examples added to system prompt
     │
     ▼
query_llm()               HTTP call to chosen provider
     │
     ▼
process_ai_response()     ← Lua: validates JSON, updates state, returns narration
     │
     ├── valid? ──no──► retry (up to --max-retries)
     │
     ▼
write_turn()              save to disk (LAST or FULL mode)
     │
     ▼
narration + updated HUD → player
```

---

### Design philosophy

**The engine is dumb; the script is smart.** RpgAi deliberately puts as little game logic as possible in C++. The C++ layer handles IO, HTTP, LLM calls and persistence. Everything that makes your adventure *yours* — the world model, the NPC personalities, the rules, the win conditions — lives in Lua where you can change it without recompiling.

**The LLM is a narrator, not a rules engine.** The JSON schema your Lua script defines constrains what the LLM can do. It cannot invent new locations, spawn NPCs that don't exist in your data, or ignore the time system — the schema and Lua validation enforce the rules. The LLM's job is to write compelling prose within those constraints.

**Fail gracefully.** Every LLM call is retried up to `--max-retries` times. Image generation is always async and non-blocking. Missing assets are generated on demand. The game never crashes because an API timed out.

---

## 📦 Installation & Build

### System dependencies

These must be installed via your package manager — they are not in `vendor/`.

**macOS (Homebrew)**
```bash
brew install cmake luajit asio curl
```

**Ubuntu / Debian**
```bash
sudo apt install cmake luajit libluajit-5.1-dev libasio-dev libcurl4-openssl-dev build-essential
```

**Fedora / RHEL**
```bash
sudo dnf install cmake luajit luajit-devel asio-devel libcurl-devel gcc-c++
```

---

### Header-only dependencies

These are small single-header (or single-file) libraries. Place them inside the `vendor/` directory at the project root. The build system will find them automatically.

| Library | What it does | Where to get it | Expected path in `vendor/` |
|---|---|---|---|
| **sol2** | Lua/C++ binding | [github.com/ThePhD/sol2](https://github.com/ThePhD/sol2/releases) | `vendor/sol/sol.hpp` |
| **Crow** | HTTP server | [github.com/CrowCpp/Crow](https://github.com/CrowCpp/Crow/releases) | `vendor/crow/crow_all.h` |
| **nlohmann/json** | JSON parsing | [github.com/nlohmann/json](https://github.com/nlohmann/json/releases) | `vendor/nlohmann/json.hpp` |
| **ollama-hpp** | Ollama client | [github.com/jmont-dev/ollama-hpp](https://github.com/jmont-dev/ollama-hpp) | `vendor/ollama/ollama.hpp` |
| **stb** | Image load/write/resize | [github.com/nothings/stb](https://github.com/nothings/stb) | `vendor/stb_image.h` + `stb_image_write.h` + `stb_image_resize2.h` |

After downloading, your `vendor/` directory should look like this:

```
vendor/
├── nlohmann/
│   └── json.hpp
├── sol/
│   └── sol.hpp
├── crow/
│   └── crow_all.h
├── ollama/
│   └── ollama.hpp
├── stb_image.h
├── stb_image_write.h
└── stb_image_resize2.h
```

> **Tip:** the `vendor/` directory is git-ignored. Each developer fetches their own copy of the headers. If you prefer to commit them, remove `vendor/` from `.gitignore`.

---

### Build

```bash
# Standard release build
./build.sh

# Debug build (with symbols, no optimisation)
./build.sh debug

# Clean rebuild (wipes build/ directory first)
./build.sh clean
```

The binary is placed at `build/rpgai`.

#### Custom header paths

If your headers live somewhere other than `vendor/` or standard system paths, pass them as environment variables:

```bash
SOL2_INCLUDE_DIR=/opt/mylibs ./build.sh
NLOHMANN_JSON_INCLUDE_DIR=/opt/mylibs ./build.sh
ASIO_INCLUDE_DIR=/usr/local/include/asio ./build.sh
```

#### Manual cmake

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

---

### Verifying the build

```bash
./build/rpgai --help
```

You should see the full CLI reference. If it runs, everything linked correctly.

---

### Troubleshooting

**`nlohmann/json not found` (or any other header)**
CMake caches `find_path` results. If you added a file after a failed cmake run, the cache still holds the old `NOTFOUND` value. Always do a clean rebuild:
```bash
./build.sh clean
```

**LuaJIT not found on Linux**
Some distros split the package into runtime and dev:
```bash
sudo apt install luajit libluajit-5.1-dev   # Debian/Ubuntu
sudo dnf install luajit luajit-devel        # Fedora
```
If cmake still can't find it, pass the path explicitly:
```bash
cmake .. -DLUAJIT_INCLUDE_DIR=/usr/include/luajit-2.1 \
         -DLUAJIT_LIBRARIES=/usr/lib/x86_64-linux-gnu/libluajit-5.1.so
```

**`asio.hpp not found`**
Asio must be the standalone version, not Boost.Asio:
```bash
brew install asio          # macOS
sudo apt install libasio-dev  # Ubuntu
```

**macOS: `framework not found IOKit`**
This means LuaJIT was not found and the framework flags are applied to nothing. Fix the LuaJIT path first.

**Crow / web server won't start**
Port 8080 may already be in use. Check with:
```bash
lsof -i :8080
```
A future release will support `--port` to change the port.

---

### Running your first game

**Console mode (terminal)**
```bash
./build/rpgai \
  --provider ollama \
  --model llama3.2 \
  --path scripts/ \
  --script fantasy_demo.lua
```

**Web mode (browser UI)**
```bash
./build/rpgai \
  --web \
  --provider openrouter \
  --or-key sk-or-YOUR_KEY \
  --or-model anthropic/claude-3-haiku \
  --path scripts/ \
  --save-path saves/
```
Then open **http://localhost:8080**, select a script and press **Start**.

**With local image generation (stable-diffusion.cpp)**
```bash
./build/rpgai \
  --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --img-provider sdcpp_local --img-url http://localhost:7860 \
  --path scripts/
```

**With cloud image generation (WaveSpeed i2i + OpenRouter t2i)**
```bash
./build/rpgai \
  --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --img-provider openrouter --img-key sk-or-YOUR_KEY \
  --img-t2i-model black-forest-labs/flux-1.1-pro \
  --img-i2i-provider wavespeed --img-i2i-key YOUR_WAVESPEED_KEY \
  --path scripts/ \
  --save-path saves/
```

---

## ⚙️ Configuration & CLI Reference

All configuration is passed via command-line flags. There are no config files — every option is explicit and scriptable.

```
./build/rpgai [OPTIONS]
```

---

### General options

| Flag | Default | Description |
|---|---|---|
| `--web` | off | Start the web server instead of the console loop |
| `--path <dir>` | `../scripts/` | Directory containing `.lua` game scripts |
| `--script <file>` | `forest_adventure.lua` | Script to load (console mode only; web mode selects via UI) |
| `--save <file>` | `session_log.jsonl` | Output file for session data |
| `--save-path <dir>` | *(cwd)* | Directory for save files (web mode) |
| `--save-mode <m>` | `last` | `last` = atomic overwrite each turn · `full` = append every turn |
| `--load <file>` | — | Restore a previous session from a `.jsonl` file (console mode) |
| `--max-history <n>` | `30` | Maximum messages kept in LLM context window |
| `--max-retries <n>` | `3` | Retries per turn if the LLM returns invalid JSON |
| `--lang <code>` | — | Language code for LLM responses (e.g. `it`, `fr`, `de`). Appends a "respond in X" instruction to every system prompt. Codes and phrases are defined in `lang.txt`. |
| `--lang-file <file>` | `lang.txt` | Path to the language instruction file |

**Save modes explained:**
- `last` — the save file always contains exactly one JSON line: the most recent turn. Fast, minimal disk use. Good for normal play.
- `full` — every turn is appended. The file grows indefinitely. Use this when you want to build a RAG corpus from your play sessions (`--rag`).

---

### LLM provider options

Exactly one provider is active at a time. Select it with `--provider`.

#### Ollama (default — local, no API key)
```bash
--provider ollama
--model <name>        # default: dolphin3:latest
--url <url>           # default: http://localhost:11434
```
Requires [Ollama](https://ollama.ai) running locally. Any model available via `ollama pull` works.

#### OpenRouter
```bash
--provider openrouter
--or-key <key>        # required — get one at openrouter.ai
--or-model <name>     # default: qwen/qwen3-32b
```
Gives access to 200+ models (Claude, GPT-4o, Gemini, Llama, Mistral, …) through a single API key. Recommended for cloud play.

#### Gemini
```bash
--provider gemini
--g-key <key>         # required — Google AI Studio key
--g-model <name>      # default: gemini-flash-latest
```

#### OpenAI
```bash
--provider openai
--oai-key <key>       # required
--oai-model <name>    # default: gpt-4o-mini
```
Also works with any OpenAI-compatible endpoint (LM Studio, vLLM, Groq, etc.):
```bash
--provider openai
--oai-key <key>
--oai-model <name>
--oai-url https://api.groq.com/openai/v1/chat/completions
```

#### Claude (Anthropic)
```bash
--provider claude
--claude-key <key>    # required
--claude-model <name> # default: claude-haiku-4-5-20251001
```

---

### Embedding options (for semantic RAG)

When `--embed-model` is set, RpgAi computes vector embeddings for RAG examples and uses cosine similarity instead of keyword matching — significantly improving example selection quality.

```bash
--embed-model <name>      # enables embedding (e.g. nomic-embed-text)
--embed-provider <name>   # ollama|openai  (default: follows --provider)
--embed-url <url>         # override base URL for embedding endpoint
--embed-key <key>         # API key if different from main provider
```

Example — use Ollama for text generation but a dedicated embedding model:
```bash
./build/rpgai \
  --provider openrouter --or-key sk-or-... \
  --embed-provider ollama --embed-model nomic-embed-text \
  --rag saves/my_session.jsonl
```

---

### RAG options

RAG (Retrieval-Augmented Generation) injects past narration examples into the system prompt to lock in prose style and tone. The examples are selected based on the current location, NPCs present and player input.

```bash
--rag <file>              # path to a JSONL file of past turns (built with --save-mode full)
--rag-examples <n>        # number of examples injected per turn  (default: 3)
```

Typical workflow:
1. Play a session with `--save-mode full` to build a corpus
2. Use that `.jsonl` as `--rag` in future sessions to maintain narrative consistency

---

### Image generation options

Image generation is split into two independent operations:
- **text-to-image (t2i)** — generates missing asset images from a text prompt
- **image-to-image (i2i)** — takes the reference collage and generates the final scene

You can use different providers for each.

#### Primary image provider (used for both t2i and i2i unless overridden)

```bash
--img-provider <name>     # sdcpp_local | openai | openrouter | fal | wavespeed | dashscope | aimlapi
--img-url <url>           # base URL (for local servers)
--img-key <key>           # API key (defaults to main provider key if same service)
--img-t2i-model <name>    # model for text-to-image
--img-i2i-model <name>    # model for image-to-image
--img-width <n>           # output image width   (default: 1024)
--img-height <n>          # output image height  (default: 1024)
--img-steps <n>           # sampling steps       (default: 28)
--img-strength <f>        # i2i denoising strength 0.0–1.0 (default: 0.75)
```

#### Separate i2i provider (optional)

If you want to use one service for asset generation and a different one for scene rendering:

```bash
--img-i2i-provider <name>   # overrides --img-provider for i2i only
--img-i2i-url <url>
--img-i2i-key <key>
```

#### Provider quick reference

| Provider string | t2i | i2i | Notes |
|---|---|---|---|
| `sdcpp_local` | ✓ | ✓ | stable-diffusion.cpp server. i2i is async with native polling |
| `openai` | ✓ | ✓ | DALL-E 3 / GPT-image. i2i via multipart `/v1/images/edits` |
| `openrouter` | ✓ | ✓ | Routes to FLUX, SD and other image models |
| `fal` | — | ✓ | fal.ai. Fast, high quality. Requires fal.ai account |
| `wavespeed` | — | ✓ | WaveSpeed Qwen2.5-VL edit. Async polling. ~$0.03/image |
| `dashscope` | ✓ | ✓ | Alibaba Cloud. Good for Asian-style art |
| `aimlapi` | ✓ | ✓ | AIML API gateway |

#### Example configurations

**Fully local (stable-diffusion.cpp)**
```bash
--img-provider sdcpp_local --img-url http://localhost:7860
```

**OpenRouter for t2i, WaveSpeed for i2i (recommended cloud setup)**
```bash
--img-provider openrouter \
--img-key sk-or-YOUR_KEY \
--img-t2i-model black-forest-labs/flux-1.1-pro \
--img-i2i-provider wavespeed \
--img-i2i-key YOUR_WAVESPEED_KEY
```

**fal.ai for everything**
```bash
--img-provider fal --img-key YOUR_FAL_KEY \
--img-i2i-model fal-ai/flux/dev/image-to-image
```

---

### In-game commands

These commands work in both console mode and web mode (type them in the input box).

| Command | Description |
|---|---|
| `/save` | Save the current session to disk immediately |
| `/status` | Print the raw JSON game state |
| `/summary [N]` | Generate a narrative summary of history and compress it. `N` = recent turns to keep (default: 2) |
| `/fix <instruction>` | Ask the LLM to rewrite the last scene with a correction. Time does not advance. |
| `/observe [subject]` | Get a detailed sensory description of the scene or a specific element, without advancing time |
| `/image` | Generate an AI image of the current scene |
| `/image --partial` | Generate scene image even if some assets are missing |
| `/generate_asset <id>` | Generate or regenerate a specific asset image by ID |
| `/show_asset <id>` | Display an existing asset image |
| `/quit` · `/q` | Exit (console mode only) |
| `/help` | Show CLI reference (console mode only) |
| *any other `/xxx`* | Delegated to your Lua script's `process_player_input()` |

---

### Full example — production setup

```bash
./build/rpgai \
  --web \
  --provider openrouter \
  --or-key sk-or-YOUR_KEY \
  --or-model anthropic/claude-3.5-sonnet \
  --path ./scripts/ \
  --save-path ./saves/ \
  --save-mode full \
  --max-history 40 \
  --max-retries 3 \
  --rag ./saves/my_corpus.jsonl \
  --rag-examples 3 \
  --embed-provider ollama \
  --embed-model nomic-embed-text \
  --img-provider openrouter \
  --img-key sk-or-YOUR_KEY \
  --img-t2i-model black-forest-labs/flux-1.1-pro \
  --img-i2i-provider wavespeed \
  --img-i2i-key YOUR_WAVESPEED_KEY \
  --img-width 1024 \
  --img-height 768
```

---

## 📝 Writing a Lua Script

This is where the magic happens. A RpgAi adventure is a single `.lua` file (or a main file that `require`s others) that defines your entire game world. The engine calls specific functions in your script at specific moments — you implement them, the engine handles the rest.

---

### Minimal script structure

A working script needs at minimum these functions:

```lua
-- Called once: returns the welcome/intro text shown before the game starts
function get_welcome_message()
    return "You stand at the entrance of the dungeon. What is your name, adventurer?"
end

-- Called once after the player's first response: sets up the initial world state
function set_initial_state(player_input)
    state.player.name     = player_input
    state.player.location = "dungeon_entrance"
    state.turn            = 1
end

-- Alternative to set_initial_state: called if the player pressed Enter with no input
function generate_initial_state()
    state.player.name     = "Adventurer"
    state.player.location = "dungeon_entrance"
    state.turn            = 1
end

-- Called every turn: returns the world state as a JSON string for the LLM
function get_status_for_ai()
    return json.encode(state)
end

-- Called every turn: returns the system prompt that governs the LLM's behaviour
function get_system_prompt()
    return "You are the dungeon master of a classic fantasy RPG. " ..
           "Narrate the outcome of the player's actions. " ..
           "Follow the JSON schema exactly."
end

-- Called every turn: returns the JSON schema the LLM must follow
function get_json_schema()
    return json.encode({
        narration        = "string — what happens, 3-5 sentences",
        new_location     = "string or null — if the player moved",
        game_over        = "boolean",
        game_over_reason = "string or null"
    })
end

-- Called every turn: receives the LLM's JSON response, updates state, returns result
function process_ai_response(reply)
    local ok, data = pcall(json.decode, reply)
    if not ok then
        return { success=false, error="Invalid JSON: " .. tostring(data) }
    end

    -- Update world state from LLM response
    if data.new_location then
        state.player.location = data.new_location
    end
    state.turn = state.turn + 1

    return {
        success          = true,
        narration        = data.narration,
        game_over        = data.game_over == true,
        game_over_reason = data.game_over_reason or ""
    }
end

-- Called every turn: returns the HUD text shown above the chat
function get_display_state()
    return string.format("[ %s ]  Turn: %d",
        state.player.location, state.turn)
end

-- Called for save/load: returns the full state as a JSON string
function get_state_snapshot()
    return json.encode(state)
end

-- Called on load: restores state from a snapshot string
function restore_state(snapshot)
    local ok, data = pcall(json.decode, snapshot)
    if not ok then return { success=false, error="Parse error" } end
    state = data
    return { success=true }
end

-- Called every turn: handles player input before the LLM.
-- Return handled=true to skip the LLM call (for pure commands).
-- Return handled=false to let the turn proceed normally.
function process_player_input(input)
    return { success=true, handled=false }
end
```

---

### Required functions — reference

| Function | When called | Must return |
|---|---|---|
| `get_welcome_message()` | Once, on start | `string` — intro text |
| `set_initial_state(input)` | Once, after player's first response | nothing |
| `generate_initial_state()` | Once, if player pressed Enter with no input | nothing |
| `get_status_for_ai()` | Every turn, before LLM call | `string` — JSON of world state |
| `get_system_prompt()` | Every turn, before LLM call | `string` — system prompt |
| `get_json_schema()` | Every turn, before LLM call | `string` — JSON schema |
| `process_ai_response(reply)` | Every turn, after LLM responds | `table` — see below |
| `get_display_state()` | Every turn, after processing | `string` — HUD text |
| `get_state_snapshot()` | On save | `string` — full state JSON |
| `restore_state(snapshot)` | On load | `table {success, error?}` |
| `process_player_input(input)` | Every turn, before LLM | `table` — see below |

**`process_ai_response` return table:**
```lua
{
    success          = true,           -- false if JSON was invalid
    error            = "...",          -- only if success=false
    narration        = "string",       -- text shown to the player
    game_over        = false,          -- true ends the game
    game_over_reason = "string or nil" -- shown on game over
}
```

**`process_player_input` return table:**
```lua
{
    success = true,
    handled = false,   -- true = LLM is skipped this turn
    output  = "..."    -- optional text to show if handled=true
}
```

---

### Optional functions — image system

Implement these three functions to enable AI scene illustration. If they are not present, `/image` will report that the script does not support images.

```lua
-- Returns a list of assets for the current scene.
-- Order matters: background first, then characters.
function get_scene_images()
    local loc = state.player.location
    local result = {}

    -- Background
    if ASSET_PATHS[loc] then
        table.insert(result, { id=loc, path=ASSET_PATHS[loc] })
    end

    -- NPCs present in this location
    for npc_id, npc_loc in pairs(state.npc_locations) do
        if npc_loc == loc and ASSET_PATHS[npc_id] then
            table.insert(result, { id=npc_id, path=ASSET_PATHS[npc_id] })
        end
    end

    return result
end

-- Returns the file path for a given asset id.
-- Used by /show_asset to display an existing image.
function get_asset_path(id)
    return ASSET_PATHS[id]  -- nil if unknown
end

-- Returns {path, prompt} for generating a missing asset via text-to-image.
-- The prompt should be a detailed English description suitable for an image model.
function get_asset_prompt(id)
    local path = get_asset_path(id)
    if not path then return nil end

    -- Use LLM to build a rich txt2img prompt from your raw description
    local desc = ASSET_DESCRIPTIONS[id]
    if not desc then return nil end

    local ok, prompt = pcall(query_llm,
        "You are a professional image prompt engineer. " ..
        "Convert this description into a detailed txt2img prompt. " ..
        "Include subject, pose, lighting, style, quality. Max 80 words. English only.",
        "[]",
        "Description: " .. desc,
        "")

    return {
        path   = path,
        prompt = ok and prompt or desc  -- fallback to raw description
    }
end
```

---

### C++ functions exposed to Lua

The engine makes these functions available inside every Lua script:

#### `query_llm(sys, history_json, user, schema) → string`

Call the active LLM directly from Lua. Useful for generating NPC dialogue, dream sequences, item descriptions or anything that needs AI text generation outside the main turn loop.

```lua
local reply = query_llm(
    "You are a wise old oracle. Speak in riddles.",
    "[]",   -- empty history
    "The player asks: " .. player_question,
    ""      -- no schema, free-form response
)
```

`history_json` must be a JSON array string. Use `"[]"` for no history, or serialise a subset of your chat history if you want context.

#### `get_embedding(text) → table | nil`

Returns a vector of floats (a Lua table with numeric keys) representing the semantic embedding of `text`. Returns `nil` if no embedding model is configured (`--embed-model`).

```lua
local vec = get_embedding("the player enters the tavern")
if vec then
    -- use for semantic search, classification, etc.
end
```

#### `cosine_similarity(vec_a, vec_b) → float`

Computes cosine similarity between two embedding vectors. Returns a value between 0 (unrelated) and 1 (identical meaning).

```lua
local sim = cosine_similarity(
    get_embedding("fire spell"),
    get_embedding("flame magic")
)
-- sim ≈ 0.95
```

---

### Designing the JSON schema

The schema you return from `get_json_schema()` is injected into the system prompt and constrains what the LLM can produce. Think of it as a contract between your script and the AI.

**Tips:**
- Include a comment string for each field explaining what it should contain — the LLM reads these
- Keep it as small as possible; every unused field wastes tokens and introduces hallucination risk
- Use `null`-able fields for things that don't happen every turn (movement, NPC actions, item changes)
- Add an `avanza_tempo` integer field if your world tracks time — it gives the LLM explicit control over how much time passes

```lua
function get_json_schema()
    return json.encode({
        narration        = "string, 3-5 sentences, second person",
        new_location     = "string location_id or null if player did not move",
        avanza_tempo     = "integer minutes 0-60, 0 if no time passed",
        cambia_relazioni = "object {npc_id: {field: delta}} or null",
        genera_npc       = "boolean, true only in crowded public spaces",
        game_over        = "boolean",
        game_over_reason = "string or null"
    })
end
```

---

### NPC scheduling pattern

A common pattern for making the world feel alive — NPCs follow routines independently of the player:

```lua
local NPC_ROUTINES = {
    guard = {
        mattina = "gate",
        pomeriggio = "barracks",
        sera = "tavern",
        notte = "gate",
    },
    merchant = {
        mattina = "market",
        pomeriggio = "market",
        sera = "home",
        notte = "home",
    }
}

local function advance_npc_routines()
    local ora = state.time_of_day  -- "mattina", "pomeriggio", etc.
    for npc_id, routine in pairs(NPC_ROUTINES) do
        if routine[ora] then
            state.npc_locations[npc_id] = routine[ora]
        end
    end
end
```

Call `advance_npc_routines()` inside `process_ai_response()` whenever `avanza_tempo > 0`.

---

### Custom slash commands

Any `/command` not handled by the C++ engine is passed to `process_player_input()`. This is how you add game-specific commands:

```lua
function process_player_input(input)
    local cmd = input:lower():match("^(/[%w_]+)")

    if cmd == "/inventory" then
        local lines = {}
        for _, item in ipairs(state.player.inventory) do
            table.insert(lines, "• " .. item)
        end
        return {
            success = true,
            handled = true,
            output  = "Inventory:\n" .. table.concat(lines, "\n")
        }
    end

    if cmd == "/map" then
        return {
            success = true,
            handled = true,
            output  = build_map_string()
        }
    end

    -- Unknown command — let the LLM interpret it
    return { success=true, handled=false }
end
```

---

### Best practices

**Keep state serialisable.** Everything in `state` must be JSON-serialisable (no functions, no userdata). Use flat tables and primitive values.

**Validate LLM output defensively.** The LLM will occasionally produce unexpected field values. Always use `or` fallbacks and check types before using a field to update state.

**Use `pcall` around `query_llm` calls.** Network calls can fail. Wrap any direct LLM call in `pcall` and handle the error gracefully.

**Separate data from logic.** Put your world data (locations, NPCs, items) in a `require`d file (`estate_dataX.lua`, `dungeon_data.lua`). Keep the main script focused on game logic.

**Build the system prompt dynamically.** `get_system_prompt()` is called fresh every turn. Use this to inject current NPC states, active quests or time-of-day modifiers that should influence the LLM's narration style.

```lua
function get_system_prompt()
    local base = BASE_SYSTEM_PROMPT
    if state.time_of_day == "night" then
        base = base .. "\n\nIt is deep night. Describe sounds and shadows more than sights."
    end
    if #get_present_npcs() == 0 then
        base = base .. "\n\nThe player is alone. Emphasise atmosphere and internal thoughts."
    end
    return base
end
```

---

## 🖼 Image System

RpgAi can illustrate your adventure automatically. Each time the player types `/image`, the engine asks your Lua script which images make up the current scene, builds a reference collage, generates a visual prompt with the LLM, and sends everything to an image-to-image model to produce a coherent scene illustration.

The system is entirely optional — scripts that don't implement the image functions work perfectly without it.

---

### How a scene is rendered

```
/image command
      │
      ▼
get_scene_images()          ← your Lua script
      │
      │  [{id="dungeon_entrance", path="images/bg_entrance.jpg"},
      │   {id="goblin_guard",     path="images/npc_goblin.jpg"}]
      ▼
Check each path on disk
      │
      ├── missing? ──► get_asset_prompt(id)  ← your Lua script
      │                      │
      │                      ▼
      │               LLM generates txt2img prompt
      │                      │
      │                      ▼
      │               text_to_image()  →  save to disk
      │
      ▼
build_collage()             background first, NPCs side by side
      │
      ▼
LLM generates visual prompt
  "Dimly lit dungeon entrance, stone archway dripping with moisture.
   A hunched goblin guard leans against the wall, clutching a rusty spear.
   Torchlight flickers from within. Cinematic, dramatic shadows."
      │
      ▼
image_to_image(collage, prompt)   →  scene image
      │
      ▼
scene_cache::upsert()       saved to images/scene_cache/
      │
      ▼
Image displayed in web UI
```

---

### Directory layout

The image system expects this structure under your script's base directory:

```
scripts/
├── my_adventure.lua
└── images/
    ├── my_adventure/          ← your asset images (backgrounds + NPCs)
    │   ├── bg_dungeon.jpg
    │   ├── bg_tavern.jpg
    │   ├── npc_innkeeper.jpg
    │   └── npc_goblin.jpg
    ├── scene_cache/           ← auto-created, final rendered scenes
    │   ├── cache_db.json
    │   └── 20250421_153012_scene.jpg
    └── collage_tmp/           ← auto-created, intermediate collages
        └── 20250421_153012_collage.png
```

You provide the assets in `images/my_adventure/`. Everything under `scene_cache/` and `collage_tmp/` is managed automatically.

---

### Asset images

Assets are static images you prepare in advance (or let the engine generate on first run). They come in two types:

**Backgrounds** — the location. Typically a wide establishing shot of the room, street or landscape. Aspect ratios around 16:9 work best for the collage.

**Characters** — NPCs and the player character. Best as half-body or full-body portraits on a neutral background. The engine places them alongside the background in the collage.

#### Letting the engine generate assets

If an asset file is missing when `/image` is called, the engine automatically calls `get_asset_prompt(id)` to get a generation prompt, runs text-to-image and saves the result. The file is then reused for all future scenes in that location.

You can also trigger generation manually:
```
/generate_asset dungeon_entrance
/generate_asset goblin_guard
```

And view any existing asset:
```
/show_asset innkeeper
```

---

### Implementing the three image functions

#### `get_scene_images()` — what's in the scene

```lua
local IMAGE_DIR = "images/my_adventure/"

-- Map every asset id to its file path
local ASSET_PATHS = {
    -- Backgrounds (location id → image)
    dungeon_entrance = IMAGE_DIR .. "bg_entrance.jpg",
    dungeon_corridor = IMAGE_DIR .. "bg_corridor.jpg",
    tavern           = IMAGE_DIR .. "bg_tavern.jpg",
    -- NPCs (npc id → image)
    innkeeper        = IMAGE_DIR .. "npc_innkeeper.jpg",
    goblin_guard     = IMAGE_DIR .. "npc_goblin.jpg",
    wizard           = IMAGE_DIR .. "npc_wizard.jpg",
}

function get_scene_images()
    local result = {}
    local loc = state.player.location

    -- 1. Background for current location (always first)
    if ASSET_PATHS[loc] then
        table.insert(result, { id=loc, path=ASSET_PATHS[loc] })
    end

    -- 2. NPCs present in this location
    for npc_id, npc_loc in pairs(state.npc_locations or {}) do
        if npc_loc == loc and ASSET_PATHS[npc_id] then
            table.insert(result, { id=npc_id, path=ASSET_PATHS[npc_id] })
        end
    end

    -- 3. Dynamically spawned NPCs (if your script generates them at runtime)
    for npc_id, entry in pairs(state.npc_dinamici or {}) do
        if entry.location == loc then
            local dyn_path = IMAGE_DIR .. "npc_dyn_" .. npc_id .. ".jpg"
            table.insert(result, { id=npc_id, path=dyn_path })
        end
    end

    return result
end
```

#### `get_asset_path(id)` — path lookup

```lua
function get_asset_path(id)
    return ASSET_PATHS[id]  -- returns nil for unknown ids
end
```

#### `get_asset_prompt(id)` — generation prompt

This is where you describe each asset to the image model. Use the LLM to convert a rough description into a polished txt2img prompt:

```lua
local IMAGE_STYLE = "fantasy illustration, dramatic lighting, detailed, " ..
                    "painterly style, 8k quality"

local ASSET_DESCRIPTIONS = {
    dungeon_entrance = {
        tipo = "background",
        desc = "Stone archway entrance to a dark dungeon, iron portcullis half-raised, " ..
               "torchlight from within, moisture dripping from ceiling, wide establishing shot"
    },
    innkeeper = {
        tipo = "character",
        desc = "Stout middle-aged innkeeper, brown apron, friendly smile, " ..
               "holding a tankard, half-body portrait, front-facing, neutral background"
    },
    goblin_guard = {
        tipo = "character",
        desc = "Small green-skinned goblin guard, rusty spear, leather armour, " ..
               "suspicious expression, half-body portrait, front-facing, neutral background"
    },
}

function get_asset_prompt(id)
    local path = get_asset_path(id)
    if not path then return nil end

    local d = ASSET_DESCRIPTIONS[id]
    if not d then return { path=path, prompt=IMAGE_STYLE } end

    -- Ask the LLM to build a proper txt2img prompt from the description
    local sys = "You are a professional image generation prompt engineer. " ..
                "Convert the description into a detailed txt2img prompt. " ..
                "Include: subject, composition, lighting, style, quality tags. " ..
                "Max 80 words. English only. No JSON."

    local ok, prompt = pcall(query_llm, sys, "[]",
        "Description: " .. d.desc .. "\nStyle: " .. IMAGE_STYLE, "")

    return {
        path   = path,
        prompt = ok and prompt or (d.desc .. ", " .. IMAGE_STYLE)
    }
end
```

---

### Scene cache

The engine caches scene images to avoid regenerating identical scenes. The cache key is computed from:

- The script filename
- The sorted list of asset IDs in the scene
- The most recent file modification timestamp among those assets

This means:
- **Same scene, same assets, no asset changes** → cached image is reused instantly
- **Same scene but assets were regenerated** → cache invalidated, new scene rendered
- **Different NPCs in the same room** → different cache key, new scene rendered

The cache database lives at `images/scene_cache/cache_db.json` and contains one entry per cached scene with the key, asset list, prompt used and generation timestamp.

To clear the cache, simply delete `images/scene_cache/`.

---

### Image generation tips

**Background first, always.** The collage builder places images left to right in the order returned by `get_scene_images()`. The background should always be the first entry — it sets the spatial context that the i2i model uses to place characters.

**Keep asset images consistent.** Use the same photographic style, lighting direction and colour palette for all assets in an adventure. Mismatched styles are the main source of jarring scene images.

**Neutral backgrounds for character portraits.** NPCs should be photographed/generated against a plain grey or white background. The i2i model will place them into the scene environment naturally.

**Use `--img-strength` to control creativity.** Lower values (0.5–0.6) keep the scene very close to the collage — good when your assets are already high quality. Higher values (0.75–0.9) give the model more freedom to reinterpret — better when assets are rough or mismatched.

**Generate assets before playing.** Run `/generate_asset <id>` for every location and NPC before your first session. This way `/image` during play is instant (cache hit or collage+i2i only, no t2i delay).

**`/image --partial`** generates the scene even if some assets are missing, using only the available images. Useful for testing before all assets are ready.

---

### Supported image providers — details

#### WaveSpeed (`wavespeed`) — recommended for i2i
Async polling API. Submit → get prediction ID → poll until done. Fast and cheap (~$0.03/image). Excellent for Qwen2.5-VL based editing.
```bash
--img-i2i-provider wavespeed
--img-i2i-key YOUR_KEY
# no --img-i2i-model needed, endpoint is fixed
```

#### fal.ai (`fal`) — recommended for i2i
High quality, fast. Supports FLUX and other leading models.
```bash
--img-i2i-provider fal
--img-i2i-key YOUR_FAL_KEY
--img-i2i-model fal-ai/flux/dev/image-to-image
```

#### stable-diffusion.cpp (`sdcpp_local`) — local, free
Run your own image server locally. Both t2i and i2i supported. i2i uses the native async job API.
```bash
--img-provider sdcpp_local
--img-url http://localhost:7860
--img-t2i-model your_checkpoint_name
--img-i2i-model your_checkpoint_name
```

#### OpenRouter (`openrouter`) — for t2i
Access to FLUX 1.1 Pro, SDXL and others via your existing OpenRouter key.
```bash
--img-provider openrouter
--img-key sk-or-YOUR_KEY
--img-t2i-model black-forest-labs/flux-1.1-pro
```

---

## 🎲 Demo Scripts

RpgAi ships with two ready-to-play adventure scripts designed to showcase the engine's capabilities and serve as starting templates for your own worlds.

---

### `fantasy_demo.lua` — The Tomb of the Forgotten King

*A classic fantasy dungeon crawl. Torchlight, traps, monsters and treasure.*

You are a lone adventurer who has discovered the entrance to an ancient tomb. Legends speak of a forgotten king buried with his legendary sword — and of the curse that protects it. Navigate crumbling corridors, outwit (or fight) the undead guardians, find the artefacts that break the curse, and claim the blade before the tomb seals itself forever.

**Features demonstrated:**
- Multi-room dungeon with connectable locations
- NPC guardians with patrol routines (skeleton, ghost, stone golem)
- Inventory system with key items
- Time pressure mechanic (the tomb seals at dawn)
- Win/lose conditions
- Full image system support with fantasy art style

**Recommended model:** any capable model works. For best narration quality try `anthropic/claude-3.5-sonnet` or `qwen/qwen3-32b` via OpenRouter.

```bash
# Console mode — local Ollama
./build/rpgai \
  --provider ollama --model llama3.2 \
  --path scripts/ --script fantasy_demo.lua

# Web mode — OpenRouter
./build/rpgai --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --or-model anthropic/claude-3.5-sonnet \
  --path scripts/ --save-path saves/
```

---

### Playing in a different language — `--lang`

The demo script is written in English, but the LLM can narrate in any language without rewriting the script. Pass `--lang <code>` and the engine appends a "respond always in X" instruction to every system prompt automatically.

```bash
# Play The Tomb of the Forgotten King narrated in Italian
./build/rpgai --web \
  --provider openrouter --or-key sk-or-YOUR_KEY \
  --or-model qwen/qwen3-32b \
  --path scripts/ --save-path saves/ \
  --lang it
```

Language codes and their injected phrases are defined in `lang.txt` at the repo root. Seven languages ship out of the box (`it`, `fr`, `de`, `es`, `pt`, `ja`, `zh`); adding a new one is a single line in that file.

Scripts can also read the `LANG` global (set by the engine before any Lua call) to localise welcome messages or other player-facing strings:

```lua
function get_welcome_message()
    if LANG == "it" then return "Bentornato, avventuriero..." end
    return "Welcome, adventurer..."
end
```

---

### Using the demos as templates

Both scripts are heavily commented. They are designed to be read alongside this documentation — every pattern described in the *Writing a Lua Script* section appears in the demos with an explanation of why it is done that way.

To start your own adventure:

1. Copy `fantasy_demo.lua` to a new file (e.g. `my_world.lua`)
2. Replace the location definitions, NPC roster and system prompt with your world
3. Adjust the JSON schema to match the game mechanics you want
4. Run it — the engine handles the rest

---

### Community scripts

Have you written an adventure you want to share? Open a pull request adding your `.lua` file to `scripts/community/`. Include a short description at the top of the file (title, genre, recommended model, author).

We are particularly looking for:
- 🏙 **Urban / noir** — city investigations, social intrigue
- 🚀 **Sci-fi** — space exploration, cyberpunk, post-apocalyptic
- 🧪 **Experimental** — non-linear narratives, puzzle-heavy, comedy
- 🌍 **Non-English** — adventures in any language
- 📚 **Literary** — adaptations of public-domain settings

---

## 🗺 Roadmap

Things we want to build next — contributions welcome:

- [ ] `--port` flag to configure the web server port
- [ ] Multi-session web mode (multiple simultaneous players)
- [ ] Two-level scene cache (hard key + soft narrative key for smart i2i reuse)
- [ ] WebSocket streaming for real-time narration display
- [ ] Voice input/output integration
- [ ] Script hot-reload in web mode (edit Lua without restarting)
- [ ] Issue templates for bug reports and feature requests

---

## 📄 License

MIT License — see [LICENSE](LICENSE).

```
Copyright (c) 2025 Massimo Bernava

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
```

---

*Built with C++17 · LuaJIT · sol2 · Crow · nlohmann/json · libcurl · stb · ollama-hpp*

*~90% of the code written by [Claude](https://claude.ai) (Anthropic) · Architecture and direction by Massimo Bernava*
