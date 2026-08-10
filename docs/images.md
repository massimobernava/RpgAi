# Image System

RpgAi can illustrate your adventure automatically. Each time the player types `/image`, the engine asks your Lua script which images make up the current scene, builds a reference collage, generates a visual prompt with the LLM, and sends everything to an image-to-image model to produce a coherent scene illustration.

The system is entirely optional — scripts that don't implement the image functions work perfectly without it.

---

## How a scene is rendered

```
/image command
      │
      ▼
get_scene_images()          ← your Lua script
      │   [{id="bg", path="..."}, {id="npc", path="..."}]
      ▼
Check each path on disk
      ├── missing? ──► get_asset_prompt(id)
      │                      │
      │               LLM generates txt2img prompt
      │                      │
      │               text_to_image()  →  save to disk
      │
      ▼
build_collage()             background first, NPCs side by side
      │
      ▼
LLM generates visual prompt  (scene context + last narration + asset tags)
      │
      ▼
image_to_image(source, prompt)   →  scene image
      │
      ▼
scene_cache::upsert()       saved to images/scene_cache/
      │
      ▼
Image displayed in web UI
```

---

## Directory layout

```
scripts/
├── my_adventure.lua
└── images/
    ├── my_adventure/          ← your asset images (backgrounds + NPCs)
    │   ├── bg_dungeon.jpg
    │   ├── bg_tavern.jpg
    │   ├── npc_innkeeper.jpg
    │   └── npc_goblin.jpg
    ├── scene_cache/           ← auto-created; final rendered scenes
    │   ├── cache_db.json
    │   └── 20250421_153012_scene.jpg
    └── collage_tmp/           ← auto-created; intermediate collages
```

You provide assets in `images/my_adventure/`. Everything under `scene_cache/` and `collage_tmp/` is managed automatically.

---

## Asset images

**Backgrounds** — the location. Wide establishing shot. 16:9 aspect ratio works best for the collage.

**Characters** — NPCs. Half-body or full-body portraits on a neutral background. The engine places them alongside the background in the collage.

---

## Implementing the three image functions

### `get_scene_images()` — what's in the scene

```lua
local IMAGE_DIR = "images/my_adventure/"

local ASSET_PATHS = {
    dungeon_entrance = IMAGE_DIR .. "bg_entrance.jpg",
    tavern           = IMAGE_DIR .. "bg_tavern.jpg",
    innkeeper        = IMAGE_DIR .. "npc_innkeeper.jpg",
    goblin_guard     = IMAGE_DIR .. "npc_goblin.jpg",
}

function get_scene_images()
    local result = {}
    local loc = state.player.location

    -- Background first (always)
    if ASSET_PATHS[loc] then
        table.insert(result, { id=loc, path=ASSET_PATHS[loc] })
    end

    -- NPCs present in this location
    for npc_id, npc_loc in pairs(state.npc_locations or {}) do
        if npc_loc == loc and ASSET_PATHS[npc_id] then
            table.insert(result, { id=npc_id, path=ASSET_PATHS[npc_id] })
        end
    end

    -- Dynamically spawned NPCs
    for npc_id, entry in pairs(state.npc_dinamici or {}) do
        if entry.location == loc then
            local dyn_path = IMAGE_DIR .. "npc_dyn_" .. npc_id .. ".jpg"
            table.insert(result, { id=npc_id, path=dyn_path })
        end
    end

    return result
end
```

`get_scene_images()` can also return **Format B** with an explicit i2i base hint:

```lua
return {
    assets     = result,
    base_image = "last"       -- reuse the last cached render as i2i base
    -- base_image = "/abs/path.jpg"  -- use a specific file
    -- base_image = nil              -- default: build collage from assets
}
```

Use `base_image="last"` when only the narration changed (same location, same NPCs) — the model refines the existing render instead of rebuilding from scratch.

### `get_asset_path(id)`

```lua
function get_asset_path(id)
    return ASSET_PATHS[id]
end
```

### `get_asset_prompt(id)` — generation prompt

```lua
local IMAGE_STYLE = "fantasy illustration, dramatic lighting, detailed, painterly style, 8k quality"

local ASSET_DESCRIPTIONS = {
    dungeon_entrance = {
        tipo = "background",
        desc = "Stone archway entrance to a dark dungeon, iron portcullis half-raised, " ..
               "torchlight from within, moisture dripping from ceiling, wide establishing shot"
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

### `get_image_style()` — scene render style

Optional. Return a style string that gets injected into the i2i visual prompt for every scene render. Use it to keep the same style you already use in `get_asset_prompt()` for t2i assets:

```lua
local IMAGE_STYLE = "fantasy illustration, dramatic lighting, painterly style, 8k quality"

-- t2i asset prompts already include IMAGE_STYLE (see get_asset_prompt above)

-- This ensures the same style reaches i2i scene renders
function get_image_style()
    return IMAGE_STYLE
