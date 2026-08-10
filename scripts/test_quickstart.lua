-- test_quickstart.lua — headless regression test for lib/quickstart.lua
--
-- Verifies that quick.define(spec) installs a complete, working script
-- contract: init, status, prompt, schema, response processing, commands,
-- tools (engine startup order: get_tools BEFORE set_initial_state),
-- save/restore, questionnaire, spec validation errors.
--
-- Run:  luajit scripts/test_quickstart.lua
-- (no network: query_llm and validated_call are stubbed)

package.path = "./scripts/lib/?.lua;./scripts/?.lua;" .. package.path

os.execute("rm -rf /tmp/npcs_qs_test /tmp/qs_test_memory.json")

local json = require("lib/json")

-- ── Stubs (before requiring persona) ─────────────────────────────────────────
_G.query_llm = function(sys, hist, user, schema, model, provider, label)
    -- jobs.submit's sync fallback (no query_llm_async here) also calls this
    -- global directly — the stage-4 personal-stats backfill fires on every
    -- npc_object() build, so it needs a harmless response too.
    if schema and schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    return '{"narration":"Arrivo nel borgo.","intent":"osserva incuriosito","speech":""}'
end
local llm_util = require("lib/llm_util")
llm_util.validated_call = function(sys, user, schema, validate, opts)
    return {
        name = "Carlo", age = 30, job = "aiuto barista",
        personality = "x", description = "y", appearance = "capelli neri",
        agent_system = string.rep("Io sono Carlo. ", 8),
        routine = {
            { time_from="06:00", time_to="23:00", location_id="bar",    activity="lavora" },
            { time_from="23:00", time_to="06:00", location_id="casa",   activity="dorme" },
        },
    }, nil
end

local quick = require("lib/quickstart")

-- ── Assertion helpers ────────────────────────────────────────────────────────
local pass, fail = 0, 0
local function ok(cond, label)
    if cond then pass = pass + 1
    else fail = fail + 1; print("  FAIL: " .. label) end
end

-- ── 1) Spec validation: actionable errors, fail fast ────────────────────────
print("== 1) spec validation ==")
local okc, err = pcall(quick.define, { name = "x" })
ok(not okc and tostring(err):find("locations"), "missing locations → error names the field")
okc, err = pcall(quick.define, {
    name = "x", welcome = "w", prompt = { header = "h" },
    locations = { a = { name = "A" } }, start = "b",
})
ok(not okc and tostring(err):find("start") and tostring(err):find("a"),
    "bad start → error lists available ids")
okc, err = pcall(quick.define, {
    name = "x", welcome = "w", prompt = { header = "h" },
    locations = { a = { name = "A" } }, start = "a",
    travel = { a = { "ghost" } },
})
ok(not okc and tostring(err):find("ghost"), "dangling travel edge → error names it")

