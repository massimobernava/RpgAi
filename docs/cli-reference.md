# CLI Reference

All configuration is passed via command-line flags. No config files — every option is explicit and scriptable.

```
./build/rpgai [OPTIONS]
```

---

## General options

| Flag | Default | Description |
|---|---|---|
| `--web` | off | Start the web server instead of the console loop |
| `--path <dir>` | `../scripts/` | Directory containing `.lua` game scripts |
| `--script <file>` | `forest_adventure.lua` | Script to load (console mode only) |
| `--save <file>` | `session_log.jsonl` | Output file for session data |
| `--save-path <dir>` | *(cwd)* | Directory for save files (web mode) |
| `--save-mode <m>` | `last` | `last` = atomic overwrite · `full` = append every turn |
| `--load <file>` | — | Restore a previous session (console mode) |
| `--max-history <n>` | `30` | Maximum messages kept in LLM context window |
| `--max-retries <n>` | `3` | Retries per turn if the LLM returns invalid JSON |
| `--lang <code>` | — | Language code for LLM responses (`it`, `fr`, `de`, …). Appends a "respond in X" instruction to every system prompt. |
| `--lang-file <file>` | `lang.txt` | Path to the language instruction file |
| `--asset-root <path>` | *(cwd)* | Base dir for `asset/` & `catalog/` — lets `--path` point elsewhere while assets stay absolute |
| `--help` | — | Print this reference and exit |

**Save modes:**
- `last` — always exactly one line: the most recent turn. Fast, minimal disk use.
- `full` — every turn appended. Use this to build a RAG corpus (`--rag`).

---

## Web server options

| Flag | Default | Description |
|---|---|---|
| `--web` | off | REST server (Crow) + auto-open the browser UI |
| `--rest` | off | Same REST server, headless — no browser auto-open. Pair with a native client (e.g. `rpgai-gui`) instead of the browser |
| `--port <n>` | `8080` | Web server port |
| `--llm-timeout <s>` | `120` | Per-request LLM timeout (seconds) |
| `--max-output-tokens <n>` | `1024` | Cap completion tokens per LLM call (`0` = provider default). Without a cap a degenerating model can stream to the provider's max — minutes-long hangs, "void" output, and a large token bill |
| `--debug-gui` | off | Enable GUI debug routes (NPC possess/action injection) |

`--web` and `--rest` expose the identical `/api/*` surface; only browser auto-open differs.

---

## LLM provider options

#### Ollama (default — local, no API key)
```bash
--provider ollama
--model <name>        # default: dolphin3:latest
--url <url>           # default: http://localhost:11434
```

#### OpenRouter
```bash
--provider openrouter
--or-key <key>        # required
--or-model <name>     # default: qwen/qwen3-32b
```

#### Gemini
```bash
--provider gemini
--g-key <key>
--g-model <name>      # default: gemini-flash-latest
```

#### OpenAI (and compatible endpoints)
```bash
--provider openai
--oai-key <key>
--oai-model <name>    # default: gpt-4o-mini
--oai-url <url>       # override base URL (LM Studio, vLLM, Groq, …)
```

#### Claude (Anthropic)
```bash
--provider claude
--claude-key <key>
--claude-model <name> # default: claude-haiku-4-5-20251001
```

---

## Embedding options (semantic RAG)

```bash
--embed-model <name>      # enables embedding (e.g. nomic-embed-text)
--embed-provider <name>   # ollama|openai  (default: follows --provider)
--embed-url <url>
--embed-key <key>
```

Example — Ollama embedding with OpenRouter for text generation:
```bash
--provider openrouter --or-key sk-or-... \
--embed-provider ollama --embed-model nomic-embed-text \
--rag saves/my_session.jsonl
```

---

## Model tier options

Give different roles a different model/provider — e.g. a strong model for one-off entity generation, a cheap one for the high-volume roles (NPC agents fire on nearly every turn; ambient events fire off-screen). Any tier left unset falls back to the main `--provider`/`--model`.

```bash
--gen-model <m>          --gen-provider <p>       # entity generation (persona/world) — strong model
--agent-model <m>        --agent-provider <p>     # NPC agents (think_as_npc) — cheap model
--ambient-model <m>      --ambient-provider <p>   # NPC↔NPC ambient events — cheapest model
```

Also editable live from the web Settings panel (no restart needed). See `/cost` in-game (or `get_token_usage()`) to see where tokens actually go before picking tiers.

---

## CoderAI options

In-browser coding assistant (see [CoderAI](../CODERAI.md)). Requires `--coder-provider` to be `ollama`, `openrouter` or `openai` (tool-calling wire format) — route Claude/Gemini through OpenRouter.

