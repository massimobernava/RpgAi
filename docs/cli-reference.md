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

**Save modes:**
- `last` — always exactly one line: the most recent turn. Fast, minimal disk use.
- `full` — every turn appended. Use this to build a RAG corpus (`--rag`).

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