-- ── 2) Full define ───────────────────────────────────────────────────────────
print("== 2) define + engine startup order ==")
local handle = quick.define{
    name    = "qs_test",
    path    = "/tmp/",
    context = "Borgo di mare di test.",
    config  = { use_inventory = true, use_memory = true },
    time    = "10:00",
    locations = {
        piazza = { name = "Piazza",       desc = "La piazza." },
        bar    = { name = "Bar di Gino",  desc = "Il bar." },
        molo   = { name = "Molo",         desc = "Il molo." },
    },
    travel = { piazza = { "bar", "molo" }, bar = { "piazza" }, molo = { "piazza" } },
    start  = "piazza",
    npcs = {
        gino = { name = "Gino", age = 58, job = "barista", location = "bar",
                 personality = "Burbero ma di cuore.",
                 agent_system = "Sono Gino, barista. Parlo con %s.",
                 routine = {
                     { time_from="06:00", time_to="22:00", location_id="bar",   activity="serve caffè" },
                     { time_from="22:00", time_to="06:00", location_id="bar",   activity="dorme nel retro" },
                 } },
        anna = { name = "Anna", age = 31, job = "pescatrice", location = "molo",
                 personality = "Diretta.", agent_system = "Sono Anna.",
                 routine = {
                     { time_from="05:00", time_to="14:00", location_id="molo",  activity="sistema le reti" },
                     { time_from="14:00", time_to="16:00", location_id="bar",   activity="pranzo da Gino" },
                     { time_from="16:00", time_to="05:00", location_id="molo",  activity="dorme a bordo" },
                 } },
    },
    welcome = "Benvenuto nel borgo.",
    arrival = true,
    player_role = "nuovo arrivato",
    character_questions = {
        { field = "name",    prompt = "Nome?",    type = "text" },
        { field = "capelli", prompt = "Capelli?", type = "choice", options = { "ricci", "lisci" } },
    },
    prompt = {
        header = "Sei il narratore del Borgo. Protagonista: {player}.",
        rules  = { "Regola uno.", "Regola due." },
        blocks = function(state) return "\n\n## BLOCCO EXTRA fiducia=" .. (state.fiducia or "?") end,
    },
    schema_extra = {
        fiducia_borgo = {
            type = "integer", minimum = -2, maximum = 2, description = "Delta fiducia.",
            on_set = function(v, state)
                if type(v) == "number" and v ~= 0 then
                    state.fiducia = (state.fiducia or 20) + v
                end
            end,
        },
    },
    state_init   = function(state) state.fiducia = 20 end,
    status_extra = function(state) return { fiducia_borgo = state.fiducia } end,
    hud          = function(state) return "Fiducia: " .. (state.fiducia or 0) end,
    inventory    = { "telefono" },
    money        = 50,
    commands = {
        { cmd = "/fiducia", desc = "Mostra fiducia",
          fn = function(rest, state) return "Fiducia: " .. state.fiducia end },
    },
    tools = {
        { name = "saluta", description = "Saluto di test.",
          params = [[{ "type":"object", "properties":{} }]],
          fn = function(args_json) return json.encode({ ok = true, msg = "ciao" }) end },
    },
}
ok(type(handle) == "table" and handle.state, "define returns handle")

-- Engine calls get_tools() at startup, BEFORE set_initial_state.
local toolset = get_tools()
ok(type(toolset) == "table" and #toolset > 0, "get_tools before init returns tools")
local T = {}
for _, t in ipairs(toolset) do T[t.name] = t.fn end
ok(T.think_as_npc and T.move_player and T.advance_time, "standard tools present")
ok(T.generate_npc and T.npc_life_event, "persona generation tools present")
ok(T.cambia_inventario, "inventory tool present (use_inventory)")
ok(T.memory_write and T.memory_read, "memory tools present (use_memory)")
ok(T.saluta, "adventure-specific tool present")

ok(get_welcome_message() == "Benvenuto nel borgo.", "welcome")
ok(type(get_character_questions()) == "table" and #get_character_questions() == 2,
    "character questions installed")

-- ── 3) init + status + prompt + schema ──────────────────────────────────────
print("== 3) init / status / prompt / schema ==")
set_initial_state("Marco")
local s = handle.state()
ok(s.player.name == "Marco", "player name set")
ok(s.player.location == "piazza", "start location")
ok(s.npc_locations.gino == "bar" and s.npc_locations.anna == "molo", "npc start locations")
ok(s.fiducia == 20, "state_init ran")
ok(s.inventario[1] == "telefono" and s.soldi == 50, "inventory/money init")
ok(s.time == "10:00", "start time")

