# Visual Novel mode

Point-and-click mode that replaces the chat interface with a bg image + NPC
sprites + verb-coin interaction. ZERO new engine turns — verb-coin synthesises
an input string and posts it to the standard `/api/chat` endpoint.

File: `scripts/lib/visualnovel.lua`

```lua
-- loaded lazily by adventure.lua when CFG.vn is set
```

---

## Enabling VN mode in an adventure

```lua
-- in your adventure script, inside adv.set_config():
adv.set_config({
    vn = {
        catalog = "catalog/vn_scene.json",  -- relative to ASSET_ROOT
        -- optional verb palette overrides:
        verb_kinds = { talk="talk", chiedi="talk", dai="give" }
    }
    -- ... other CFG fields
})
```

When `cfg.vn.catalog` is set, adventure.lua lazy-loads visualnovel.lua and
calls `vn.init(catalog_path, asset_root)` on the first call to `vn_scene()`.

---

## Catalog file: `catalog/vn_scene.json`

Lives at `<ASSET_ROOT>/catalog/vn_scene.json` (ASSET_ROOT = dir that contains
`asset/` and `catalog/`; usually the same as `--path`).

```json
{
  "backgrounds": [
    {
      "id": "cucina_giorno",
      "location": "cucina",
      "tags": ["giorno"],
      "file": "asset/vn/bg/cucina_giorno.jpg",
      "hotspots": [
        {
          "type": "obj",
          "object": "frigo",
          "label": "Frigorifero",
          "rect": [0.05, 0.3, 0.2, 0.6]
        },
        {
          "type": "exit",
          "location": "salotto",
          "label": "Salotto →",
          "rect": [0.85, 0.2, 0.15, 0.8]
        }
      ]
    }
  ],
  "sprites": [
    {
      "id": "anna_default",
      "npc": "anna",
      "tags": ["default"],
      "file": "asset/vn/npc/anna_default.png"
    },
    {
      "id": "anna_felice",
      "npc": "anna",
      "tags": ["felice", "sorridente"],
      "file": "asset/vn/npc/anna_felice.png"
    }
  ],
  "palette": {
    "npc_verbs":  ["parla", "chiedi", "dai", "osserva"],
    "obj_verbs":  ["esamina", "prendi", "usa", "apri"],
    "verb_kinds": { "parla": "talk", "chiedi": "talk", "dai": "give" }
  }
}
```

### `file` paths
All `file` fields are **relative to ASSET_ROOT**. They are served by
`GET /api/vn/asset?path=<file>` (base64 JSON). Absolute paths also work but
are not portable.

### Background selection
`pick_background_for(location_id, variant_tags)` — selects the bg whose
`location` matches the current room, then picks the best variant by tags.
Falls back to `pick_background(bg_tags)` (legacy tag-only match) if no bg
has a `location` field.

**Variant tags** control day/night/state variants of the same room:
```lua
-- In adventure.lua vn_scene opts:
variant_tags = function(s) return {s.ora < 8 and "notte" or "giorno"} end
```

### Sprite selection
`pick_sprite(npc_id, mood_tags)` — finds sprites where `sprite.npc == npc_id`,
picks best overlap with `mood_tags`. Falls back to first sprite for that NPC.

---

## get_vn_scene() global

Engine calls `GET /api/vn/scene` → calls Lua global `get_vn_scene()`.
Install with `adv.vn_scene(opts)`:

```lua
function get_vn_scene()
    return adv.vn_scene({
        -- all optional:
        bg_tags       = { state.location, daypart() },  -- legacy fallback
        variant_tags  = function(s) return {daypart()} end,
        npc_verbs     = {},   -- per-NPC extra verbs (table: {npc_id=[verbs]})
        npc_topics    = {},   -- per-NPC extra topics
        inventory     = nil,  -- override state.inventario
    })
end
```

`adv.vn_scene()` assembles the scene JSON:
```json
{
  "location": "cucina",
  "background": { "file": "asset/vn/bg/cucina_giorno.jpg", "hotspots": [...] },
  "npcs": [
    {
      "id": "anna",
      "name": "Anna",
      "sprite": "asset/vn/npc/anna_felice.png",
      "slot": 0,
      "verbs": ["parla", "chiedi", "dai", "osserva"],
      "topics": ["obj:frigo", "npc:marco", "topic:weekend"]
    }
  ],
  "exits": [
    { "location": "salotto", "label": "Salotto →" }
  ],
  "inventory": ["chiave", "lettera"],
  "palette": {
    "npc_verbs": [...],
    "obj_verbs": [...],
    "moods": [...],
    "intensities": [...],
    "verb_kinds": { "parla": "talk", "dai": "give" }
  }
}
```