```bash
--coder-provider <n>            # ollama|openrouter|openai (default: inherits --provider, if compatible)
--coder-model <n>               # default: inherits --model
--coder-key <key>                # default: inherits the main provider's key
--coder-path <dir>              # knowledge base dir (default: <--path>/coder_knowledge/,
                                 # falling back to <--asset-root>/scripts/coder_knowledge/)
--coder-persona <text>          # personality/identity prefix (default: built-in; editable in web Preferenze)
--coder-vision-provider <n>     # provider for image analysis (analyze_image tool)
--coder-vision-model <n>        # model for image analysis
```

`--coder-path` matters most when `--path` points somewhere other than `scripts/` (e.g. `my_scripts/`) — without it, CoderAI can't find the knowledge base and works blind.

---

## Web search & stock images (CoderAI tools)

```bash
--search-provider <n>    # duckduckgo|brave (default: duckduckgo)
--search-key <key>       # required for brave
--pixabay-key <key>      # enables search_images tool (stock photo lookup)
```

---

## RAG options

```bash
--rag <file>              # JSONL file of past turns (built with --save-mode full)
--rag-examples <n>        # examples injected per turn (default: 3)
```

Workflow: play with `--save-mode full` → use that `.jsonl` as `--rag` in future sessions.

---

## Image generation options

Image generation has two independent operations:
- **text-to-image (t2i)** — generates missing asset images from a prompt
- **image-to-image (i2i)** — takes the reference collage and renders the final scene

Different providers can be used for each.

#### Primary image provider

```bash
--img-provider <name>     # sdcpp_local | openai | openrouter | fal | wavespeed | dashscope | aimlapi | qwen_local
--img-url <url>           # base URL (local servers)
--img-key <key>
--img-t2i-model <name>
--img-i2i-model <name>
--img-width <n>           # default: 1024
--img-height <n>          # default: 1024
--img-steps <n>           # default: 28
--img-strength <f>        # i2i denoising strength 0.0–1.0 (default: 0.75)
--img-lora <name>         # LoRA name/path (provider-dependent)
--img-lora-scale <f>      # LoRA weight (default: 1.0)
--i2i-model-lora <name>   # LoRA specifically for the i2i model
--img-i2i-steps <n>       # i2i step count override (default: 0 = use --img-steps)
--img-guidance-scale <f>  # CFG/guidance scale (default: 1.0)
```

#### Separate i2i provider (optional)

```bash
--img-i2i-provider <name>
--img-i2i-url <url>
--img-i2i-key <key>
```

#### Provider quick reference

| Provider string | t2i | i2i | Notes |
|---|---|---|---|
| `sdcpp_local` | ✓ | ✓ | stable-diffusion.cpp server. i2i is async with native polling |
| `openai` | ✓ | ✓ | DALL-E 3 / GPT-image. i2i via multipart `/v1/images/edits` |
| `openrouter` | ✓ | ✓ | Routes to FLUX, SD and other image models |
| `fal` | — | ✓ | fal.ai. Fast, high quality |
| `wavespeed` | — | ✓ | WaveSpeed Qwen2.5-VL. Async polling. ~$0.03/image. Sends bg + NPC portraits as separate images |
| `dashscope` | ✓ | ✓ | Alibaba Cloud |
| `aimlapi` | ✓ | ✓ | AIML API gateway |
| `qwen_local` | — | ✓ | Local Qwen-based image editing server |

---

## Local server URLs

Optional local Python servers, manageable from the web UI (start/stop, install deps). Each has its own repo folder (`tts_locale/`, `faceswap_locale/`) with its own `server.py`.

```bash
--tts-url <url>          # XTTS v2 TTS server (voice synthesis, /api/tts)
--faceswap-url <url>     # faceswap_locale server
```

---

## In-game commands

Work in both console mode and web mode.

| Command | Description |
|---|---|
| `/save` | Save current session to disk |
| `/status` | Print raw JSON game state |
| `/summary [N]` | Summarise history and compress it. `N` = recent turns to keep (default: 2) |
| `/fix <instruction>` | Ask LLM to rewrite the last scene with a correction. Time does not advance. |
| `/observe [subject]` | Detailed sensory description without advancing time |
| `/image` | Generate scene image (cached if assets unchanged) |
| `/image --partial` | Generate scene image even if some assets are missing |
| `/image regen` | Force regenerate scene (bypass cache) |
| `/image refine` | Refine last render — uses previous scene image as i2i base |
| `/image fix --s <f>` | Force regenerate with custom i2i strength `<f>` (0.0–1.0) |
| `/image compose` | Use background only as i2i base (no NPC collage) |
| `/generate_asset <id>` | Generate or regenerate a specific asset image |
| `/show_asset <id>` | Display an existing asset image |
| `/quit` · `/q` | Exit (console mode only) |
| `/help` | Show CLI reference (console mode only) |
| *any other `/xxx`* | Delegated to your Lua script's `process_player_input()` |

---

## Full example — production setup

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
