# Asset system and image tools

How assets are stored, served, generated, and managed in RpgAi.
CoderAI has a full image generation + editing toolkit.

---

## ASSET_ROOT

All assets (backgrounds, sprites, NPC portraits) live under **ASSET_ROOT**:
- Set by `--path` (basePath) — same dir as the adventure scripts
- `catalog/` — JSON catalogs (vn_scene.json, rules.json, ...)
- `asset/` — images subdivided by type:
  - `asset/vn/bg/`   — VN background images
  - `asset/vn/npc/`  — VN NPC sprites (PNG alpha)
  - `asset/npcs/`    — NPC portrait images (used by game scene)
  - `asset/bg/`      — scene backgrounds (non-VN mode)

File paths in catalogs and persona files are **relative to ASSET_ROOT**.

---

## Routes for assets

```
GET /api/vn/asset?path=<rel>       → base64 JSON {data, mime}  (rooted at ASSET_ROOT)
GET /api/serve_file?path=<rel>     → raw bytes               (rooted at scripts/ basePath)
GET /api/show_asset?path=<rel>     → base64 JSON              (rooted at basePath)
GET /api/coder/image?path=<rel>    → raw PNG/JPG              (CoderAI asset preview)
GET /api/coder/image?url=<url>     → proxied raw bytes        (avoids Referer blocks)
```

Use `/api/vn/asset` for anything in the VN catalog. Use `/api/coder/image`
in CoderAI replies to display local images inline in the chat.

---

## CoderAI image tools

### Auto (no approval needed)

**`search_images(query, count?)`** — search Pixabay (or configured provider)
for free stock images. Returns list of URLs + preview URLs. Use to find
backgrounds, textures, reference photos.

**`analyze_image(path_or_url, question?)`** — VLM description of image.
`path` = local relative path (whitelist: scripts/, my_scripts/, saves/, images/).
`url` = external URL (fetched via curl).
If `question` is provided, answers that specific question.

**`t2i_reference(action, char_id?, file?)`**
- `action="list"` — list all registered face references
- `action="health"` — check t2i_locale server status
- `action="add", char_id="X", file="path/to/face.png"` — upload face crop
- `action="build", char_id="X"` — build IP-Adapter embedding for that face

### Confirm (approval modal)

**`generate_image(prompt, save_path)`** — text-to-image. Uses configured
t2i provider (`--img-provider`). Prompt MUST be in English. Returns local path.
Use for: backgrounds, props, concept art, NPC portraits (no face conditioning).

**`edit_image(input_path, instruction, output_path?)`** — image-to-image edit.
Uses configured i2i provider. Instruction MUST be in English.
- Default output: `<stem>_edited.<ext>` (never overwrites input)
- Takes 1-10 minutes (GPU). Warn user before calling.
- NEVER call more than once per user request.
- For VN sprites: generate → strip bg → composite on white/transparent.

**`generate_portrait(prompt, save_path, char_id?, id_scale?)`**
Requires `--img-url` pointing to t2i_locale server.
- With `char_id`: face-conditioned (IP-Adapter). `id_scale` 0.0-1.0 (default 0.6).
- Without `char_id`: same as generate_image on t2i_locale.
- `faceswap=true`: hard face replacement (photorealistic). Omit for stylized.

**`generate_scene(prompt, chars, save_path)`**
Multi-NPC face conditioning. `chars` = list of char_ids with registered refs.
Requires `--img-url`.

**`download_asset(url, save_path)`** — download from URL to local path.
Useful after `search_images` to save a chosen image.

**`copy_file(src, dst)`** — copy a file within the whitelist paths.
Use to place a generated image at its final asset path.

---

## NPC portrait workflow (complete)

```
1. get_npc_description(id)     → physical description from persona file
2. get_adventure_style()       → visual style string (e.g. "anime, soft colors")
3. Compose prompt (English): "<appearance>, <style>, portrait, white background"
4. Show prompt to user for review/edit before generating
5. generate_image(prompt, "asset/npcs/<id>_draft.png")
   OR generate_portrait(prompt, "asset/npcs/<id>.png", char_id=id)  [if t2i_locale]
6. analyze_image("asset/npcs/<id>.png", "describe the character")   [verify]
7. copy_file(draft_path, final_asset_path)   [if draft approved]
8. get_asset_path(id)   [find where the game expects the asset]
```

---

## NPC sprite workflow (VN mode)

NPC sprites = PNG with TRANSPARENT background (alpha channel required).
The GUI overlays them on the background image.

