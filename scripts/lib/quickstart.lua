-- =============================================================================
--  quickstart.lua — declarative adventure bootstrap
--
--  Turns an adventure into a DATA TABLE. quick.define(spec) installs ALL the
--  script-contract globals (get_welcome_message, set_initial_state,
--  get_status_for_ai, get_system_prompt, get_json_schema, process_ai_response,
--  process_player_input, get_display_state, get_state_snapshot, restore_state,
--  get_tools, before_ai_turn, after_ai_turn, get_commands, image functions)
--  with correct wiring: persona registration order, agent init, NPC_DATA
--  rebuild after every reload, adv.set_npc_data propagation, save/restore.
--
--  Minimal complete adventure:
--
--      local quick = require("lib/quickstart")
--      quick.define{
--          name      = "mia_avventura",
--          path      = "./my_scripts/",
--          context   = "Una pensione a Napoli, estate 1987.",
--          locations = { hall={name="Hall", desc="..."}, ... },
--          travel    = { hall={"cucina"}, cucina={"hall"} },
--          start     = "hall",
--          npcs      = { rosa={ name="Rosa", age=55, personality="...",
--                               agent_system="Sono Rosa...", location="cucina" } },
--          welcome   = [[ ... testo intro ... ]],
--          prompt    = { header="Sei il narratore di ...", rules={"regola 1", "regola 2"} },
--      }
--
--  Everything else is optional. See scripts/template_min.lua for the full
--  commented spec reference.
--
--  ESCAPE HATCH: define() OVERWRITES the globals it installs. To customize one
--  function, define your own version AFTER the quick.define() call — the later
--  definition wins. Everything else keeps working.
--
--  SPEC FIELDS (all optional unless marked *):
--    name*        snake_case id — memory file, persona dir, image dir naming
--    path         base dir (default "./my_scripts/")
--    context      world context for persona/NPC generation
--    config       adventure.lua CFG overrides. Defaults: use_time=true,
--                 use_notes=true, use_agents/use_persona=true when npcs given,
--                 use_inventory=false, use_memory=false,
--                 generate_npcs=use_persona, mode="tools" ("schema" = MODE A:
--                 no tools, no workflow block — for local models),
--                 use_npc_tick=true when use_persona AND at least one npc has
--                 a routine — builds persona.npc_object + composed agents +
--                 NPC.tick wiring (living NPCs: they move with their routine,
--                 time jumps simulate instead of teleporting). Explicit
--                 use_npc_tick=false keeps prompt-only agents.
--                 session_npcs=true (default with use_persona): every NEW game
--                 forks the persona dir into npcs_<name>_sessions/<timestamp>/
--                 (persona.new_session) so in-game NPC evolution (life events,
--                 dreams, generated NPCs) does NOT bleed into the next game;
--                 saves record the session path and restore back into it.
--                 session_npcs=false = single shared dir (old behaviour).
--    days         day names list (→ CFG.days)
--    time         starting clock "HH:MM" (default "09:00")
--    locations*   { id = { name, desc, zone? } }
--    travel       { id = { reachable ids } }
--    start*       starting location id
--    npcs         { id = { name*, age, job, relationship, personality,
--                   agent_system ("%s" → player name), short_term_goals,
--                   long_term_goals, location, appearance, secret,
--                   routine = { {time_from="HH:MM", time_to="HH:MM",
--                                location_id="...", activity="...", stats?,
--                                outfit?, day?}, ... } (cover all 24h),
--                   model, provider, ... (any persona.register_static field) } }
--    welcome*     intro text
--    player       default player name (default "protagonista")
--    player_role  short role string shown to the LLM (e.g. "CEO di BioForma")
--    character_questions  questionnaire list (engine-driven, see template.lua)
--    build_appearance     fn(answers) → appearance string (override default)
--    arrival      true = default arrival scene | string = custom sys prompt
--    prompt*      { header* (string|fn(state)) — "{player}" is substituted,
--                   rules (string|list), blocks=fn(state)→string,
--                   workflow_extra_tools, workflow_extra_rules }
--    schema_extra { field = { type, enum?, description?, required?,
--                   on_set=fn(value, state) } } — extra MODE C schema fields;
--                   on_set fires when the LLM sets a non-nil, non-"" value
--    state_init   fn(state) — add adventure-specific state fields
--    status_extra fn(state) → table merged into get_status_for_ai output
--    on_response  fn(r, state) — after schema_extra; return a full result
--                 table to override the standard response
--    on_command   fn(input) → handled-table|nil — free-form command hook
--    on_restore   fn(state) — after restore_state rewiring
--    commands     { {cmd="/x [arg]", desc="...", fn=fn(rest, state)→string|table} }
--    tools        { tool defs } — adventure-specific tools (tools.lua format)
--    max_agent_calls   shared agent LLM budget per turn (default 3)
--    inventory / money initial values (when use_inventory)
--    images       { style, dir?, prompts={id=...}?, paths={id=...}? } —
--                 paths auto-derived: dir/bg_<loc>.jpg, dir/npc_<id>.jpg
--    hooks        { before_turn=fn(input,state), after_turn=fn(narr,reply,state) }
--    tick         fn(time, day, gidx, step) → adv.set_tick_fn
--    debug_fn     fn(state) → string appended to /debug
--    persona_path override persona dir (default path .. "npcs_<name>/")
--
--  Returns a handle: { adv, persona, state=fn, npc_data=fn, rebuild_npc_data=fn }.
-- =============================================================================

local json      = require("lib/json")
local adv       = require("lib/adventure")
local tools_lib = require("lib/tools")
local wlog      = require("lib/log")
require("lib/json_repair")

local M = {}

local function bail(msg)
    error("[quickstart] " .. msg
        .. "\nSpec di riferimento: read_knowledge(\"quickstart\") oppure "
        .. "read_file(\"scripts/template_min.lua\").", 0)
end