---

## Verb-coin interaction flow

1. Player clicks NPC → chooses verb → (if talk) chooses topic → Conferma
2. Player clicks hotspot obj → chooses verb → Conferma
3. Player clicks exit hotspot OR exit button → move immediately
4. Player clicks inventory item → chooses verb → (if "usa") clicks target → Conferma

Confirmed action → synthesised input string:
```
[AZIONE] verbo=parla target=anna tipo=npc argomento=topic:weekend umore=curioso intensita=2
```
Posted to `/api/chat`. Engine runs normal LLM turn with `prompt_vn_action()`
block in system prompt (see below).

---

## System prompt integration

```lua
function get_system_prompt()
    return ...
        .. adv.prompt_vn_action()  -- explains [AZIONE] format to LLM
        ..
end
```

`prompt_vn_action()` instructs the LLM to:
- Parse the `[AZIONE]` prefix
- Generate `player_line` (1st-person speech between «») embedded in narration
- Run tool calls (move_player, object_action) as normal
- Return standard schema response

---

## Scaffold (one-click VN conversion)

`POST /api/scaffold {"mode":"vn"}` — creates `catalog/vn_scene.json` skeleton
and installs a universal `get_vn_scene` in the running adventure. Persists
across reload/load (catalog presence triggers auto-scaffold).

---

## Verb kinds

Verbs in the palette have a "kind" that determines the submenu shown:
- `"act"` (default) — no submenu, commit immediately
- `"talk"` — shows topics submenu (NPC-specific topics)
- `"give"` — shows inventory picker

Priority (lower wins):
1. Built-in dict (parla/chiedi=talk, dai/mostra=give)
2. `catalog.palette.verb_kinds` (set via GUI Catalogo VN → verb badge)
3. `CFG.vn.verb_kinds` (script-level, always wins)

---

## Persona: per-NPC verbs and topics

NPC persona files can carry VN-specific fields:
```lua
-- in persona file (or via npc_life_event tool):
vn_verbs = { "flirta", "rimprovera" }  -- extra verbs for this NPC
topics   = { "sua sorella", "il lavoro" }  -- extra topics
```

Access: `persona.vn_verbs(id)`, `persona.topics(id)`
These are merged into the scene's NPC entry by `adv.vn_scene()`.

---

## Asset conventions

```
ASSET_ROOT/
  catalog/
    vn_scene.json       -- catalog (edit via GUI Catalogo VN or CoderAI)
  asset/
    vn/
      bg/               -- background images (JPG or PNG, any resolution)
        cucina_giorno.jpg
        salotto_notte.jpg
      npc/              -- NPC sprites (PNG with TRANSPARENT background required)
        anna_default.png
        anna_felice.png
```

NPC sprites MUST be PNG with alpha (transparent bg). Use the "rimuovi sfondo"
workflow to strip backgrounds from photos/generated images.

---

## Authoring workflow (CoderAI)

1. `POST /api/scaffold {"mode":"vn"}` — creates empty catalog
2. Generate or download backgrounds: `generate_image` / `search_images` + `download_asset`
3. Generate or download NPC sprites → strip background → save as PNG alpha
4. Edit catalog via GUI (Catalogo VN) OR `read_file` + `str_replace` the JSON
5. `POST /api/vn/catalog` (via write_file to the catalog path) to hot-reload
6. Test via GUI VN view (Finestra Visual Novel)

For generating NPC sprites:
- Use `get_npc_description(id)` + `get_adventure_style()` to compose prompt
- `generate_portrait(prompt, path, char_id?)` if t2i_locale configured
- `generate_image(prompt, path)` for any t2i provider
- Strip bg via rembg_locale server (POST /remove, port 8005) or ask user to do it manually

---

## Live catalog reload

After editing `catalog/vn_scene.json`:
- GUI auto-reloads on save (Salva catalogo button)
- Engine: `POST /api/vn/catalog` with full JSON body reloads the catalog
- Lua: global `vn_reload_catalog(path)` (installed by adventure.lua)
- NO engine restart needed
