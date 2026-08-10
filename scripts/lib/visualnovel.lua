-- lib/visualnovel.lua
-- ============================================================================
-- Visual-Novel mode — asset catalog (tag-matched) + scene assembler.
--
-- Backs the "japanese visual novel" rendering mode in rpgai-gui:
--   ONE flat background image per room (tagged, e.g. {"cucina","giorno"}) +
--   alpha-cut NPC sprites (tagged, e.g. {"pigiama","felice"}) pasted in the
--   foreground. No i2i/t2i at runtime — the master authors assets once and tags
--   them; this lib SELECTS the closest asset for the current world state.
--
-- Catalog file: catalog/vn_scene.json (see schema in that file / CLAUDE.md).
--
-- Usage (from an adventure, opt-in):
--   local vn = require("lib/visualnovel")
--   vn.init("catalog/vn_scene.json")            -- once at startup
--   function get_vn_scene()
--       return json.encode(vn.build_scene({
--           location     = state.player.location,
--           time         = state.time, day = state.day,
--           npcs_present = { "anna", "marco" },
--           bg_tags      = { "cucina", vn.daypart(state.time) },
--           npc_tags     = { anna = {"pigiama","felice"}, marco = {"completo"} },
--       }))
--   end
--
-- Design: structural beats prompt — selection is deterministic Lua, never an
-- LLM call. Degrades gracefully: a partial/empty tag set still yields the best
-- available asset (fuzzy fallback), never an empty scene.
-- ============================================================================

local json = require("json")

local M = {}

local _backgrounds      = {}   -- array of { id, tags={}, file, hotspots={} }
local _sprites          = {}   -- array of { id, npc, tags={}, file }
local _verb_options     = {}   -- map verb → list of L2 option strings (from catalog verb_options)
local _catalog_palette  = {}   -- {npc_verbs=[], obj_verbs=[]} — L1 overrides from catalog
local _loaded           = false
local _path             = nil

-- ── helpers ────────────────────────────────────────────────────────────────

local function _as_set(list)
    local s = {}
    if type(list) == "table" then
        for _, t in ipairs(list) do
            if type(t) == "string" then s[t:lower()] = true end
        end
    end
    return s
end

-- Jaccard-ish score: how well a candidate's tag set covers the wanted tags.
-- Rewards overlap, lightly penalises extra unrelated tags so a tighter asset
-- wins ties. Returns a float; higher is better. 0 = no overlap.
local function _score(cand_set, want_set)
    local inter, want_n, cand_n = 0, 0, 0
    for t in pairs(want_set) do
        want_n = want_n + 1
        if cand_set[t] then inter = inter + 1 end
    end
    for _ in pairs(cand_set) do cand_n = cand_n + 1 end
    if inter == 0 then return 0 end
    -- coverage of wanted tags dominates; small bonus for fewer spurious tags
    local coverage = inter / math.max(1, want_n)
    local tightness = inter / math.max(1, cand_n)
    return coverage + 0.25 * tightness
end

-- Pick the best entry from `list` for `want_tags`, optional pre-filter `pred`.
-- Exact full-set match wins; else max fuzzy score; deterministic tie-break by id.
-- require_overlap=true → return nil when NO candidate shares any tag (avoids
-- silently returning an unrelated asset just because it was first in the list).
local function _pick(list, want_tags, pred, require_overlap)
    local want = _as_set(want_tags)
    local best, best_score = nil, -1
    for _, e in ipairs(list) do
        if not pred or pred(e) then
            local sc = _score(_as_set(e.tags), want)
            -- deterministic: strictly greater, or equal score + smaller id
            if sc > best_score
               or (sc == best_score and best and tostring(e.id) < tostring(best.id)) then
                best, best_score = e, sc
            end
        end
    end
    if require_overlap and best_score <= 0 then return nil end
    return best
end

-- ── init ─────────────────────────────────────────────────────────────────────