local status = json.decode(get_status_for_ai())
ok(status.player.name == "Marco" and status.player.role == "nuovo arrivato", "status player")
ok(status.location.id == "piazza" and #status.location.exits == 2, "status location+exits")
ok(status.fiducia_borgo == 20, "status_extra merged")
ok(status.time == "10:00" and status.soldi == 50, "status time+money")

local prompt = get_system_prompt()
ok(prompt:find("Protagonista: Marco", 1, true), "{player} substituted in header")
ok(prompt:find("## REGOLE") and prompt:find("1. Regola uno."), "rules list rendered")
ok(prompt:find("## LOCATION: Piazza", 1, true), "location block")
ok(prompt:find("BLOCCO EXTRA fiducia=20", 1, true), "custom prompt block")
ok(prompt:find("WORKFLOW OBBLIGATORIO", 1, true), "workflow block (MODE C)")
ok(prompt:find("generate_npc", 1, true), "generate_npc in workflow")

local schema = json.decode(get_json_schema())
ok(schema.properties.narration and schema.properties.game_over, "base schema fields")
ok(schema.properties.fiducia_borgo and schema.properties.fiducia_borgo.on_set == nil,
    "schema_extra field present, on_set stripped")
ok(schema.properties.fiducia_borgo.minimum == -2, "schema_extra constraints kept")

-- ── 4) turn cycle ────────────────────────────────────────────────────────────
print("== 4) turn cycle ==")
ok(before_ai_turn("guardo il bar") == nil, "before_ai_turn")
local res = process_ai_response('{"narration":"Ti guardi intorno.","fiducia_borgo":2}')
ok(res.success == true and res.narration == "Ti guardi intorno.", "process_ai_response ok")
ok(handle.state().fiducia == 22, "schema_extra on_set applied")
ok(handle.state().turn == 1, "turn incremented")

res = process_ai_response("not json at all {{{")
ok(res.success == false, "garbage reply → success=false (engine retries)")

res = process_ai_response('{"narration":"Fine.","game_over":true,"game_over_reason":"vittoria"}')
ok(res.success and res.game_over == true and res.game_over_reason == "vittoria", "game over path")

-- ── 5) commands ──────────────────────────────────────────────────────────────
print("== 5) commands ==")
res = process_player_input("/fiducia")
ok(res.handled and tostring(res.output):find("22"), "declarative command")
res = process_player_input("/map")
ok(res ~= nil and res.handled, "framework command (/map)")
res = process_player_input("ciao Gino")
ok(res.handled == false, "normal input not handled")
local cmds = get_commands()
local has_fiducia = false
for _, c in ipairs(cmds) do if c.cmd == "/fiducia" then has_fiducia = true end end
ok(has_fiducia, "get_commands includes declarative command")

