-- =============================================================================
--  template_min.lua — minimal adventure via lib/quickstart (RECOMMENDED START)
--
--  The whole adventure is ONE data table: quick.define(spec) installs every
--  script-contract function (state, prompt, schema, tools, save/load, HUD)
--  with the correct wiring. You only write DATA + adventure-specific logic.
--
--  1. Copy to my_scripts/my_adventure.lua  (private scripts stay in my_scripts/)
--  2. Replace the data below (locations, npcs, welcome, prompt)
--  3. Run: ./build/rpgai --web --path my_scripts/ --script my_adventure.lua
--          [+ provider flags, e.g. --provider openrouter --or-key ... --or-model ...]
--  4. In game: /validate checks the world; /debug shows full state.
--
--  Need more control? Any global you define AFTER quick.define() overrides the
--  installed one. For advanced features (world.lua procedural locations,
--  npc.lua routines, VN mode) start from scripts/template.lua instead.
-- =============================================================================

local quick = require("lib/quickstart")

quick.define{

    -- ── identity ─────────────────────────────────────────────────────────
    name    = "template_min",          -- snake_case: names persona dir, memory file
    path    = "./scripts/",            -- base dir ("./my_scripts/" for private)
    context = "Un piccolo borgo di mare italiano, estate. Tono leggero, realistico.",

    -- ── features (defaults shown; delete what you don't change) ──────────
    config = {
        use_time      = true,          -- clock + advance_time/sleep_until tools
        use_notes     = true,          -- remember tool (player/public/npc scopes)
        use_inventory = false,         -- cambia_inventario tool + HUD line
        use_memory    = false,         -- memory_write/read cross-session tools
        -- use_agents / use_persona default to true because npcs is non-empty
        -- mode = "schema",            -- MODE A: no tools (local models)
    },
    days = { "lunedì","martedì","mercoledì","giovedì","venerdì","sabato","domenica" },
    time = "10:00",

    -- ── world ────────────────────────────────────────────────────────────
    locations = {
        piazza = { name = "Piazza del Borgo",
                   desc = "Piazza assolata con una fontana, il bar di Gino sotto i portici." },
        bar    = { name = "Bar di Gino",
                   desc = "Bancone in legno, macchina del caffè che sbuffa, tre tavolini." },
        molo   = { name = "Molo",
                   desc = "Barche da pesca ormeggiate, odore di salsedine, reti stese." },
    },
    travel = {
        piazza = { "bar", "molo" },
        bar    = { "piazza" },
        molo   = { "piazza" },
    },
    start = "piazza",

    -- ── NPCs (persona-backed: first run writes npcs_<name>/<id>.lua,
    --          then the file on disk is authoritative and evolves) ─────────
    npcs = {
        gino = {
            name         = "Gino",
            age          = 58,
            job          = "barista",
            location     = "bar",
            personality  = "Barista da trent'anni, burbero ma di cuore. Sa tutto di tutti "
                        .. "e lo lascia intendere senza mai dirlo davvero.",
            agent_system = "Sono Gino, 58 anni, barista del borgo. Burbero, ironico, "
                        .. "osservatore. Non do mai un'informazione gratis. "
                        .. "Prima persona, MAX 3 frasi.",
            short_term_goals = { "capire chi è il nuovo arrivato" },
            long_term_goals  = { "proteggere la tranquillità del borgo" },
            -- routine → NPC "vivo": si muove da solo, i salti di tempo lo
            -- simulano (use_npc_tick automatico). Coprire tutte le 24h.
            routine = {
                { time_from="06:00", time_to="22:00", location_id="bar",
                  activity="dietro il bancone, serve caffè e ascolta tutto" },
                { time_from="22:00", time_to="06:00", location_id="bar",
                  activity="dorme nel retro del bar" },
            },
        },
        anna = {
            name         = "Anna",
            age          = 31,
            job          = "pescatrice",
            location     = "molo",
            personality  = "Ha ereditato la barca del padre. Pratica, diretta, diffidente "
                        .. "con i turisti ma leale con chi si guadagna la sua fiducia.",
            agent_system = "Sono Anna, 31 anni, pescatrice. Diretta, concreta, poca "
                        .. "pazienza per le chiacchiere. Prima persona, MAX 3 frasi.",
            short_term_goals = { "riparare il motore della barca" },
            long_term_goals  = { "non dover vendere la barca di famiglia" },
            routine = {
                { time_from="05:00", time_to="14:00", location_id="molo",
                  activity="sistema le reti e scarica il pescato" },
                { time_from="14:00", time_to="16:00", location_id="bar",
                  activity="pranzo tardi e un bicchiere da Gino" },
                { time_from="16:00", time_to="05:00", location_id="molo",
                  activity="lavora sulla barca, poi dorme a bordo" },
            },
        },
    },

    -- ── intro ────────────────────────────────────────────────────────────
    welcome = [[
══════════════════════════════════════════════════════
  IL BORGO — demo minimale
══════════════════════════════════════════════════════

Sei appena arrivato in un piccolo borgo di mare.
Nessuno ti conosce. Tutti ti osservano.

Come ti chiami?  (Invio per giocare come "protagonista")
]],

    arrival = true,   -- LLM narrates the arrival scene as turn 0 (delete to skip)

    -- ── narrator ─────────────────────────────────────────────────────────
    prompt = {
        header = "Sei il narratore di «Il Borgo», avventura contemporanea realistica.\n"
              .. "Il protagonista è {player}, appena arrivato nel borgo.\n"
              .. "Seconda persona, presente. Vivido ma conciso (2-4 frasi).",
        rules = {
            "Il protagonista agisce SOLO su input del giocatore.",
            "Non inventare luoghi o persone oltre a quelli definiti (o genera NPC col tool).",
            "Gli abitanti sono diffidenti: la fiducia si guadagna in più scene.",
        },
        -- blocks = function(state) return "\n\n## EXTRA\n..." end,
        -- workflow_extra_tools = "    [opt] my_tool(...) — quando usarlo. MAX 1/turno.",
    },

    -- ── extra schema fields (MODE C: simple atomic outcomes) ─────────────
    schema_extra = {
        fiducia_borgo = {
            type = "integer", minimum = -2, maximum = 2,
            description = "Variazione della fiducia del borgo verso il protagonista "
                       .. "questo turno (0 = invariata).",
            on_set = function(v, state)
                if type(v) == "number" and v ~= 0 then
                    state.fiducia = math.max(0, math.min(100, (state.fiducia or 20) + v))
                end
            end,
        },
    },

    -- ── adventure-specific state ─────────────────────────────────────────
    state_init = function(state)
        state.fiducia = 20   -- 0-100: quanto il borgo si fida
    end,
    status_extra = function(state)
        return { fiducia_borgo = state.fiducia }
    end,
    hud = function(state)
        return "Fiducia borgo: " .. (state.fiducia or 0) .. "/100"
    end,

    -- ── custom /commands ─────────────────────────────────────────────────
    commands = {
        { cmd = "/fiducia", desc = "Mostra la fiducia del borgo",
          fn = function(rest, state)
              return "Fiducia del borgo: " .. (state.fiducia or 0) .. "/100"
          end },
    },

    -- ── adventure-specific tools (think_as_npc, move_player, move_npc,
    --    advance_time, remember, generate_npc... are already installed) ────
    -- tools = {
    --     { name = "my_tool", description = "...",
    --       params = [[{ "type":"object", "required":["x"],
    --                    "properties":{ "x":{"type":"string"} } }]],
    --       fn = function(args_json)                   -- args_json is a STRING
    --           local json = require("lib/json")
    --           local a = json.decode(args_json)
    --           return json.encode({ ok = true })
    --       end },
    -- },

    -- ── images (optional: paths auto-derived bg_<loc>.jpg / npc_<id>.jpg) ─
    -- images = { style = "photorealistic, italian seaside village, golden hour",
    --            dir   = "images/template_min/" },

    -- ── hooks (optional) ─────────────────────────────────────────────────
    -- hooks = {
    --     after_turn = function(narration, raw_reply, state) ... end,
    -- },
}

-- Any global defined BELOW this line overrides the installed one:
-- function get_display_state() return "custom HUD" end