-- Tolerant boundary: mid models invent plausible-but-wrong spec dialects
-- (id instead of name, exits inside locations, config.start_location…).
-- Map the COMMON intuitive variants onto the real spec — with a warning so
-- the author learns the canonical field — and let validate_spec guide the rest.
local function normalize_spec(spec)
    if type(spec) ~= "table" then return spec end
    local warns = {}
    local function w(msg) table.insert(warns, msg) end
    local cfgt = type(spec.config) == "table" and spec.config or nil

    if not spec.name and type(spec.id) == "string" then
        spec.name = spec.id; w("spec.id → usa spec.name")
    end
    if not spec.start then
        local sl = spec.start_location or (cfgt and cfgt.start_location)
        if type(sl) == "string" then
            spec.start = sl; w("start_location → usa spec.start")
        end
    end
    if not spec.player then
        local pn = spec.protagonist_name or (cfgt and cfgt.protagonist_name)
        if type(pn) == "string" then
            spec.player = pn; w("protagonist_name → usa spec.player")
        end
    end
    if cfgt and type(cfgt.mode) == "string" then
        local m = cfgt.mode:lower()
        if m == "a" or m == "mode a" then cfgt.mode = "schema"
        elseif m == "b" or m == "c" or m == "mode b" or m == "mode c" then
            cfgt.mode = "tools"
        end
    end
    -- travel derived from locations[].exits (very common intuitive layout)
    if (type(spec.travel) ~= "table" or not next(spec.travel))
       and type(spec.locations) == "table" then
        local t, any = {}, false
        for id, loc in pairs(spec.locations) do
            if type(loc) == "table" and type(loc.exits) == "table" and #loc.exits > 0 then
                t[id] = loc.exits; any = true
            end
        end
        if any then
            spec.travel = t; w("locations[].exits → usa spec.travel")
        end
    end
    if (not spec.welcome or spec.welcome == "")
       and type(spec.description) == "string" and spec.description ~= "" then
        spec.welcome = spec.description .. "\n\nCome ti chiami?"
        w("description usata come welcome — scrivi spec.welcome")
    end
    if spec.time and not tostring(spec.time):match("^%d%d?:%d%d$") then
        w("spec.time '" .. tostring(spec.time) .. "' ignorato — formato \"HH:MM\"")
        spec.time = nil
    end
    -- narrator header buildable from title+description when missing
    if (type(spec.prompt) ~= "table" or not spec.prompt.header) then
        local ttl = (type(spec.title) == "string" and spec.title) or spec.name
        if type(spec.description) == "string" and spec.description ~= "" and ttl then
            spec.prompt = (type(spec.prompt) == "table") and spec.prompt or {}
            spec.prompt.header = "Sei il narratore di «" .. ttl .. "».\n"
                .. spec.description
                .. "\nSeconda persona, presente, 2-4 frasi. Protagonista: {player}."
            w("prompt.header costruito da title+description — scrivi spec.prompt.header")
        end
    end
    -- npc-level aliases: location ← home (when it's a real location id);
    -- secret ← secrets = {keys} resolved against a top-level notes map
    if type(spec.npcs) == "table" then
        for id, npc in pairs(spec.npcs) do
            if type(npc) == "table" then
                if not npc.location and type(npc.home) == "string"
                   and type(spec.locations) == "table" and spec.locations[npc.home] then
                    npc.location = npc.home
                    w("npcs." .. tostring(id) .. ".home → usa .location")
                end
                if (not npc.secret or npc.secret == "")
                   and type(npc.secrets) == "table" and #npc.secrets > 0 then
                    local parts = {}
                    for _, s in ipairs(npc.secrets) do
                        local resolved = type(spec.notes) == "table" and spec.notes[s]
                        table.insert(parts, type(resolved) == "string" and resolved or tostring(s))
                    end
                    npc.secret = table.concat(parts, " ")
                    w("npcs." .. tostring(id) .. ".secrets → usa .secret (stringa)")
                end
            end
        end
    end

    for _, msg in ipairs(warns) do wlog.warn("quickstart", "alias spec: " .. msg) end
    return spec
end

-- Fail fast at load with actionable messages (the CoderAI-facing boundary:
-- never strict-and-silent — every rejection states the fix).
local function validate_spec(spec)
    if type(spec) ~= "table" then
        bail('define(spec): spec deve essere una tabella')
    end
    if type(spec.name) ~= "string" or not spec.name:match("^[%w_]+$") then
        bail('spec.name obbligatorio, snake_case (es. name="mia_avventura")')
    end
    if type(spec.locations) ~= "table" or not next(spec.locations) then
        bail('spec.locations obbligatorio: { id = { name="...", desc="..." } }')
    end
    local ids = {}
    for id in pairs(spec.locations) do table.insert(ids, id) end
    table.sort(ids)
    local id_list = table.concat(ids, ", ")
    if type(spec.start) ~= "string" or not spec.locations[spec.start] then
        bail('spec.start deve essere una location esistente. Disponibili: ' .. id_list)
    end
    if type(spec.welcome) ~= "string" or spec.welcome == "" then
        bail('spec.welcome obbligatorio (testo mostrato prima della partita)')
    end
    if type(spec.prompt) ~= "table" or not spec.prompt.header then
        bail('spec.prompt.header obbligatorio (identità e tono del narratore)')
    end
    for src, exits in pairs(spec.travel or {}) do
        if not spec.locations[src] then
            bail("spec.travel: source '" .. tostring(src) .. "' non è in spec.locations. Disponibili: " .. id_list)
        end
        if type(exits) ~= "table" then
            bail("spec.travel." .. src .. " deve essere una lista di location id")
        end
        for _, dst in ipairs(exits) do
            if not spec.locations[dst] then
                bail("spec.travel: '" .. src .. "' → '" .. tostring(dst)
                    .. "' non è in spec.locations. Disponibili: " .. id_list)
            end
        end
    end
    for id, npc in pairs(spec.npcs or {}) do
        if type(npc) ~= "table" or not npc.name then
            bail("spec.npcs." .. tostring(id) .. ": campo name obbligatorio")
        end
        if not tostring(id):match("^[%w_]+$") then
            -- accents/spaces in ids survive, but the LLM will type the ascii
            -- variant in tool calls and miss — warn loudly, don't block
            wlog.warn("quickstart", "npc id '" .. tostring(id)
                .. "' non è snake_case ascii — usa es. '"
                .. tostring(id):gsub("[^%w_]", "") .. "' per id affidabili nei tool")
        end
        if npc.location and not spec.locations[npc.location] then
            bail("spec.npcs." .. id .. ": location '" .. tostring(npc.location)
                .. "' non è in spec.locations. Disponibili: " .. id_list)
        end
    end
    for field in pairs(spec.schema_extra or {}) do
        if field == "narration" or field == "game_over" or field == "game_over_reason" then
            bail("spec.schema_extra: '" .. field .. "' è un campo riservato dello schema base")
        end
    end
end

local DEFAULT_ARRIVAL_SYS = [[Sei il narratore di questa avventura. Scrivi la scena d'arrivo
del protagonista in seconda persona, 3-5 frasi, atmosferica. Descrivi come gli
altri presenti lo vedono in base al suo aspetto. Non inventare azioni del
giocatore: descrivi solo l'ambiente, l'arrivo e le prime reazioni.]]

function M.define(spec)
    spec = normalize_spec(spec)
    validate_spec(spec)

    spec.path   = spec.path or "./my_scripts/"
    spec.travel = spec.travel or {}
    spec.npcs   = spec.npcs or {}
    local has_npcs = next(spec.npcs) ~= nil

    -- ── CFG with defaults ────────────────────────────────────────────────
    local CFG = {}
    for k, v in pairs(spec.config or {}) do CFG[k] = v end
    if CFG.use_time      == nil then CFG.use_time      = true      end
    if CFG.use_notes     == nil then CFG.use_notes     = true      end
    if CFG.use_agents    == nil then CFG.use_agents    = has_npcs  end
    if CFG.use_persona   == nil then CFG.use_persona   = has_npcs  end
    if CFG.use_inventory == nil then CFG.use_inventory = false     end
    if CFG.use_memory    == nil then CFG.use_memory    = false     end
    if CFG.generate_npcs == nil then CFG.generate_npcs = CFG.use_persona end
    local any_routine = false
    for _, cfg in pairs(spec.npcs) do
        if type(cfg.routine) == "table" and #cfg.routine > 0 then any_routine = true end
    end
    if CFG.use_npc_tick == nil then
        CFG.use_npc_tick = (CFG.use_persona and CFG.use_agents and any_routine) or false
    end
    if CFG.use_npc_tick and not (CFG.use_persona and CFG.use_agents) then
        wlog.warn("quickstart", "use_npc_tick richiede use_persona+use_agents — disattivato")
        CFG.use_npc_tick = false
    end
    if CFG.session_npcs == nil then CFG.session_npcs = CFG.use_persona end
    CFG.days = spec.days or CFG.days
    local mode_tools = (CFG.mode ~= "schema")   -- MODE A opt-out
    if not mode_tools then
        -- no tools → no agents/persona-generation tool surface
        CFG.use_agents, CFG.generate_npcs = false, false
    end
    adv.set_config(CFG)

    -- ── libs on demand ───────────────────────────────────────────────────
    local memory_lib = nil
    if CFG.use_memory then
        memory_lib = require("lib/memory")
        memory_lib.init(spec.name, spec.path)
    end
    local persona = nil
    local persona_template = spec.persona_path or (spec.path .. "npcs_" .. spec.name .. "/")
    if CFG.use_persona then
        persona = require("lib/persona")
        persona.init(persona_template,
                     spec.context or ("Avventura: " .. spec.name))
    end

    -- ── NPC_DATA (rebuilt after every persona reload) ────────────────────
    local NPC_DATA = {}
    local function rebuild_npc_data()
        NPC_DATA = {}
        for id, cfg in pairs(spec.npcs) do
            local p = persona and persona.get(id) or nil
            -- Prefer npc_summary (dream-evolved "who they are TODAY") over the
            -- static personality — otherwise a dream changes the NPC's own
            -- dialogue voice (agent_object already prepends npc_summary) but
            -- the NARRATOR's view of them (this description, fed into
            -- prompt_npc_personalities) never catches up.
            local desc = (p and p.npc_summary and p.npc_summary ~= "" and p.npc_summary)
                      or (p and p.personality ~= "" and p.personality)
                      or cfg.personality or ""
            NPC_DATA[id] = {
                name         = (p and p.name) or cfg.name,
                age          = (p and p.age)  or cfg.age,
                relationship = cfg.relationship,
                description  = desc,
            }
        end
        adv.set_npc_data(NPC_DATA, spec.locations, spec.travel)
    end
    rebuild_npc_data()   -- tools built at startup must see static data at once

    -- fork_session=true (new game with session_npcs): register the spec NPCs
    -- into the TEMPLATE dir, then fork it into npcs_<name>_sessions/<ts>/ —
    -- in-game evolution stays in the session copy, the template stays pristine.
    -- Returns the active persona path (record it in state for restore).
    local function register_personas(fork_session)
        if not persona then return nil end
        if fork_session then persona.use_path(persona_template) end
        for id, cfg in pairs(spec.npcs) do persona.register_static(id, cfg) end
        if fork_session then persona.new_session(persona_template) end
        persona.reload_all()
        -- Cross-script bleed guard: if a PREVIOUS script (hot-swap) left this
        -- id in persona memory, register_static skipped the file write for
        -- THIS adventure's dir — and reload_all just wiped memory. Register
        -- again on the now-clean state so the file lands in the right folder.
        for id, cfg in pairs(spec.npcs) do
            if not persona.get(id) then persona.register_static(id, cfg) end
        end
        return persona.get_path()
    end

    -- ── living NPCs (use_npc_tick): npc.lua objects + composed agents ─────
    local npc_objects = {}

    local function make_world_adapter()
        return {
            getLocation = function(name)
                local s = adv.get_state()
                if not s then return nil end
                if name == "player" then return s.player.location end
                return s.npc_locations[name] or (s.gen_npc_locations or {})[name]
            end,
            setLocation = function(name, loc)
                local s = adv.get_state()
                if s then s.npc_locations[name] = loc end
            end,
            isInLocation = function(name, loc)
                local s = adv.get_state()
                if not s then return false end
                if name == "player" then return s.player.location == loc end
                return s.npc_locations[name] == loc
            end,
            countInLocation = function(loc)
                local s = adv.get_state()
                if not s then return 0 end
                local n = (s.player.location == loc) and 1 or 0
                for _, l in pairs(s.npc_locations or {}) do
                    if l == loc then n = n + 1 end
                end
                return n
            end,
            getAppearance = function(name)
                local s = adv.get_state()
                local o = s and s.npc_outfits and s.npc_outfits[name]
                if o and o ~= "" then return o end
                if persona and persona.current_outfit then
                    local po = persona.current_outfit(name, s and s.time, s and s.day)
                    if po and po ~= "" then return po end
                end
                return "NORMAL"
            end,
            setAppearance = function(name, o)
                local s = adv.get_state()
                if not s then return end
                s.npc_outfits = s.npc_outfits or {}
                s.npc_outfits[name] = o
            end,
            distance = function(a, b) return (a == b) and 0 or 2 end,
        }
    end

    -- One tick: routine/needs/sequences update + event dispatch; syncs
    -- locations/activities/stats/memories back into state (save/load).
    local function run_npc_tick()
        if not CFG.use_npc_tick or not next(npc_objects) then return end
        local s = adv.get_state()
        if not s then return end
        local NPC_lib = require("lib/npc")
        local ok, results = pcall(NPC_lib.tick, npc_objects,
            s.time or "09:00", s.day or "", s.player.location)
        if not ok then
            wlog.warn("quickstart", "NPC.tick: " .. tostring(results))
            return
        end
        s.npc_activities    = s.npc_activities or {}
        s.narrative_context = {}
        for id, r in pairs(results or {}) do
            if r.location then s.npc_locations[id] = r.location end
            s.npc_activities[id] = r.activity
            if r.narrative_hint then
                table.insert(s.narrative_context, "[" .. id .. "] " .. r.narrative_hint)
            end
        end
        s.npc_stats    = s.npc_stats or {}
        s.npc_memories = s.npc_memories or {}
        for id, nobj in pairs(npc_objects) do
            local st = {}
            for k, v in pairs(nobj.stats or {}) do st[k] = v end
            s.npc_stats[id]    = st
            s.npc_memories[id] = nobj.memory
        end

        -- Stage 2a: earn need+sequence from real stat crossings instead of
        -- dream free-invention. Submission is async/non-blocking — safe here.
        if persona and persona.check_pending_needs then
            local ok_needs, nerr = pcall(persona.check_pending_needs, npc_objects)
            if not ok_needs then
                wlog.warn("quickstart", "check_pending_needs: " .. tostring(nerr))
            end
        end

        -- Stage 3b: routine variations earned from WITNESSED repetition —
        -- only counts occurrences where the protagonist is co-located with
        -- the NPC (an off-screen repeat is invisible, costs nothing).
        if persona and persona.track_routine_variation then
            for id, nobj in pairs(npc_objects) do
                local witnessed = s.npc_locations[id] ~= nil
                               and s.npc_locations[id] == s.player.location
                local ok_var, verr = pcall(persona.track_routine_variation,
                    id, s.time or "09:00", s.day or "", witnessed, nobj)
                if not ok_var then
                    wlog.warn("quickstart", "track_routine_variation: " .. tostring(verr))
                end
            end
        end
    end

    -- A persona .lua file can exist on disk without ever having been placed:
    -- the two normal creation paths (declarative spec.npcs, or the in-game
    -- generate_npc tool) both auto-place the NPC, but a file written by hand
    -- or by CoderAI's write_file/str_replace tools skips that step entirely
    -- — reload_all() loads its DATA into the persona registry, but nothing
    -- ever adds it to gen_npc_locations, so it stays structurally invisible
    -- to the narrator (never listed in prompt_npc_positions / display_state)
    -- no matter how many times the script is reloaded. Adopt any such orphan
    -- here, right after the persona registry is (re)loaded: place it at its
    -- .home field if that resolves to a real location, else .workplace, else
    -- the adventure's start location — visible-but-approximate beats invisible.
    local function adopt_orphan_personas(state)
        if not persona or not state then return end
        state.gen_npc_locations = state.gen_npc_locations or {}
        for _, id in ipairs(persona.known_ids()) do
            if not NPC_DATA[id] and not state.gen_npc_locations[id]
               and not (state.npc_locations and state.npc_locations[id]) then
                local p = persona.get(id)
                local loc = (p and spec.locations[p.home] and p.home)
                         or (p and spec.locations[p.workplace] and p.workplace)
                         or spec.start
                state.gen_npc_locations[id] = loc
                wlog.warn("quickstart", "adopted orphan persona '" .. id
                    .. "' (file existed, never placed) -> " .. tostring(loc))
                if CFG.use_npc_tick and not npc_objects[id] then
                    local nobj = persona.npc_object(id, make_world_adapter())
                    if nobj then
                        npc_objects[id] = nobj
                        local ag = persona.agent_object(id, { npc = nobj })
                        if ag then adv.add_agent(id, ag) end
                    end
                end
            end
        end
    end

    local function init_agents()
        if not CFG.use_agents then return end
        local s = adv.get_state()
        local pname = (s and s.player and s.player.name) or "protagonista"

        if CFG.use_npc_tick then
            -- persona-backed composition: npc.lua object (routine/needs tick)
            -- + agent with npc_summary/secret/family/known_facts auto-injected
            npc_objects = {}
            local adapter = make_world_adapter()
            local agents_map = {}
            for id, cfg in pairs(spec.npcs) do
                local nobj = persona.npc_object(id, adapter)
                if nobj then
                    npc_objects[id] = nobj
                    local st = s and s.npc_stats and s.npc_stats[id]
                    if st then for k, v in pairs(st) do nobj.stats[k] = v end end
                    local mem = s and s.npc_memories and s.npc_memories[id]
                    if mem and #mem > 0 then nobj.memory = mem end
                else
                    wlog.warn("quickstart", "npc_object nil per '" .. id
                        .. "' — persona non caricata?")
                end
                local ag = persona.agent_object(id, {
                    npc = nobj, model = cfg.model, provider = cfg.provider,
                })
                if ag then
                    if ag.system then ag.system = ag.system:gsub("%%s", pname) end
                    agents_map[id] = ag
                end
            end
            -- Dynamically-generated NPCs (generate_npc tool, mid-game) get
            -- the SAME live composition when created (see get_tools' gen.fn
            -- wrapper) — but init_agents rebuilds everyone from scratch on
            -- every restore/reload, and this loop only ever covered
            -- spec.npcs. Without this, a generated NPC loses her npc_object/
            -- full agent the moment the game is saved and reloaded — she'd
            -- fall back to the bare lazy-built prompt-only agent (dialogue
            -- only, no tick, no stage 2a/2b/3b hooks).
            for id in pairs(s and s.gen_npc_locations or {}) do
                if not agents_map[id] then
                    local nobj = persona.npc_object(id, adapter)
                    if nobj then
                        npc_objects[id] = nobj
                        local st = s and s.npc_stats and s.npc_stats[id]
                        if st then for k, v in pairs(st) do nobj.stats[k] = v end end
                        local mem = s and s.npc_memories and s.npc_memories[id]
                        if mem and #mem > 0 then nobj.memory = mem end
                    end
                    local ag = persona.agent_object(id, { npc = nobj })
                    if ag then agents_map[id] = ag end
                end
            end
            adv.set_agents(agents_map, spec.max_agent_calls or 3)
            return
        end

        -- prompt-only agents from configs (no routine tick)
        local agents = {}
        for id, cfg in pairs(spec.npcs) do
            local p = persona and persona.get(id) or nil
            local sys = (p and p.agent_system and p.agent_system ~= "" and p.agent_system)
                        or cfg.agent_system or ""
            sys = sys:gsub("%%s", pname)
            agents[id] = {
                system           = sys,
                model            = cfg.model,
                provider         = cfg.provider,
                short_term_goals = (p and p.short_term_goals) or cfg.short_term_goals or {},
                long_term_goals  = (p and p.long_term_goals)  or cfg.long_term_goals  or {},
            }
        end
        adv.init_agents(agents, spec.max_agent_calls or 3)
    end

    -- ── initial state ────────────────────────────────────────────────────
    local function build_initial_state(player_input)
        local state = adv.default_state(CFG)
        if spec.time then state.time = spec.time end
        state.player.name     = spec.player or "protagonista"
        state.player.location = spec.start
        state.gen_npc_locations = {}
        for id, cfg in pairs(spec.npcs) do
            state.npc_locations[id] = cfg.location or spec.start
        end
        if CFG.use_npc_tick then
            state.npc_stats    = {}
            state.npc_memories = {}
            state.npc_outfits  = {}
            state.narrative_context = {}
        end
        if CFG.use_persona then
            state.last_dream = {}   -- { npc_id -> day_index_of_last_dream }
        end
        if CFG.use_inventory then
            state.inventario = {}
            for _, v in ipairs(spec.inventory or {}) do table.insert(state.inventario, v) end
            state.soldi = spec.money or 0
        end

        -- questionnaire answers (JSON blob) or bare name — handle both
        local answers = nil
        if player_input and player_input:match("^%s*{") then
            local ok, dec = pcall(json.decode, player_input)
            if ok and type(dec) == "table" then answers = dec end
        end
        if answers then
            if answers.name and answers.name ~= "" then state.player.name = answers.name end
            if spec.build_appearance then
                state.player.appearance = spec.build_appearance(answers) or ""
            else
                local parts = {}
                for _, q in ipairs(spec.character_questions or {}) do
                    local v = answers[q.field]
                    if v and v ~= "" and q.field ~= "name" then
                        table.insert(parts, q.field .. ": " .. tostring(v))
                    end
                end
                state.player.appearance = table.concat(parts, ", ")
            end
            state.player.outfit = answers.vestiti or answers.outfit or ""
        elseif player_input and player_input ~= "" then
            state.player.name = player_input
        end

        if spec.state_init then spec.state_init(state) end
        return state
    end

    -- ── schema (built once) ──────────────────────────────────────────────
    local schema_json
    do
        local props = {
            narration        = { type = "string" },
            game_over        = { type = "boolean" },
            game_over_reason = { type = "string",
                description = "Motivo vittoria/sconfitta. Stringa vuota finché il gioco continua." },
        }
        local required = { "narration" }
        for field, def in pairs(spec.schema_extra or {}) do
            local copy = {}
            for k, v in pairs(def) do
                if k ~= "on_set" and k ~= "required" then copy[k] = v end
            end
            props[field] = copy
            if def.required then table.insert(required, field) end
        end
        schema_json = json.encode({ type = "object", required = required, properties = props })
    end

    -- ── commands normalization ───────────────────────────────────────────
    local commands_list = {}
    for _, c in ipairs(spec.commands or {}) do
        local key = type(c.cmd) == "string" and c.cmd:match("^(/[%w_]+)") or nil
        if key then
            table.insert(commands_list, { key = key, cmd = c.cmd, desc = c.desc or "", fn = c.fn })
        else
            wlog.warn("quickstart", "comando ignorato: cmd deve iniziare con /nome — "
                .. tostring(c.cmd))
        end
    end

    -- =====================================================================
    -- GLOBALS (overwrite semantics: script overrides AFTER quick.define win)
    -- =====================================================================
    local function install(name, fn) _G[name] = fn end

    install("get_welcome_message", function() return spec.welcome end)

    if spec.character_questions then
        install("get_character_questions", function() return spec.character_questions end)
    end

    install("set_initial_state", function(player_input)
        local state = build_initial_state(player_input)
        local ppath = register_personas(CFG.session_npcs)
        if ppath and CFG.session_npcs then
            state._persona_path = ppath   -- saved with the snapshot; adv.restore switches back
        end
        rebuild_npc_data()
        adopt_orphan_personas(state)
        adv.set_state(state)
        init_agents()
        run_npc_tick()   -- place NPCs on their routine slot for the start time
    end)

    install("generate_initial_state", function() _G.set_initial_state("") end)

    if spec.arrival then
        install("generate_arrival", function()
            local s = adv.get_state()
            if not s then return "" end
            local sys = type(spec.arrival) == "string" and spec.arrival or DEFAULT_ARRIVAL_SYS
            local loc = spec.locations[s.player.location] or {}
            local present = {}
            for id, l in pairs(s.npc_locations or {}) do
                if l == s.player.location and NPC_DATA[id] then
                    table.insert(present, NPC_DATA[id].name .. " — "
                        .. (NPC_DATA[id].description or ""))
                end
            end
            local ap = adv.player_appearance()
            local user = "Protagonista: " .. (ap ~= "" and ap or s.player.name)
                .. "\nLuogo: " .. (loc.name or s.player.location) .. " — " .. (loc.desc or "")
                .. "\nPresenti: " .. (#present > 0 and table.concat(present, "; ") or "nessuno")
            local schema = [[{ "type":"object", "required":["narration"],
                               "properties":{ "narration":{"type":"string"} } }]]
            local ok, reply = pcall(query_llm, sys, "[]", user, schema, nil, nil, "narrator")
            if not ok then return "" end
            local okd, data = pcall(json.decode, reply)
            if okd and type(data) == "table" and not data.error and data.narration then
                return data.narration
            end
            return ""   -- LLM failure → skip arrival, never leak raw errors
        end)
    end

    install("get_status_for_ai", function()
        local s = adv.get_state()
        if not s then return "{}" end
        local loc = spec.locations[s.player.location] or {}
        local present = {}
        for id, l in pairs(s.npc_locations or {}) do
            if l == s.player.location then
                local npc = NPC_DATA[id]
                if npc then
                    table.insert(present, {
                        id = id, name = npc.name, age = npc.age,
                        relationship = npc.relationship,
                        activity = (s.npc_activities or {})[id],
                    })
                end
            end
        end
        local status = {
            player = { name = s.player.name, role = spec.player_role,
                       location = loc.name or s.player.location,
                       appearance = s.player.appearance },
            location = { id = s.player.location, name = loc.name or s.player.location,
                         desc = loc.desc or "", zone = loc.zone,
                         exits = spec.travel[s.player.location] or {} },
            npcs_present = present,
            turn = s.turn,
        }
        if CFG.use_time then
            status.time = s.time; status.day = s.day; status.giorno_index = s.giorno_index
        end
        if CFG.use_inventory then
            status.inventario = s.inventario; status.soldi = s.soldi
        end
        if persona then
            local gen_present, gen_known = {}, {}
            for npc_id, gloc in pairs(s.gen_npc_locations or {}) do
                if gloc == s.player.location then
                    local p = persona.get(npc_id)
                    if p then
                        table.insert(gen_present, { id = npc_id, name = p.name,
                                                    job = p.job, age = p.age })
                    end
                end
            end
            for _, npc_id in ipairs(persona.known_ids()) do
                local p = persona.get(npc_id)
                if p and not spec.npcs[npc_id] then
                    -- A bare "?" here read as "unknown location" rather than
                    -- "she exists but was never placed" — a real bug this
                    -- caused: the narrator didn't realize she could just be
                    -- moved in, and generated a DUPLICATE character instead
                    -- (e.g. a CoderAI-created NPC never touched by
                    -- generate_npc). Spell out the actionable fix instead.
                    local loc = (s.gen_npc_locations or {})[npc_id]
                    table.insert(gen_known, { id = npc_id, name = p.name, job = p.job,
                        age = p.age,
                        location = loc or "NON PIAZZATO — esiste già, usa move_npc(id, location) "
                                        .. "per posizionarlo, NON generate_npc di nuovo" })
                end
            end
            if #gen_present > 0 then status.gen_npcs_present = gen_present end
            if #gen_known   > 0 then status.gen_npcs_known   = gen_known   end
        end
        if spec.status_extra then
            local ok, extra = pcall(spec.status_extra, s)
            if ok and type(extra) == "table" then
                for k, v in pairs(extra) do status[k] = v end
            elseif not ok then
                wlog.warn("quickstart", "status_extra: " .. tostring(extra))
            end
        end
        return json.encode(status)
    end)

    install("get_system_prompt", function()
        local s = adv.get_state()
        if not s then return spec.prompt.header end
        local loc_id = s.player.location
        local loc    = spec.locations[loc_id] or {}

        local header = spec.prompt.header
        if type(header) == "function" then header = header(s) or "" end
        header = header:gsub("{player}", s.player.name or "")

        local rules = spec.prompt.rules or ""
        if type(rules) == "table" then
            local lines = {}
            for i, r in ipairs(rules) do table.insert(lines, i .. ". " .. r) end
            rules = "\n\n## REGOLE\n" .. table.concat(lines, "\n")
        elseif rules ~= "" then
            rules = "\n\n## REGOLE\n" .. rules
        end

        local location_block = string.format("\n\n## LOCATION: %s\n%s",
            loc.name or loc_id, loc.desc or "")

        local appearance_block = ""
        local ap = adv.player_appearance()
        if ap ~= "" then
            appearance_block = "\n\n## ASPETTO PROTAGONISTA (visibile agli NPC)\n" .. ap
        end

        -- generated NPCs: personality blocks for present, roster for known
        local gen_present_block, gen_known_block = "", ""
        if persona then
            local blocks = {}
            for npc_id, gloc in pairs(s.gen_npc_locations or {}) do
                if gloc == loc_id then
                    local blk = persona.format(npc_id)
                    if blk and blk ~= "" then
                        local p = persona.get(npc_id)
                        table.insert(blocks, string.format("=== %s [id:%s] ===\n%s",
                            p and p.name or npc_id, npc_id, blk))
                    end
                end
            end
            if #blocks > 0 then
                gen_present_block = "\n\n## NPC GENERATI PRESENTI\n"
                    .. table.concat(blocks, "\n\n")
            end
            local lines = {}
            for _, npc_id in ipairs(persona.known_ids()) do
                local p = persona.get(npc_id)
                if p and not spec.npcs[npc_id] then
                    table.insert(lines, string.format("  %s [id:%s] — %s, %d anni — %s",
                        p.name, npc_id, p.job or "?", p.age or 0,
                        (s.gen_npc_locations or {})[npc_id] or "?"))
                end
            end
            if #lines > 0 then
                gen_known_block = "\n\nNPC SECONDARI GIÀ NEL SISTEMA (NON rigenerare — usa id esatto):\n"
                    .. table.concat(lines, "\n")
            end
        end

        -- NPC tick narrative hints (event reactions, sequence beats)
        local hints_block = ""
        if s.narrative_context and #s.narrative_context > 0 then
            hints_block = "\n\n## CONTESTO NPC (intreccia nella narrazione)\n"
                .. table.concat(s.narrative_context, "\n")
        end

        local extra = ""
        if spec.prompt.blocks then
            local ok, b = pcall(spec.prompt.blocks, s)
            if ok and type(b) == "string" then extra = b
            elseif not ok then wlog.warn("quickstart", "prompt.blocks: " .. tostring(b)) end
        end

        local workflow = ""
        if mode_tools then
            local gen_lines = ""
            if CFG.generate_npcs then
                gen_lines = "    [opt] generate_npc(id, context)   — primo incontro con persona NUOVA. "
                    .. "id snake_case stabile. MAX 2/turno.\n"
                    .. "    [opt] npc_life_event(id, ...)      — evento permanente nella vita di un NPC."
            end
            local extra_tools = gen_lines
            if spec.prompt.workflow_extra_tools and spec.prompt.workflow_extra_tools ~= "" then
                extra_tools = (extra_tools ~= "" and (extra_tools .. "\n") or "")
                    .. spec.prompt.workflow_extra_tools
            end
            workflow = adv.prompt_workflow(extra_tools, spec.prompt.workflow_extra_rules)
        end

        return header .. rules .. location_block
            .. appearance_block
            .. adv.prompt_exits(spec.travel, spec.locations)
            .. adv.prompt_npc_positions(NPC_DATA)
            .. adv.prompt_npc_personalities(NPC_DATA)
            .. gen_present_block .. gen_known_block
            .. hints_block
            .. extra
            .. adv.prompt_notes()
            .. adv.prompt_events()
            .. adv.prompt_pending_event()
            .. workflow
    end)

    install("get_json_schema", function() return schema_json end)

    install("process_ai_response", function(reply)
        local r, err = adv.parse_reply(reply)
        if not r then return err end
        local s = adv.get_state()

        -- schema_extra handlers: fire on non-nil, non-"" values
        for field, def in pairs(spec.schema_extra or {}) do
            local v = r[field]
            if def.on_set and v ~= nil and v ~= "" then
                local ok, e = pcall(def.on_set, v, s)
                if not ok then
                    wlog.warn("quickstart", "schema_extra." .. field .. ".on_set: " .. tostring(e))
                end
            end
        end

        if spec.on_response then
            local ok, override = pcall(spec.on_response, r, s)
            if not ok then
                wlog.warn("quickstart", "on_response: " .. tostring(override))
            elseif type(override) == "table" then
                s.turn = s.turn + 1
                return override
            end
        end

        s.turn = s.turn + 1
        local narr = r.narration or r.narrative
        if r.game_over == true then
            return adv.response_ok(narr, true, r.game_over_reason)
        end
        return adv.response_ok(narr)
    end)

    install("process_player_input", function(input)
        local handled = adv.handle_input(input, NPC_DATA, spec.locations, spec.travel)
        if handled then return handled end
        local trimmed = (input:match("^%s*(.-)%s*$") or input)
        local cmd, rest = trimmed:match("^(/[%w_]+)%s*(.*)")
        if cmd then
            for _, c in ipairs(commands_list) do
                if c.key == cmd and c.fn then
                    local ok, out = pcall(c.fn, rest or "", adv.get_state())
                    if not ok then
                        return { success = true, handled = true,
                                 output = "Errore comando " .. cmd .. ": " .. tostring(out) }
                    end
                    if type(out) == "table" then return out end
                    return { success = true, handled = true, output = tostring(out or "") }
                end
            end
        end
        if spec.on_command then
            local res = spec.on_command(input)
            if res then return res end
        end
        return { success = true, handled = false }
    end)

    install("get_display_state", function()
        local extra = nil
        if spec.hud then
            local ok, h = pcall(spec.hud, adv.get_state())
            if ok then extra = h end
        end
        return adv.display_state(NPC_DATA, spec.locations, extra)
    end)

    install("get_state_snapshot", function() return adv.snapshot() end)

    install("restore_state", function(snapshot)
        -- persona files must be loaded BEFORE agents are rebuilt (fresh-process
        -- load: persona._npcs is empty until register/reload runs)
        local data, result = adv.restore(snapshot, function()
            register_personas()
            rebuild_npc_data()
            init_agents()
        end)
        if not data then return result end
        if not data.gen_npc_locations then data.gen_npc_locations = {} end
        if CFG.use_persona and not data.last_dream then data.last_dream = {} end
        -- A static NPC added to spec.npcs AFTER a save was created has no
        -- entry in that save's npc_locations (only build_initial_state, on a
        -- brand-new game, seeds it from cfg.location/.home) — she'd otherwise
        -- stay invisible on every future restore no matter how many times the
        -- script is reloaded, since restore only rehydrates what the snapshot
        -- already had.
        data.npc_locations = data.npc_locations or {}
        for id, cfg in pairs(spec.npcs) do
            if not data.npc_locations[id] then
                data.npc_locations[id] = cfg.location or spec.start
                wlog.warn("quickstart", "backfilled missing npc_locations for '" .. id
                    .. "' (added to spec.npcs after this save existed) -> "
                    .. tostring(data.npc_locations[id]))
            end
        end
        adopt_orphan_personas(data)
        adv.set_state(data)
        if spec.on_restore then
            local ok, e = pcall(spec.on_restore, data)
            if not ok then wlog.warn("quickstart", "on_restore: " .. tostring(e)) end
        end
        return result
    end)

    install("get_commands", function()
        local out = {}
        table.insert(out, { cmd = "/map",   desc = "Uscite dalla location attuale" })
        if has_npcs then table.insert(out, { cmd = "/npcs", desc = "Posizioni NPC" }) end
        if CFG.use_time      then table.insert(out, { cmd = "/time",  desc = "Ora e giorno" })           end
        if CFG.use_inventory then table.insert(out, { cmd = "/inv",   desc = "Inventario e contanti" })  end
        if CFG.use_notes     then table.insert(out, { cmd = "/notes", desc = "Note personali" })         end
        table.insert(out, { cmd = "/debug",    desc = "Stato completo" })
        table.insert(out, { cmd = "/validate", desc = "Lint del mondo (mappa, NPC, routine)" })
        for _, c in ipairs(commands_list) do
            table.insert(out, { cmd = c.cmd, desc = c.desc })
        end
        return out
    end)

    if mode_tools then
        install("get_tools", function()
            local extra = {}
            if persona and CFG.generate_npcs then
                local gen = persona.as_tool_generate(spec.generate_npc_desc
                    or ("Genera un NPC secondario incontrato per la prima volta. "
                     .. "Usa id snake_case stabile (es. 'carlo_barista'). "
                     .. "L'NPC viene posizionato automaticamente nella location attuale. MAX 2/turno."))
                local base_fn = gen.fn
                gen.fn = function(args_json)
                    local result_json = base_fn(args_json)
                    local ok, result = pcall(json.decode, result_json)
                    -- generate_npc(id=<existing>) returns ok=true+already_exists=true
                    -- (idempotent, no new file written) — but this wrapper used to
                    -- ALWAYS re-place result.id at the player's current location
                    -- regardless, even for a STATIC npc who already has a real home
                    -- in npc_locations. Two real bugs from that: (a) a static NPC
                    -- ends up registered in BOTH npc_locations AND gen_npc_locations
                    -- — every listing that merges both (e.g. /npcs) shows her twice
                    -- under the same id; (b) re-calling generate_npc on an existing
                    -- GENERATED npc silently teleports her to wherever the player
                    -- happens to be. Only place her here on a genuinely NEW creation.
                    if ok and type(result) == "table" and result.ok and result.id
                       and not result.already_exists and not NPC_DATA[result.id] then
                        local s = adv.get_state()
                        if s then
                            s.gen_npc_locations = s.gen_npc_locations or {}
                            s.gen_npc_locations[result.id] = s.player.location
                        end
                        -- Give her the SAME live composition static NPCs get
                        -- (npc_object tied to NPC.tick + full agent_object,
                        -- not the bare lazy-built prompt-only fallback
                        -- think_as_npc otherwise constructs on the fly) —
                        -- without this a generated NPC can talk but never
                        -- act autonomously, and stage 3 crystallization
                        -- would write a routine that nothing ever ticks.
                        if CFG.use_npc_tick then
                            local nobj = persona.npc_object(result.id, make_world_adapter())
                            if nobj then
                                npc_objects[result.id] = nobj
                                local ag = persona.agent_object(result.id, { npc = nobj })
                                if ag then adv.add_agent(result.id, ag) end
                            end
                        end
                    end
                    return result_json
                end
                table.insert(extra, gen)
                table.insert(extra, persona.as_tool_life_event(spec.life_event_desc
                    or ("Registra un evento importante e permanente nella vita di un NPC "
                     .. "(cambio lavoro, trauma, svolta di relazione). Persiste su disco. "
                     .. "Solo per fatti realmente accaduti in gioco.")))
            end
            for _, t in ipairs(spec.tools or {}) do
                if type(t) == "table" then
                    -- tolerate OpenAI-style tool defs: parameters → params,
                    -- table schema → JSON string (tools.build wants a string)
                    if t.params == nil and t.parameters ~= nil then
                        t.params = t.parameters
                    end
                    if type(t.params) == "table" then t.params = json.encode(t.params) end
                    if type(t.fn) ~= "function" then
                        wlog.warn("quickstart", "tool '" .. tostring(t.name)
                            .. "' senza fn — ignorato (serve fn = function(args_json) ... end)")
                    else
                        table.insert(extra, t)
                    end
                end
            end
            return tools_lib.build(adv.get_tools(NPC_DATA, spec.locations, spec.travel,
                                                 memory_lib, extra))
        end)
    end

    install("before_ai_turn", function(player_input)
        local r = adv.before_turn(player_input)
        run_npc_tick()   -- routine/needs/sequences beat + state sync
        if spec.hooks and spec.hooks.before_turn then
            local ok, hr = pcall(spec.hooks.before_turn, player_input, adv.get_state())
            if not ok then
                wlog.warn("quickstart", "hooks.before_turn: " .. tostring(hr))
            elseif type(hr) == "table" then
                return hr   -- allow { skip_llm=true, narration="..." }
            end
        end
        return r
    end)

    -- Nightly dream tick: fires from after_ai_turn (matches the timing every
    -- hand-written adventure using dreams already uses — pensione_riva.lua).
    -- Two cases: sleep_until crossed the 01:00-05:00 window → the WHOLE cast
    -- dreams at once (adv.get_tools' sleep_until sets state._dream_due_day);
    -- otherwise, one NPC dreams per turn IF the clock is currently inside
    -- that window (M.dream_tick's own internal guard).
    local function run_dream_tick()
        if not (CFG.use_persona and persona) then return end
        local s = adv.get_state()
        if not s then return end
        s.last_dream = s.last_dream or {}
        local dreamed = false
        if s._dream_due_day then
            local day = s._dream_due_day
            s._dream_due_day = nil
            local list = persona.dream_tick_all(day, s.last_dream, nil)
            dreamed = (#list > 0)
        elseif CFG.use_time then
            local r = persona.dream_tick(s.time, s.giorno_index or 1, s.last_dream, nil)
            dreamed = (r ~= nil)
        end
        if dreamed then rebuild_npc_data() end   -- npc_summary may have changed
    end

    install("after_ai_turn", function(narration, raw_reply)
        adv.after_turn(narration, raw_reply)
        run_dream_tick()
        if spec.hooks and spec.hooks.after_turn then
            local ok, e = pcall(spec.hooks.after_turn, narration, raw_reply, adv.get_state())
            if not ok then wlog.warn("quickstart", "hooks.after_turn: " .. tostring(e)) end
        end
    end)

    -- ── image system (optional) ──────────────────────────────────────────
    if spec.images then
        local img = spec.images
        local dir = img.dir or ("images/" .. spec.name .. "/")
        local paths = {}
        for id in pairs(spec.locations) do paths[id] = dir .. "bg_" .. id .. ".jpg" end
        for id in pairs(spec.npcs)      do paths[id] = dir .. "npc_" .. id .. ".jpg" end
        for id, p in pairs(img.paths or {}) do paths[id] = p end

        local function prompt_for(id)
            if img.prompts and img.prompts[id] then return img.prompts[id] end
            local loc = spec.locations[id]
            if loc then return (loc.name or id) .. ". " .. (loc.desc or "") end
            local npc = spec.npcs[id]
            if npc then
                local p   = persona and persona.get(id) or nil
                local app = (p and p.appearance and p.appearance ~= "" and p.appearance)
                            or npc.appearance
                local base = (app and app ~= "" and app)
                             or (npc.name .. ", " .. (npc.personality or ""))
                return base .. ", half-body portrait"
            end
            return id
        end

        install("get_scene_images", function()
            local s = adv.get_state()
            if not s then return {} end
            local loc_id = s.player.location
            local assets = {}
            if paths[loc_id] then table.insert(assets, { id = loc_id, path = paths[loc_id] }) end
            for id, l in pairs(s.npc_locations or {}) do
                if l == loc_id and paths[id] then
                    table.insert(assets, { id = id, path = paths[id] })
                end
            end
            local hint = (s._last_image_loc == loc_id) and "last" or nil
            s._last_image_loc = loc_id
            return { assets = assets, base_image = hint }
        end)

        install("get_asset_path", function(id) return paths[id] end)

        install("get_asset_prompt", function(id)
            local path = paths[id]
            if not path then return nil end
            local desc = prompt_for(id)
            local sys = "You are an image generation prompt engineer. "
                     .. "Convert the description into a detailed txt2img prompt. "
                     .. "Max 80 words. English only. Output the prompt text only."
            local ok, prompt = pcall(query_llm, sys, "[]",
                "Description: " .. desc .. "\nStyle: " .. (img.style or ""), "")
            return { path = path,
                     prompt = ok and prompt or (desc .. ", " .. (img.style or "")) }
        end)

        install("get_image_style", function() return img.style or "" end)
    end

    -- ── optional wiring ──────────────────────────────────────────────────
    if spec.debug_fn then
        adv.set_debug_fn(function() return spec.debug_fn(adv.get_state()) or "" end)
    end
    if spec.tick then
        adv.set_tick_fn(spec.tick)          -- explicit tick wins (advanced)
    elseif CFG.use_npc_tick then
        -- time jumps (advance_time/sleep_until) simulate the routine step by
        -- step instead of teleporting NPCs to the final slot
        adv.set_tick_fn(function(time_str, day_str, gidx, step)
            if not next(npc_objects) then return end
            local NPC_lib = require("lib/npc")
            local s = adv.get_state()
            local ok, err = pcall(NPC_lib.tick, npc_objects,
                time_str, day_str, s and s.player.location)
            if not ok then wlog.warn("quickstart", "tick_fn: " .. tostring(err)) end
        end)
    end

    -- routine sanity: unknown location ids won't crash the tick, but the NPC
    -- ends up in a room the player can never reach — warn early
    if CFG.use_npc_tick then
        for id, cfg in pairs(spec.npcs) do
            for _, slot in ipairs(cfg.routine or {}) do
                local lid = slot.location_id or slot.location
                if lid and not spec.locations[lid] then
                    wlog.warn("quickstart", "npcs." .. id .. ".routine: location '"
                        .. tostring(lid) .. "' non in spec.locations")
                end
            end
        end
    end

    return {
        adv              = adv,
        persona          = persona,
        state            = adv.get_state,
        npc_data         = function() return NPC_DATA end,
        npc_objects      = function() return npc_objects end,
        rebuild_npc_data = rebuild_npc_data,
    }
end

return M