-- ── 6) tools live ────────────────────────────────────────────────────────────
print("== 6) tools ==")
before_ai_turn("")
local mv = json.decode(T.move_player('{"location":"bar"}'))
ok(mv.error == nil, "move_player to adjacent location")
ok(handle.state().player.location == "bar", "player moved")
local think = T.think_as_npc('{"id":"gino","situation":"il nuovo arrivato entra nel bar"}')
ok(type(think) == "string" and #think > 0, "think_as_npc returns string")

-- Witness gate on remember(scope="public") — real bug found in play: a
-- "public" note reached an NPC across town who "remembered witnessing"
-- something private that happened in a different room. Player is in "bar"
-- with Gino; Anna is at "molo" and must NOT receive this note directly.
local rem = json.decode(T.remember('{"note":"cosa privata nel bar","scope":"public"}'))
ok(rem.ok == true, "remember(public) succeeds")

-- think_as_npc's return value is the STUBBED agent reply (fixed text) — the
-- injected note lives in the SITUATION sent to query_llm, not the response.
-- Capture the actual argument the agent forwards to verify injection.
local captured_situation
local real_query_llm = _G.query_llm
_G.query_llm = function(sys, hist, situation, schema, model, provider, label)
    captured_situation = situation
    return real_query_llm(sys, hist, situation, schema, model, provider, label)
end

before_ai_turn("")   -- new turn: reset think_as_npc's per-turn cache from the earlier call
T.think_as_npc('{"id":"gino","situation":"altra situazione"}')
ok(captured_situation and captured_situation:find("cosa privata nel bar", 1, true) ~= nil,
   "co-located NPC (Gino, in the bar) DOES get the witnessed public note")
before_ai_turn("")
json.decode(T.move_player('{"location":"molo"}'))
before_ai_turn("")
captured_situation = nil
T.think_as_npc('{"id":"anna","situation":"altra situazione ancora"}')
ok(captured_situation and captured_situation:find("cosa privata nel bar", 1, true) == nil,
   "NPC elsewhere at write time (Anna, at molo) does NOT get the note — no cross-room leak")
-- Legacy note (written before this fix, no .witnesses field) — must reach
-- NOBODY directly (narrator's own prompt_notes still sees it, unaffected).
handle.state().notes = handle.state().notes or {}
table.insert(handle.state().notes, { date="x", content="nota legacy senza testimoni", scope="public" })
before_ai_turn("")
captured_situation = nil
T.think_as_npc('{"id":"anna","situation":"terza situazione"}')
ok(captured_situation and captured_situation:find("nota legacy", 1, true) == nil,
   "legacy public note with no witnesses field reaches no specific NPC (retroactive fix)")
_G.query_llm = real_query_llm
local narrator_prompt = get_system_prompt()
ok(narrator_prompt:find("cosa privata nel bar", 1, true) ~= nil,
   "narrator's OWN prompt still sees ALL public notes unconditionally (unaffected by witness gate)")
-- The witness gate only blocks the per-NPC think_as_npc injection — the
-- narrator's OWN free prose isn't gated at all (it can't be, it needs full
-- state for coherent narration) and a real bug slipped through THIS
-- channel twice: the narrator itself had an unrelated NPC declare "lo sanno
-- tutti" about a witnessed-only fact. Fix: show the real witness list (or
-- an explicit "no witness" flag) right next to each public fact so the
-- narrator has a concrete anchor instead of an assumption to reach for.
ok(narrator_prompt:find("testimoni diretti: gino", 1, true) ~= nil,
   "narrator's prompt shows the REAL witness list next to the witnessed fact")
ok(narrator_prompt:find("nessun testimone registrato", 1, true) ~= nil,
   "legacy note with no witnesses is flagged explicitly, not framed as universally known")
before_ai_turn("")
json.decode(T.move_player('{"location":"bar"}'))

-- Real bug: an NPC created OUTSIDE the in-game generate_npc flow (e.g. via
-- CoderAI's editor_npc_create → persona.register_static/generate directly)
-- has a full persona file but is invisible to gen_npc_locations — move_npc
-- rejected her, so the narrator generated a DUPLICATE character instead of
-- reusing the real one. Simulate that exact setup: register a persona
-- directly (bypassing generate_npc entirely), then move_npc must accept her.
handle.persona.register_static("don_marcello_test", {
    name = "Don Marcello", personality = "Riservato, custodisce segreti.",
    agent_system = string.rep("Sono Don Marcello. ", 8),
})
ok(handle.state().gen_npc_locations.don_marcello_test == nil,
   "sanity: the persona exists but is NOT yet in gen_npc_locations")
local pre_status = json.decode(get_status_for_ai())
local pre_hint = nil
for _, n in ipairs(pre_status.gen_npcs_known or {}) do
    if n.id == "don_marcello_test" then pre_hint = n.location end
end
ok(pre_hint and pre_hint:find("NON PIAZZATO", 1, true) ~= nil,
   "before placement: status spells out the fix (move_npc), not a bare '?'")

local mv2 = json.decode(T.move_npc('{"id":"don_marcello_test","location":"bar"}'))
ok(mv2.error == nil, "move_npc accepts an existing-but-unplaced persona (not 'NPC non trovato')")
ok(handle.state().gen_npc_locations.don_marcello_test == "bar",
   "she is lazily registered into gen_npc_locations on first successful move")

local post_status = json.decode(get_status_for_ai())
local post_hint = nil
for _, n in ipairs(post_status.gen_npcs_known or {}) do
    if n.id == "don_marcello_test" then post_hint = n.location end
end
ok(post_hint == "bar", "after placement: status shows her real location, hint gone")

local gen = json.decode(T.generate_npc(
    '{"id":"carlo_barista","context":"Carlo, aiuto barista del bar di Gino, 30 anni, timido"}'))
ok(gen.ok == true and gen.id == "carlo_barista", "generate_npc creates persona")
ok(handle.state().gen_npc_locations.carlo_barista == "bar", "generated NPC auto-positioned")
-- Stage 3 prerequisite: a generated NPC gets the SAME live composition a
-- static NPC gets (npc_object tied to NPC.tick + full agent, not the bare
-- lazy-built prompt-only fallback) — otherwise she can talk but never act
-- autonomously, and crystallization would write a routine nothing ticks.
ok(handle.npc_objects().carlo_barista ~= nil,
   "generated NPC gets a live npc.lua tick object (use_npc_tick adventure)")
status = json.decode(get_status_for_ai())
local carlo_present = false
for _, n in ipairs(status.gen_npcs_present or {}) do
    if n.id == "carlo_barista" then carlo_present = true end
end
ok(carlo_present, "generated NPC in status")
local sal = json.decode(T.saluta("{}"))
ok(sal.ok == true and sal.msg == "ciao", "adventure tool callable")

-- ── 7) display + arrival ─────────────────────────────────────────────────────
print("== 7) display / arrival ==")
local hud = get_display_state()
ok(hud:find("Fiducia: 22", 1, true), "HUD extra line")
ok(hud:find("Gino", 1, true), "HUD shows present NPC")
ok(hud:find("Carlo", 1, true), "HUD shows generated NPC (gen_npc_locations)")
local npcs_out = process_player_input("/npcs")
ok(npcs_out.handled and npcs_out.output:find("Carlo", 1, true), "/npcs lists generated NPC")
ok(generate_arrival() == "Arrivo nel borgo.", "arrival scene from LLM")

-- ── 8) save / restore ────────────────────────────────────────────────────────
print("== 8) save / restore ==")
local snap = get_state_snapshot()
handle.state().fiducia = 99
handle.state().player.location = "molo"
res = restore_state(snap)
ok(res.success == true, "restore ok")
ok(handle.state().fiducia == 22, "state field restored")
ok(handle.state().player.location == "bar", "location restored")
ok(handle.state().player.name == "Marco", "name restored")
-- Restore prerequisite: a generated NPC (carlo_barista, from §6) must NOT
-- lose her live composition across a save/reload — init_agents() rebuilds
-- npc_objects/agents from scratch every restore, and used to only ever
-- cover spec.npcs, never state.gen_npc_locations.
ok(handle.npc_objects().carlo_barista ~= nil,
   "generated NPC's live tick object survives save/restore")
