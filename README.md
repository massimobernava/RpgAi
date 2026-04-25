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
- 🔧 **Hackable C++ core** — clean header-based architecture designed to be extended
- 🗂 **Smart image cache** — scene images cached by composition key; unchanged scenes reuse existing renders
- 🔁 **In-game commands** — `/fix`, `/observe`, `/summary`, `/image` and custom Lua commands in both console and web mode

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
│   └── lib/              # Shared Lua libraries (json, json_repair)
├── docs/                 # Extended documentation
├── vendor/               # Header-only dependencies (see docs/installation.md)
├── CMakeLists.txt
└── build.sh
```

### Core components

**`main.cpp`** — engine heart. CLI parsing, LuaJIT lifecycle, Crow web server (14 routes), console game loop, async image job system, RAG engine, session persistence.

**`llm_query.h`** — single abstraction for all text generation providers behind one function: `query_llm(provider, sys_prompt, history, user_prompt, json_schema, model)`. Supported providers: Ollama, OpenRouter, Gemini, OpenAI, Claude, any OpenAI-compatible endpoint.

**`llm_image.h`** — everything visual: asset collage builder, text-to-image, image-to-image, scene cache. Providers: stable-diffusion.cpp, OpenAI, OpenRouter, fal.ai, WaveSpeed, DashScope, AIMLAPI, Qwen local. t2i and i2i can use different providers independently.

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
- [ ] Voice output — multi-voice TTS with per-character voice cloning
  - Local inference via F5-TTS or Coqui XTTS v2
  - `--tts-provider` flag: `none` | `f5` | `xtts` | `openai` | `elevenlabs`
- [ ] Script hot-reload in web mode
- [ ] Issue templates for bug reports and feature requests

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