end
```

The string is:
1. Shown to the LLM that generates the visual prompt (as style guidance)
2. Appended directly to the final prompt sent to the image model

Scripts without `get_image_style()` work exactly as before.

---

## Scene cache

The cache key is: **script name + sorted asset IDs + max asset mtime**.

- Same scene, same assets, no changes → cached image reused instantly
- Assets regenerated → cache invalidated, new render
- Different NPCs in same room → different key, new render

Cache database: `images/scene_cache/cache_db.json`. To clear: delete `images/scene_cache/`.

---

## `/image` render modes

| Command | Behaviour |
|---|---|
| `/image` | Return cached image if key matches; otherwise render |
| `/image regen` | Bypass cache, force new render |
| `/image refine` | Use the last cached render as i2i base (refine instead of rebuild) |
| `/image fix --s <f>` | Force render with custom i2i strength `<f>` (0.0–1.0) |
| `/image compose` | Use background only (no NPC collage) as i2i base |
| `/image --partial` | Render even if some assets are missing |

**When to use `refine`:** scene composition unchanged (same location, same NPCs), only narration changed. The model gets the last render as its base → subtle coherent update instead of a full rebuild.

**When to use `compose`:** you want the i2i model to place the characters freely into the background rather than blending from a pre-merged collage. Strength defaults to 0.95.

---

## Generating and viewing assets

Missing assets are generated automatically on `/image`. You can also trigger generation manually:

```
/generate_asset dungeon_entrance
/generate_asset goblin_guard
```

View an existing asset without rendering a scene:

```
/show_asset innkeeper
```

---

## Provider details

### WaveSpeed (`wavespeed`) — recommended for i2i
Async polling API (~$0.03/image). Unlike other providers that receive a merged collage, WaveSpeed gets **background and NPC portraits as separate images** — background as `images[0]`, each NPC as an additional reference in the `images` array. This lets the model place characters naturally without collage blending artefacts.

```bash
--img-i2i-provider wavespeed --img-i2i-key YOUR_KEY
```

### fal.ai (`fal`) — recommended for i2i
High quality, fast. Supports FLUX and other leading models.
```bash
--img-i2i-provider fal --img-i2i-key YOUR_FAL_KEY \
--img-i2i-model fal-ai/flux/dev/image-to-image
```

### stable-diffusion.cpp (`sdcpp_local`) — local, free
Both t2i and i2i. i2i uses the native async job API.
```bash
--img-provider sdcpp_local --img-url http://localhost:7860 \
--img-t2i-model your_checkpoint --img-i2i-model your_checkpoint
```

### OpenRouter (`openrouter`) — for t2i
Access to FLUX 1.1 Pro, SDXL and others.
```bash
--img-provider openrouter --img-key sk-or-YOUR_KEY \
--img-t2i-model black-forest-labs/flux-1.1-pro
```

### OpenAI (`openai`) — t2i and i2i
DALL-E 3 / GPT-image. i2i via the multipart `/v1/images/edits` endpoint.
```bash
--img-provider openai --img-key sk-YOUR_KEY --img-t2i-model gpt-image-1
```

### DashScope (`dashscope`) — Alibaba Cloud
```bash
--img-provider dashscope --img-key YOUR_KEY --img-t2i-model wanx-v1
```

### AIMLAPI (`aimlapi`) — gateway to multiple image models
```bash
--img-provider aimlapi --img-key YOUR_KEY --img-t2i-model flux/dev
```

### `qwen_local` — local Qwen-based i2i server
i2i only, no t2i. Pairs with `t2i_locale/` (own FLUX + PuLID pipeline for t2i) for a fully local setup — see that server's own docs.
```bash
--img-i2i-provider qwen_local --img-i2i-url http://localhost:PORT
```

---

## LoRA options

For providers that support them (local servers mainly):
```bash
--img-lora <name>          # LoRA name/path
--img-lora-scale <f>       # LoRA weight, default 1.0
--i2i-model-lora <name>    # LoRA specifically for the i2i pass
--img-i2i-steps <n>        # i2i step count override (0 = use --img-steps)
--img-guidance-scale <f>   # CFG/guidance scale, default 1.0
```

---

## CoderAI image tools

Beyond the per-turn scene pipeline above, [CoderAI](../CODERAI.md) (the in-browser coding assistant) has its own image tools for building/fixing assets while editing a script: `generate_image` (plain t2i), `edit_image` (i2i, bypasses the scene cache), `analyze_image` (vision-model Q&A on a local or external image), and — if a `t2i_locale` server is configured — `generate_portrait`/`generate_scene` (FLUX + IP-Adapter face-conditioned NPC portraits). These are approval-gated tool calls in the CoderAI chat, not CLI flags.

---

## Tips

- **Background first, always.** The collage places images left-to-right in order returned by `get_scene_images()`. Background must be first.
- **Consistent asset style.** Same photographic style, lighting and colour palette for all assets. Mismatched styles are the main source of jarring results.
- **Neutral backgrounds for portraits.** NPCs on plain grey/white — the i2i model places them into the scene naturally.
- **`--img-strength`:** 0.5–0.6 = stay close to collage (good for high-quality assets); 0.75–0.9 = more freedom (better for rough/mismatched assets).
- **Generate assets before playing.** Run `/generate_asset <id>` for every location and NPC first — `/image` during play is then instant.