res = restore_state("garbage{{")
ok(res.success == false, "bad snapshot → success=false")

-- ── 9) questionnaire flow ────────────────────────────────────────────────────
print("== 9) questionnaire ==")
set_initial_state('{"name":"Anna Rossi","capelli":"ricci"}')
s = handle.state()
ok(s.player.name == "Anna Rossi", "name from answers")
ok(tostring(s.player.appearance):find("capelli: ricci", 1, true), "appearance from answers")

-- ── 9b) living NPCs: routine tick moves them, state synced ───────────────────
print("== 9b) npc tick (use_npc_tick auto) ==")
set_initial_state("Marco")
s = handle.state()
ok(next(handle.npc_objects()) ~= nil, "npc_objects built (routine present)")
ok(s.npc_locations.anna == "molo", "anna on routine slot at 10:00")
ok((s.npc_activities or {}).anna ~= nil, "activity from tick")
ok(type(s.npc_stats) == "table", "npc_stats synced into state")
s.time = "15:00"
before_ai_turn("")
ok(s.npc_locations.anna == "bar", "anna moved to bar at 15:00 (routine)")
ok(s.npc_locations.gino == "bar", "gino still at bar")
s.time = "23:00"
before_ai_turn("")
ok(s.npc_locations.anna == "molo", "anna back to molo at 23:00")
local snap2 = get_state_snapshot()
restore_state(snap2)
ok(handle.state().npc_locations.anna == "molo", "npc locations survive restore")
ok(next(handle.npc_objects()) ~= nil, "npc_objects rebuilt on restore")