```
1. Generate portrait (generate_portrait or generate_image)
2. Remove background with CoderAI tool:
   remove_background(
     input_path  = "asset/npcs/rossana_portrait.png",
     output_path = "asset/vn/npc/rossana_sprite.png",
     type        = "rgba"           # transparent PNG
   )
   Requires rembg_locale running: cd rembg_locale && ./start.sh
   type options: rgba (transparent, default), map (grayscale matte),
                 white / green (flat bg, useful as intermediate)
   threshold: optional float 0..1 for harder edges (try 0.5 for clean subjects)
3. Verify result: analyze_image("asset/vn/npc/rossana_sprite.png", "Is the bg transparent?")
4. Add to catalog (GUI Catalogo VN → Sprite tab, or str_replace catalog/<stem>_vn.json)
5. Reload catalog (GUI Salva)
```

rembg_locale server details:
- URL: http://127.0.0.1:8005
- GET /health → {"status":"ok","model_loaded":true,...}
- POST /remove (multipart): image file + type + optional threshold → raw PNG bytes
- Backed by transparent-background / InSPyReNet (better than u2net on human figures)
- First start downloads model ~200 MB to ~/.transparent-background/
- `--mode fast` for speed, `--mode base` for quality (default)

---

## Background workflow (VN mode)

**IMPORTANT — resolution**: VN backgrounds MUST be **landscape (horizontal)**.
Target: **1280×720** (16:9) or **1920×1080** (16:9 HD).
Portrait/vertical images look wrong in the VN window (letterboxed horizontally,
wasted vertical space, characters appear squished). Always confirm orientation
before saving or handing off to the user.

```
1. search_images("interior italian apartment kitchen day")
   OR generate_image("an Italian apartment kitchen, daytime, realistic, wide angle,
                      landscape, 16:9, 1280x720")
2. download_asset(url, "asset/vn/bg/<loc>_giorno.jpg")
   → Verify: analyze_image(path, "Is this landscape (wider than tall)?")
   → If portrait: edit_image(path, "Convert to landscape 16:9, extend sides naturally")
3. Add to catalog:
   { "id": "<loc>_giorno", "location": "<loc>", "tags": ["giorno"], "file": "asset/vn/bg/..." }
4. Optionally add hotspots via GUI (Catalogo VN → canvas drag)
5. Reload catalog
```

One background per (location, variant). Variants = day/night/state.
Tag "giorno" / "notte" / "pioggia" etc. are matched by the VN renderer.

---

## get_asset_path / get_npc_description / get_adventure_style

Three auto tools that query the live game session:

**`get_asset_path(asset_id)`** — returns the file path registered for an
asset ID in the current adventure's `get_asset_path()` Lua function.
Use before `copy_file` to find where the game expects the asset.

**`get_npc_description(npc_id)`** — returns `appearance` + `outfit` from
the NPC's persona file. Use as base for image generation prompts.

**`get_adventure_style()`** — returns `get_image_style()` from the active
adventure script (visual style for consistent generation).

---

## Scene image pipeline (non-VN mode)

Used by `/image` command in the game chat. Managed by `get_scene_images()`,
`get_asset_prompt()`, `get_image_style()` Lua hooks.
CoderAI can generate/replace assets:

```lua
-- adventure script hooks:
function get_scene_images()
    return { assets={ {id="bg_cucina", path="asset/bg/cucina.jpg"},
                      {id="npc_anna",  path="asset/npcs/anna.png"} } }
end
function get_asset_prompt(id)
    if id=="bg_cucina" then return {path="asset/bg/cucina.jpg", prompt="Italian kitchen..."} end
end
function get_image_style()
    return "anime style, soft watercolor palette"
end
```

---

## Checking asset existence

```python
# From CoderAI:
list_files("asset/vn/bg/")       # see what backgrounds exist
list_files("asset/vn/npc/")      # see what sprites exist
list_files("asset/npcs/")        # see what portraits exist
analyze_image("asset/vn/npc/anna_default.png")  # verify content
```

---

## Catalog hot-reload vs engine restart

- **Catalog JSON** (`vn_scene.json`, `rules.json`) → hot-reloadable via POST
- **Lib files** (`adventure.lua`, `visualnovel.lua`, etc.) → require engine restart
- **Adventure script** → hot-reload via `reload_script()`
- **Persona files** → hot-reload via `reload_script(preserve_state=true)` (runs `persona.reload_all()`)

---

## rembg_locale server (background removal)

```
Port: 8005
Endpoint: POST /remove
Form fields:
  image: <binary PNG/JPG>
  type:  rgba (transparent PNG) | white | green
  threshold: 0.5 (default)

GET /health  →  {"status":"ok"}
```

Not always running — check `GET /api/servers/status` first.
If down: `POST /api/servers/action {"name":"rembg_locale","action":"start"}`.