--- Load the VN asset catalog. Safe to call repeatedly (idempotent reload).
-- @param path  Path to catalog/vn_scene.json (relative to the engine cwd).
-- @return true on success, or false, err
function M.init(path)
    _backgrounds, _sprites, _verb_options, _catalog_palette, _loaded = {}, {}, {}, {}, false
    _path = path
    local f = io.open(path, "r")
    if not f then
        return false, "cannot open VN catalog: " .. tostring(path)
    end
    local raw = f:read("*a"); f:close()
    local ok, data = pcall(json.decode, raw)
    if not ok or type(data) ~= "table" then
        return false, "VN catalog is not valid JSON: " .. tostring(data)
    end
    for _, b in ipairs(data.backgrounds or {}) do
        if type(b) == "table" and b.file then
            _backgrounds[#_backgrounds + 1] = {
                id       = b.id or b.file,
                location = b.location,   -- explicit hard link to a location id (preferred)
                tags     = b.tags or {}, -- variant axis: giorno/notte/state (legacy: also the location link)
                file     = b.file,
                hotspots = b.hotspots or {},
            }
        end
    end
    for _, s in ipairs(data.sprites or {}) do
        if type(s) == "table" and s.file then
            local vv = {}
            if type(s.verbs) == "table" then
                for _, v in ipairs(s.verbs) do vv[#vv+1] = v end
            end
            _sprites[#_sprites + 1] = {
                id    = s.id or s.file,
                npc   = s.npc,
                tags  = s.tags or {},
                file  = s.file,
                verbs = vv,
            }
        end
    end
    -- verb_options: {verb → [string, ...]} — L2 sub-options for the GUI verb coin.
    if type(data.verb_options) == "table" then
        for verb, opts in pairs(data.verb_options) do
            if type(opts) == "table" then
                _verb_options[verb] = opts
            end
        end
    end
    -- palette: L1 verb overrides + verb_kinds set in the GUI Catalogo VN.
    if type(data.palette) == "table" then
        _catalog_palette = {
            npc_verbs  = type(data.palette.npc_verbs)  == "table" and data.palette.npc_verbs  or {},
            obj_verbs  = type(data.palette.obj_verbs)  == "table" and data.palette.obj_verbs  or {},
            verb_kinds = type(data.palette.verb_kinds) == "table" and data.palette.verb_kinds or {},
        }
    end
    _loaded = true
    return true
end

--- Returns the verb_options map loaded from the catalog (verb → [string,...]).
-- Empty table when catalog has no verb_options section.
function M.get_verb_options()
    return _verb_options
end

--- Returns catalog-level L1 verb overrides {npc_verbs=[...], obj_verbs=[...]}.
-- Empty tables when catalog has no palette section (engine defaults apply).
function M.get_catalog_palette()
    return _catalog_palette
end

function M.loaded() return _loaded end

-- ── time helper ───────────────────────────────────────────────────────────────

-- Map an "HH:MM" string to a coarse daypart tag, handy for bg_tags.
function M.daypart(time_str)
    local h = tonumber(tostring(time_str or ""):match("^(%d+)")) or 12
    if h < 6  then return "notte"     end
    if h < 12 then return "mattina"   end
    if h < 18 then return "giorno"    end
    if h < 22 then return "sera"      end
    return "notte"
end

-- ── condition evaluation ───────────────────────────────────────────────────────
-- Single condition object:
--   { time_from="HH:MM", time_to="HH:MM" }  -- time window, midnight-wrap ok
--   { flag="flagname" }                       -- state.flags[name] truthy
--   { object="id", state="stato" }            -- ctx.object_states[id]=="stato"
local function eval_condition(cond, ctx)
    if not cond or type(cond) ~= "table" then return true end
    ctx = ctx or {}

    if cond.time_from or cond.time_to then
        local function hm(s)
            if not s then return nil end
            local h, m = s:match("^(%d+):(%d+)$")
            return h and (tonumber(h) * 60 + tonumber(m)) or nil
        end
        local cur_m = hm(ctx.time or "12:00") or 720
        local from_m = hm(cond.time_from)
        local to_m   = hm(cond.time_to)
        if from_m and to_m then
            if from_m <= to_m then
                if not (cur_m >= from_m and cur_m < to_m) then return false end
            else
                if not (cur_m >= from_m or cur_m < to_m) then return false end
            end
        elseif from_m and cur_m < from_m then return false
        elseif to_m   and cur_m >= to_m  then return false
        end
    end

    if cond.flag then
        local st = ctx.state or {}
        local flags = (type(st.flags) == "table" and st.flags)
                   or (type(st.world_flags) == "table" and st.world_flags)
                   or {}
        if not flags[cond.flag] then return false end
    end

    if cond.object and cond.state then
        local obj_states = ctx.object_states or {}
        if obj_states[cond.object] ~= cond.state then return false end
    end

    return true
end

-- Evaluates all conditions on a bg entry (AND logic).
-- Supports both legacy `entry.condition` (single object) and `entry.conditions` (array).
-- No conditions at all = always satisfied (default/fallback bg).
local function eval_conditions(entry, ctx)
    local arr = entry.conditions
    if arr then
        -- new array format
        if type(arr) ~= "table" then return true end
        for _, c in ipairs(arr) do
            if not eval_condition(c, ctx) then return false end
        end
        return true
    elseif entry.condition then
        -- legacy single-object format (backward compat)
        return eval_condition(entry.condition, ctx)
    end
    return true -- no conditions
end

-- ── selection ─────────────────────────────────────────────────────────────────

--- Best background for the given tag list. Returns nil if no background shares
-- any tag (so the scene shows "(nessuno sfondo)" rather than an unrelated room).
function M.pick_background(tags)
    return _pick(_backgrounds, tags or {}, nil, true)
end

--- Explicit-link selection: among backgrounds whose `location` == location_id,
-- pick the best VARIANT by tags (daypart/state). Conditions on bg entries are
-- evaluated against ctx (time, state, object_states). A bg with an unsatisfied
-- condition is excluded. No require_overlap: a location with a single bg always
-- returns it; variants chosen by tag overlap, default = deterministic first by id.
-- Returns nil if no background declares this location.
function M.pick_background_for(location_id, want_tags, ctx)
    if not location_id then return nil end
    local has_any = false
    for _, e in ipairs(_backgrounds) do
        if e.location == location_id then has_any = true; break end
    end
    if not has_any then return nil end
    ctx = ctx or {}
    return _pick(_backgrounds, want_tags or {},
                 function(e)
                     if e.location ~= location_id then return false end
                     return eval_conditions(e, ctx)
                 end, false)
end

-- Evaluate conditions array on a sprite entry against per-NPC context:
--   npc_ctx = { outfit=string, stats={name→number}, flags={name→bool} }
-- Types: {outfit="pigiama"}, {stat="salute",min=X,max=Y}, {flag="name"}
-- AND logic: all conditions must be satisfied.
-- No conditions (empty []) = default sprite (always valid, lowest priority).
local function eval_sprite_conditions(entry, npc_ctx)
    local arr = entry.conditions
    if not arr then
        -- legacy single condition
        if entry.condition then arr = {entry.condition}
        else return true end  -- no conditions = always valid
    end
    if type(arr) ~= "table" or #arr == 0 then return true end
    npc_ctx = npc_ctx or {}
    for _, c in ipairs(arr) do
        if c.outfit then
            local cur = (npc_ctx.outfit or ""):lower()
            if cur:find(c.outfit:lower(), 1, true) == nil then return false end
        end
        if c.stat then
            local stats = npc_ctx.stats or {}
            local val = stats[c.stat]
            if val == nil then return false end
            if c.min and val < c.min then return false end
            if c.max and val > c.max then return false end
        end
        if c.flag then
            local flags = npc_ctx.flags or {}
            if not flags[c.flag] then return false end
        end
    end
    return true
end

--- Best sprite for npc_id matching the given tags and NPC context (outfit/stats/flags).
-- npc_ctx = { outfit, stats, flags } — from adventure.lua build_scene ctx.npc_ctxs[id].
-- Priority: conditioned sprites > unconditioned (default) sprite.
-- Falls back to any valid sprite for this npc; nil if none exist.
function M.pick_sprite(npc_id, tags, npc_ctx)
    if not npc_id then return nil end
    npc_ctx = npc_ctx or {}
    -- First pass: try conditioned sprites (have at least one condition)
    local pred_cond = function(e)
        if e.npc ~= npc_id then return false end
        local arr = e.conditions
        if not arr or (type(arr)=="table" and #arr==0) then return false end
        if not arr and not e.condition then return false end
        return eval_sprite_conditions(e, npc_ctx)
    end
    local hit = _pick(_sprites, tags or {}, pred_cond)
    if hit then return hit end
    -- Second pass: default sprites (no conditions) — pick by tag among those
    local pred_def = function(e)
        if e.npc ~= npc_id then return false end
        local arr = e.conditions
        if arr and type(arr)=="table" and #arr > 0 then return false end
        if not arr and e.condition then return false end
        return true
    end
    hit = _pick(_sprites, tags or {}, pred_def)
    if hit then return hit end
    -- Last resort: first sprite for this npc with satisfied conditions
    for _, e in ipairs(_sprites) do
        if e.npc == npc_id and eval_sprite_conditions(e, npc_ctx) then return e end
    end
    return nil
end

-- ── scene assembly ─────────────────────────────────────────────────────────────

--- Build the scene descriptor for the current world state.
-- ctx = {
--   location, time, day,
--   bg_tags      = { ... },                 -- tags to select the background
--   npcs_present = { "anna", ... },          -- ordered NPC ids in the room
--   npc_tags     = { anna = {"pigiama"}, ... },
--   exits        = { {location=,label=}, ... } (optional; passed through)
-- }
-- Returns a plain table (the adventure json.encodes it for get_vn_scene()):
--   { location, background={id,file,hotspots}, npcs={ {id,file,slot} }, exits={} }
-- NPCs with no matching sprite are skipped (logged via warn-less return).
function M.build_scene(ctx)
    ctx = ctx or {}
    local scene = {
        location   = ctx.location,
        background = nil,
        npcs       = {},
        exits      = ctx.exits or {},
    }

    -- Prefer the explicit location→background link (deterministic, no collisions).
    -- variant_tags (daypart + world-state) choose among that location's variants.
    -- Condition on a bg entry (time/flag/object_state) is evaluated against ctx.
    -- Fall back to legacy tag matching for catalogs without a `location` field.
    local bg = M.pick_background_for(ctx.location, ctx.variant_tags or ctx.bg_tags or {}, ctx)
    if not bg then bg = M.pick_background(ctx.bg_tags or {}) end
    if bg then
        scene.background = { id = bg.id, file = bg.file, hotspots = bg.hotspots or {} }
    end

    local present  = ctx.npcs_present or {}
    local tags     = ctx.npc_tags or {}
    local npc_ctxs = ctx.npc_ctxs or {}
    local names    = ctx.npc_names or {}
    local slot     = 0
    for _, npc_id in ipairs(present) do
        local sp = M.pick_sprite(npc_id, tags[npc_id], npc_ctxs[npc_id])
        slot = slot + 1
        -- Include NPC even when no sprite found: GUI will draw a placeholder.
        scene.npcs[#scene.npcs + 1] = {
            id    = npc_id,
            name  = names[npc_id] or npc_id,
            file  = sp and sp.file or "",
            slot  = slot,
            verbs = (sp and sp.verbs and #sp.verbs > 0) and sp.verbs or nil,
        }
    end

    return scene
end

-- ── Phase B: verb gating (object verbs derived from world state machine) ──────
-- Returns the verb list a target object supports: read-only verbs + the keys of
-- its defined `actions` (world.lua state machine). Falls back to read-only only
-- if the object is unknown. Real gating, never an invented list.
function M.verbs_for_object(world, obj_id)
    local verbs = { "esamina" }
    if world and world.get_object then
        local ok, obj = pcall(world.get_object, obj_id)
        if ok and type(obj) == "table" and type(obj.actions) == "table" then
            for action_name in pairs(obj.actions) do
                verbs[#verbs + 1] = action_name
            end
        end
    end
    return verbs
end

return M