-- ── 9c) hallucinated-dialect tolerance (id/exits/home/parameters…) ──────────
print("== 9c) dialect tolerance ==")
os.execute("rm -rf /tmp/npcs_dialetto")
local ok9c, H2 = pcall(quick.define, {
    id = "dialetto", path = "/tmp/",
    title = "Il Test",
    description = "Un test di tolleranza.",
    config = { mode = "C", start_location = "sala", protagonist_name = "Pino" },
    locations = {
        sala   = { name = "Sala",   desc = "x", exits = { "cucina" } },
        cucina = { name = "Cucina", desc = "y", exits = { "sala" } },
    },
    npcs = { rosa = { name = "Rosa", personality = "p", home = "cucina",
                      secrets = { "s1" } } },
    notes = { s1 = "Il segreto." },
    time  = "mattina",
    tools = {
        { name = "tool_tab",  description = "d",
          parameters = { type = "object", properties = {} },
          fn = function() return "{}" end },
        { name = "tool_nofn", description = "d",
          parameters = { type = "object" } },
    },
})
ok(ok9c, "dialect spec accepted: " .. tostring(H2))
if ok9c then
    local ts2 = get_tools()
    local T2 = {}
    for _, t in ipairs(ts2) do T2[t.name] = t end
    ok(T2.tool_tab and type(T2.tool_tab.params) == "string", "parameters table → params string")
    ok(T2.tool_nofn == nil, "fn-less tool skipped")
    set_initial_state("")
    local s2 = H2.state()
    ok(s2.player.name == "Pino", "protagonist_name alias")
    ok(s2.player.location == "sala", "start_location alias")
    ok(s2.npc_locations.rosa == "cucina", "npc home → location alias")
    ok(s2.time ~= "mattina", "non-HH:MM time ignored")
    ok(H2.persona.get("rosa").secret == "Il segreto.", "secrets keys → secret resolved via notes")
    ok(get_welcome_message():find("Un test di tolleranza", 1, true), "description → welcome")
    ok(get_system_prompt():find("Il Test", 1, true), "prompt.header built from title+description")
    local mp = process_player_input("/map")
    ok(mp.handled and mp.output:find("cucina", 1, true), "travel derived from locations[].exits")
end
os.execute("rm -rf /tmp/npcs_dialetto")

-- error pointer: bail messages must point to the doc
okc, err = pcall(quick.define, { name = "x" })
ok(not okc and tostring(err):find("quickstart"), "bail points to read_knowledge quickstart")

-- ── 9d) dream wiring: after_ai_turn fires persona.dream_tick automatically ──
-- Confirmed bug (project_dream_system_redesign memory): quickstart NEVER
-- called dream_tick, so the whole dream system was dead in practice even
-- after the stage-1 content rewrite. Verify it actually fires now.
print("== 9d) dream wiring ==")
os.execute("rm -rf /tmp/npcs_qs_dream")
local H4 = quick.define{
    name = "qs_dream", path = "/tmp/",
    config = { use_time = true },
    locations = { sala = { name = "Sala", desc = "s" } },
    travel = {}, start = "sala",
    npcs = { mara = { name = "Mara", personality = "gentile",
                      agent_system = "Sono Mara e parlo sempre con calma e gentilezza." } },
    welcome = "w", prompt = { header = "h" },
}
set_initial_state("A")
local s4 = H4.state()
ok(type(s4.last_dream) == "table", "state.last_dream initialized")

-- dream-aware stub: distinguishes the dream call from any other by schema
-- content. Save/restore the original so later sections (10: template_min)
-- aren't contaminated by this scoped swap.
local real_validated_call = llm_util.validated_call
local dream_stub_calls = 0
llm_util.validated_call = function(sys, user, schema, validate, opts)
    if schema:find('"dream_narrative"', 1, true) then
        dream_stub_calls = dream_stub_calls + 1
        return {
            dream_narrative = "Sogna il mare.", aspect_developed = "quiete",
            life_event_summary = "[dream] sogna il mare",
            npc_summary_update = "Oggi Mara sembra più serena del solito.",
        }, nil
    end
    return { summary = "giornata tranquilla" }, nil
end

s4.time = "02:30"   -- inside M.dream_tick's 01:00-05:00 window
before_ai_turn("")
process_ai_response('{"narration":"Passa la notte."}')
after_ai_turn("Passa la notte.", "{}")
ok(dream_stub_calls == 1, "after_ai_turn triggers exactly one dream this turn")
ok(s4.last_dream.mara ~= nil, "last_dream guard updated for the dreaming NPC")
ok(H4.persona.get("mara").npc_summary == "Oggi Mara sembra più serena del solito.",
   "npc_summary applied to the persona file")
ok(H4.npc_data().mara.description == "Oggi Mara sembra più serena del solito.",
   "rebuild_npc_data prefers npc_summary over static personality once it exists")

-- outside the window: no dream fires
dream_stub_calls = 0
s4.time = "11:00"
before_ai_turn("")
process_ai_response('{"narration":"Un turno di giorno."}')
after_ai_turn("Un turno di giorno.", "{}")
ok(dream_stub_calls == 0, "no dream call outside the 01:00-05:00 window")

-- sleep_until crossing the window → whole-cast dream_tick_all path.
-- New day (mara already dreamed on day 1 above — the guard must allow a
-- fresh dream on a new day, exercised via the sleep_until path specifically).
s4.giorno_index = (s4.giorno_index or 1) + 1
s4._dream_due_day = s4.giorno_index
dream_stub_calls = 0
after_ai_turn("Dormi fino all'alba.", "{}")
ok(dream_stub_calls == 1, "sleep_until crossing → dream_tick_all path fires")
ok(s4._dream_due_day == nil, "_dream_due_day flag consumed")
ok(s4.last_dream.mara == s4.giorno_index, "last_dream guard updated for the new day")

llm_util.validated_call = real_validated_call
os.execute("rm -rf /tmp/npcs_qs_dream")

-- ── 9e) session isolation: new game forks, restore returns to its session ───
print("== 9e) session isolation ==")
os.execute("rm -rf /tmp/npcs_qs_sess /tmp/npcs_qs_sess_sessions")
local H3 = quick.define{
    name = "qs_sess", path = "/tmp/",
    locations = { sala = { name = "Sala", desc = "s" } },
    travel = {}, start = "sala",
    npcs = { ugo = { name = "Ugo", personality = "pigro",
                     agent_system = "Sono Ugo e rispondo sempre con calma e poche parole." } },
    welcome = "w", prompt = { header = "h" },
}
set_initial_state("A")
local p1 = H3.persona.get_path()
ok(p1:find("npcs_qs_sess_sessions", 1, true) ~= nil, "new game forks a session folder")
ok(H3.state()._persona_path == p1, "state records the session path")
local tf = io.open("/tmp/npcs_qs_sess/ugo.lua")
ok(tf ~= nil, "template file exists in the template dir")
if tf then tf:close() end
H3.persona.patch("ugo", { conditions_add = { "ferito" } })   -- in-game evolution
local snap_sess = get_state_snapshot()
os.execute("sleep 1")   -- session folders are timestamped per second
set_initial_state("B")
local p2 = H3.persona.get_path()
ok(p2 ~= p1, "second new game gets a fresh session folder")
local du = H3.persona.get("ugo")
ok(du and #(du.conditions or {}) == 0, "no bleed: fresh game sees pristine NPC")
res = restore_state(snap_sess)
ok(res.success and H3.persona.get_path() == p1, "restore switches back to the saved session")
du = H3.persona.get("ugo")
ok(du and du.conditions and du.conditions[1] == "ferito", "restored session sees the evolved NPC")
os.execute("rm -rf /tmp/npcs_qs_sess /tmp/npcs_qs_sess_sessions")

-- ── 10) template_min.lua loads (syntax + define runs) ────────────────────────
print("== 10) template_min ==")
os.execute("rm -rf ./scripts/npcs_template_min ./scripts/npcs_template_min_sessions")
local chunk, lerr = loadfile("./scripts/template_min.lua")
ok(chunk ~= nil, "template_min.lua parses: " .. tostring(lerr))
if chunk then
    local okrun, rerr = pcall(chunk)
    ok(okrun, "template_min.lua define runs: " .. tostring(rerr))
    if okrun then
        set_initial_state("Test")
        ok(handle ~= nil and get_system_prompt():find("Il Borgo", 1, true),
            "template_min prompt active")
        ok(json.decode(get_json_schema()).properties.fiducia_borgo ~= nil,
            "template_min schema_extra active")
    end
end
os.execute("rm -rf ./scripts/npcs_template_min ./scripts/npcs_template_min_sessions")

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
