-- test_robustness.lua — adversarial regression harness for the LLM↔engine boundary.
--
-- The model side is unreliable: it sends empty/malformed args, invents ids,
-- acts on missing entities, picks wrong actions. This harness fires those
-- adversarial inputs at every tool and ASSERTS the engine never throws and
-- always returns actionable JSON (tolerate or guide) — never a raw crash or a
-- loop-inducing bare error. It is MODEL-AGNOSTIC: run it after changing the
-- LLM; if it passes, the structural robustness layer still holds.
--
-- Run:  luajit scripts/test_robustness.lua
-- (no network: the LLM call is stubbed)

package.path = "./scripts/lib/?.lua;./scripts/?.lua;" .. package.path

local json = require("lib/json")

-- Default fallback for jobs.submit's sync path (no query_llm_async in this
-- harness — see lib/jobs.lua) so an M.npc_object() call anywhere in the file
-- (e.g. the stage-4 personal-stats backfill, which fires on EVERY npc_object
-- construction) always has a global to call, even before a section defines
-- its own more specific stub. Sections below override this as needed.
_G.query_llm = function(sys, hist, user, schema, model, provider, label)
    if schema and schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    return "{}"
end

-- ── Stub the LLM so generation tools don't hit the network ──────────────────
-- validated_call returns a minimal but structurally-valid entity for any schema.
local llm_util = require("lib/llm_util")
llm_util.validated_call = function(sys, user, schema, validate, opts)
    return {
        name = "Generato", age = 40, job = "lavoratore",
        personality = "x", description = "y",
        agent_system = string.rep("Io sono un personaggio. ", 8),
        connected_to = {},
        states = { "normale" }, current_state = "normale", actions = {},
        routine = {
            { time_from="06:00", time_to="08:00", location_id="g_cucina",  activity="a" },
            { time_from="08:00", time_to="18:00", location_id="g_lavoro",  activity="b" },
            { time_from="18:00", time_to="23:00", location_id="g_salotto", activity="c" },
            { time_from="23:00", time_to="06:00", location_id="g_camera",  activity="sleeps and dreams" },
        },
    }, nil
end

local world   = require("lib/world")
local persona = require("lib/persona")
local tools   = require("lib/tools")

-- register_static is idempotent-by-design (step 2: file exists on disk →
-- load it, ignore the config) — a leftover file from a PREVIOUS run of this
-- suite makes a "fresh NPC" test load stale generated state instead. Wipe
-- every test-specific id up front so each run is self-contained.
os.execute('rm -f /tmp/test_robustness_npcs/notaio_pad_test.lua'
        .. ' /tmp/test_robustness_npcs/fallito_x.lua'
        .. ' /tmp/test_robustness_npcs/junk_edit_test.lua'
        .. ' /tmp/test_robustness_npcs/dream_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/compact_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/nocompact_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/need_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/event_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/crystal_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc1.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc2.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc3.lua'
        .. ' /tmp/test_robustness_npcs/stats_capped_test.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc4.lua'
        .. ' /tmp/test_robustness_npcs/variation_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/variation_test_npc2.lua'
        .. ' /tmp/test_robustness_npcs/ingegner_cavallaro_test.lua')

world.init("Mondo di test.")
persona.init("/tmp/test_robustness_npcs/", "Mondo di test.")

-- ── Assertion helpers ───────────────────────────────────────────────────────
local pass, fail = 0, 0
local function ok(cond, label)
    if cond then pass = pass + 1
    else fail = fail + 1; print("  FAIL: " .. label) end
end

-- A tool fn must, for ANY input: not throw, and return a string that decodes to
-- a JSON object (tolerate=ok or guide=error). Never a raw Lua error / nil / loop.
local function assert_safe(label, fn, args)
    local okc, res = pcall(fn, args)
    if not okc then
        ok(false, label .. " → THREW: " .. tostring(res)); return nil
    end
    if type(res) ~= "string" then
        ok(false, label .. " → non-string result (" .. type(res) .. ")"); return nil
    end
    local okd, decoded = pcall(json.decode, res)
    if not okd or type(decoded) ~= "table" then
        ok(false, label .. " → result not JSON object: " .. tostring(res)); return nil
    end
    ok(true, label)
    return decoded
end

-- Build the tool set through tools.build so the defensive wrapper is exercised.
local toolset = tools.build({
    world.as_tool_generate_object(),
    world.as_tool_object_action(),
    world.as_tool_object_write(),
    world.as_tool_move_object(),
    world.as_tool_generate_location(),
    world.as_tool_log_event(),
    world.as_tool_bible(),
    persona.as_tool_generate(),
    persona.as_tool_life_event(),
})
local T = {}
for _, t in ipairs(toolset) do T[t.name] = t.fn end

-- Adversarial inputs every tool must survive.
local ADVERSARIAL = {
    { "empty string",      "" },
    { "whitespace",        "   " },
    { "malformed json",    "{bad" },
    { "empty object",      "{}" },
    { "null",              "null" },
    { "array not object",  "[1,2,3]" },
    { "unicode id",        '{"id":"specchio埋","action":"esamina","field":"t","to":"player","context":"x","key":"k","fact":"f","location_id":"l","text":"t"}' },
}

print("== A) Defensive wrapper: every tool survives adversarial args ==")
for _, t in ipairs(toolset) do
    for _, adv in ipairs(ADVERSARIAL) do
        assert_safe(t.name .. " / " .. adv[1], t.fn, adv[2])
    end
end

print("== B) Boundary semantics: tolerate-or-guide, never silent failure ==")

-- object_write on a missing object → AUTO-CREATE (tolerate), returns ok
local r = assert_safe("object_write missing → auto-create",
    T.object_write, '{"id":"biglietto_x","field":"testo","set":"ciao"}')
ok(r and r.ok == true, "object_write auto-create returns ok")

-- move_object on a missing object → AUTO-CREATE + move
r = assert_safe("move_object missing → auto-create",
    T.move_object, '{"id":"chiave_x","to":"player"}')
ok(r and r.ok == true, "move_object auto-create returns ok")

-- object_action on a missing STATEFUL object → GUIDE (not silent, not autogen)
r = assert_safe("object_action missing → guide",
    T.object_action, '{"id":"porta_inesistente","action":"apri"}')
ok(r and r.error ~= nil, "object_action missing returns a guiding error")

-- generate_object empty → GUIDE with example
r = assert_safe("generate_object empty → guide", T.generate_object, "{}")
ok(r and r.error ~= nil and tostring(r.error):find("Esempio"),
   "generate_object empty error carries an example")

-- world_event with no location_id → guide
r = assert_safe("world_event no id → guide", T.world_event, '{"text":"x"}')
ok(r and r.error ~= nil, "world_event without location_id guides")

-- generate_location empty → guide with example
r = assert_safe("generate_location empty → guide", T.generate_location, "{}")
ok(r and r.error ~= nil, "generate_location empty guides")

-- npc_life_event on unknown NPC → guide toward generate_npc
r = assert_safe("npc_life_event unknown → guide",
    T.npc_life_event, '{"id":"sconosciuto_x"}')
ok(r and r.error ~= nil, "npc_life_event unknown NPC guides")

-- id sanitization merges the unicode-corrupted key (no phantom duplicate)
T.object_write('{"id":"registro埋","field":"n","set":"1"}')
ok(world.format_object("registro") ~= nil, "unicode id sanitized to clean key")

print("== C) Sparse generation output: repair, never bare-reject ==")

-- mid-tier gen models emit 1-2 routine slots: generation must SUCCEED and the
-- routine must be padded to full 24h coverage (observed live: 'routine
-- insufficient (1 slots)' → narrator retried identically until the loop cap)
local real_vc = llm_util.validated_call
llm_util.validated_call = function(sys, user, schema, validate, opts)
    return {
        name = "Notaio Mancuso", age = 52, job = "notaio",
        personality = "viscido ma educato", description = "distinto",
        appearance = "distinto, brizzolato",
        agent_system = string.rep("Sono il notaio Mancuso, cliente abituale. ", 4),
        routine = {
            { time_from="09:00", time_to="13:00", location_id="g_studio",
              activity="riceve clienti in studio" },
        },
    }, nil
end
local sparse = persona.generate("notaio_pad_test", "Notaio 52enne, cliente abituale, viscido ma educato, giornata tipo in studio")
ok(sparse ~= nil, "1-slot routine → generation ACCEPTED (repaired, not rejected)")
if sparse then
    ok(#(sparse.routine or {}) >= 2, "routine padded with filler slots")
    local issues = persona.validate_routine("notaio_pad_test")
    local gap = false
    for _, msg in ipairs(issues or {}) do
        if tostring(msg):find("gap") or tostring(msg):find("empty") then gap = true end
    end
    ok(not gap, "padded routine has no 24h gaps ("
        .. table.concat(issues or {}, "; ") .. ")")
end

-- generation failure → tool error carries the REASON + keep-the-id guidance
llm_util.validated_call = function() return nil, "agent_system too short or missing" end
r = assert_safe("generate_npc failure → actionable error",
    T.generate_npc, '{"id":"fallito_x","context":"un tizio qualunque di passaggio"}')
ok(r and r.error and tostring(r.error):find("agent_system too short"),
   "error states the rejection reason")
ok(r and r.error and tostring(r.error):find("stesso id"),
   "error says to keep the same id")
llm_util.validated_call = real_vc

print("== D) Hand-edited persona junk: npc_object tolerates, never breaks tick ==")

-- observed live: an uninformed editor wrote npc_needs as plain strings and
-- npc_event_reactions as string values — must not build needs with nil
-- stat/threshold (breaks NPC.tick) nor drop the shorthand reactions
local pj = persona.register_static("junk_edit_test", {
    name = "Junk", personality = "x",
    agent_system = string.rep("Io sono un test. ", 6),
    routine = {
        { time_from="06:00", time_to="18:00", location_id="g_casa", activity="a" },
        { time_from="18:00", time_to="06:00", location_id="g_casa", activity="dorme" },
    },
})
pj.npc_needs = {
    "una stringa scritta a mano",
    { description = "tabella senza stat/threshold" },
    { stat = "fame", threshold = 0.5, sequence_name = "seq_x", description = "valida" },
}
pj.npc_event_reactions = {
    evento_str = "si nasconde dietro il bancone",
    evento_tab = { activity = "reagisce", stats = {} },
}
local adapter = {
    getLocation = function() return "g_casa" end,
    setLocation = function() end,
    isInLocation = function() return false end,
    countInLocation = function() return 0 end,
    getAppearance = function() return "NORMAL" end,
    setAppearance = function() end,
    distance = function() return 2 end,
}
local nobj = persona.npc_object("junk_edit_test", adapter)
ok(nobj ~= nil, "npc_object builds on junk-edited persona")
if nobj then
    ok(#(nobj.config.needs or {}) == 1, "only the structurally valid need survives")
    ok(nobj.config.event_reactions.evento_str
       and nobj.config.event_reactions.evento_str.activity == "si nasconde dietro il bancone",
       "string reaction tolerated as shorthand activity")
    local okup, res = pcall(function() return nobj:update("13:00", "monday", "altrove") end)
    ok(okup and res ~= nil, "NPC update runs clean on junk-edited persona")
end

print("== E) Dream redesign stage 1: scope cut, compaction-first, relationships/goals ==")

-- Discriminate compaction vs dream calls by schema content (both flow
-- through the same llm_util.validated_call entry point).
local dream_call_log = {}
llm_util.validated_call = function(sys, user, schema, validate, opts)
    table.insert(dream_call_log, { schema = schema })
    if schema:find('"summary"', 1, true) then
        return { summary = "Giornata intensa con più visite e chiacchiere." }, nil
    end
    return {
        dream_narrative     = "Sogna di essere di nuovo giovane.",
        aspect_developed    = "nostalgia",
        life_event_summary  = "[dream] sogna la giovinezza",
        npc_summary_update  = "Oggi è più malinconica del solito.",
        relationships_patch = { player = "Comincia a fidarsi di lui." },
        long_term_goals_add = { "riconciliarsi col passato" },
    }, nil
end

-- E1: dream touches ONLY personality/relationships/goals — never mechanical structure
persona.register_static("dream_test_npc", {
    name = "Rita", personality = "gentile", agent_system = string.rep("Sono Rita. ", 8),
})
local r1 = persona.dream("dream_test_npc", "day 1")
ok(r1 ~= nil, "dream succeeds with the reduced schema")
ok(r1 and r1.addition_type == nil and r1.additions == nil,
   "dream result carries no mechanical addition fields (old shape gone)")
local d1 = persona.get("dream_test_npc")
ok(d1.npc_summary == "Oggi è più malinconica del solito.", "npc_summary updated")
ok(d1.relationships and d1.relationships.player == "Comincia a fidarsi di lui.",
   "relationships_patch applied")
ok(d1.long_term_goals and d1.long_term_goals[1] == "riconciliarsi col passato",
   "long_term_goals_add applied")
ok(not d1.npc_sequences or not next(d1.npc_sequences), "no sequence ever added by dream")
ok(not d1.npc_needs or #d1.npc_needs == 0, "no need ever added by dream")

-- E2: relationships_patch MERGES, never wipes prior keys
d1.relationships.gino = "Un vecchio amico."
persona.dream("dream_test_npc", "day 2")
d1 = persona.get("dream_test_npc")
ok(d1.relationships.gino == "Un vecchio amico.", "relationships merge preserves prior keys")
ok(d1.relationships.player == "Comincia a fidarsi di lui.", "...and the dream-patched key too")

-- E3: the NPC's OWN agent prompt now reflects dream-driven relationships too
-- (previously only the narrator-facing M.format() read this field)
local ag = persona.agent_object("dream_test_npc", {})
ok(ag ~= nil, "agent_object builds")
ok(ag.system:find("Comincia a fidarsi", 1, true) ~= nil,
   "agent system prompt includes relationships")

-- E4: >=5 pending events since last dream → compacted into ONE entry BEFORE
-- dream reads context (fixes same-day-earlier events being lost to the old
-- hardcoded last-5 window)
persona.register_static("compact_test_npc", {
    name = "Elio", personality = "curioso", agent_system = string.rep("Sono Elio. ", 8),
})
local dc = persona.get("compact_test_npc")
dc.life_events = {
    { date="day 3", event="Si sveglia presto." },
    { date="day 3", event="Fa colazione con Rita." },
    { date="day 3", event="Litiga con un cliente." },
    { date="day 3", event="Aiuta un vicino." },
    { date="day 3", event="Trova una lettera misteriosa." },
}
dream_call_log = {}
local r4 = persona.dream("compact_test_npc", "day 3")
ok(r4 ~= nil, "dream after compaction succeeds")
local compact_calls = 0
for _, c in ipairs(dream_call_log) do
    if c.schema:find('"summary"', 1, true) then compact_calls = compact_calls + 1 end
end
ok(compact_calls == 1, "compaction fires exactly once for 5 pending events")
dc = persona.get("compact_test_npc")
local compressed_found, raw_found = false, false
for _, ev in ipairs(dc.life_events) do
    if (ev.event or ""):match("^%[compressed%]") then compressed_found = true end
    if ev.event == "Trova una lettera misteriosa." then raw_found = true end
end
ok(compressed_found, "5 pending events collapsed into one [compressed] entry")
ok(not raw_found, "raw pre-compaction events replaced, not duplicated alongside the summary")

-- E5: <5 pending events → no compaction call, dream reads them raw directly
persona.register_static("nocompact_test_npc", {
    name = "Sara", personality = "timida", agent_system = string.rep("Sono Sara. ", 8),
})
local dn = persona.get("nocompact_test_npc")
dn.life_events = {
    { date="day 1", event="Arriva in città." },
    { date="day 1", event="Trova lavoro." },
}
dream_call_log = {}
persona.dream("nocompact_test_npc", "day 1")
compact_calls = 0
for _, c in ipairs(dream_call_log) do
    if c.schema:find('"summary"', 1, true) then compact_calls = compact_calls + 1 end
end
ok(compact_calls == 0, "no compaction call for fewer than 5 pending events")

-- E6: dream_tick/dream_tick_all still work with npc_objects=nil (signature
-- kept for call-site compat even though nothing reads it anymore)
local last_dream = {}
local tickres = persona.dream_tick("02:00", 5, last_dream, nil)
ok(tickres ~= nil and last_dream[tickres.id] == 5, "dream_tick works with npc_objects=nil")
local last_dream2 = {}
local allres = persona.dream_tick_all(6, last_dream2, nil)
ok(type(allres) == "table", "dream_tick_all works with npc_objects=nil")

llm_util.validated_call = real_vc

print("== F) Need/sequence stage 2a: earned from real stat crossings ==")

-- jobs.submit's sync fallback (no query_llm_async in this harness) calls the
-- real `query_llm` global directly and dispatches immediately — stub it to
-- return a JSON STRING matching _NEED_SEQ_SCHEMA (not a Lua table; this path
-- bypasses llm_util.validated_call entirely).
local need_gen_calls = 0
_G.query_llm = function(sys, hist, user, schema, model, provider)
    if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    need_gen_calls = need_gen_calls + 1
    return json.encode({
        sequence_name     = "seeks_comfort_" .. tostring(need_gen_calls),
        sequence_steps    = { { location_id = "npcf_camera", activity = "si rifugia in camera" } },
        option_description = "cerca conforto in solitudine (variante " .. need_gen_calls .. ")",
    })
end

persona.register_static("need_test_npc", {
    name = "Vera", personality = "malinconica",
    agent_system = string.rep("Sono Vera. ", 8),
})
local nt_adapter = {
    getLocation = function() return "npcf_camera" end,
    setLocation = function() end,
    isInLocation = function() return false end,
    countInLocation = function() return 0 end,
    getAppearance = function() return "NORMAL" end,
    setAppearance = function() end,
    distance = function() return 2 end,
}
local nt_obj = persona.npc_object("need_test_npc", nt_adapter)
nt_obj.stats.loneliness = 0.9   -- above default threshold 0.75

-- First crossing: creates a brand new need+sequence pair
persona.check_pending_needs({ need_test_npc = nt_obj })
local ntd = persona.get("need_test_npc")
ok(need_gen_calls == 1, "first crossing fires exactly one generation call")
ok(ntd.npc_needs and #ntd.npc_needs == 1 and ntd.npc_needs[1].stat == "loneliness",
   "need created for the crossed stat")
ok(ntd.npc_sequences and ntd.npc_sequences.seeks_comfort_1 ~= nil,
   "sequence created and named by the SAME call — guaranteed to match")
ok(ntd.npc_needs[1].options[1].sequence == "seeks_comfort_1",
   "need's option references the sequence generated in the SAME call (no orphan risk)")
ok(nt_obj.config.needs and #nt_obj.config.needs == 1,
   "live npc.lua tick object mirrored immediately (no reload needed)")
ok(nt_obj.config.sequences and nt_obj.config.sequences.seeks_comfort_1 ~= nil,
   "live tick object's sequences mirrored too")

-- Second crossing (need already exists): ENRICH with one more option,
-- do not create a second need for the same stat
persona.check_pending_needs({ need_test_npc = nt_obj })
ntd = persona.get("need_test_npc")
ok(#ntd.npc_needs == 1, "still exactly one need for this stat (enriched, not duplicated)")
ok(#ntd.npc_needs[1].options == 2, "enrichment appended a second option")
ok(#nt_obj.config.needs[1].options == 2, "live object's options mirrored too")

-- Cap: keep enriching until _MAX_NEED_OPTIONS, then no further generation
persona.check_pending_needs({ need_test_npc = nt_obj })   -- 3rd option → cap
local calls_at_cap = need_gen_calls
persona.check_pending_needs({ need_test_npc = nt_obj })   -- should NOT fire again
ok(need_gen_calls == calls_at_cap, "no further generation once options cap is reached")
ok(#ntd.npc_needs[1].options == 3, "options capped at 3")

-- A stat below threshold never triggers anything
need_gen_calls = 0
nt_obj.stats.hunger = 0.2
persona.check_pending_needs({ need_test_npc = nt_obj })
ok(need_gen_calls == 0, "stat below threshold triggers no generation")

print("== G) Event reaction stage 2b: earned from a REAL event recurring twice ==")

-- jobs.submit's sync fallback calls query_llm directly with the schema as
-- the 4th arg — discriminate by a field unique to _EVENT_REACTION_SCHEMA
-- ("option_description" only exists in _NEED_SEQ_SCHEMA).
local event_gen_calls = 0
local last_event_include_sequence = false
_G.query_llm = function(sys, hist, user, schema, model, provider)
    if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    if schema:find('"option_description"', 1, true) then
        return json.encode({ sequence_name = "n", sequence_steps = {}, option_description = "d" })
    end
    event_gen_calls = event_gen_calls + 1
    local resp = { activity = "si blocca un istante, poi si guarda intorno nervosa" }
    if last_event_include_sequence then
        resp.sequence_name  = "checks_the_lock"
        resp.sequence_steps = { { location_id = "npce_ingresso", activity = "controlla la porta" } }
    end
    return json.encode(resp)
end

persona.register_static("event_test_npc", {
    name = "Nina", personality = "ansiosa",
    agent_system = string.rep("Sono Nina. ", 8),
})
local et_adapter = {
    getLocation = function() return "npce_ingresso" end,
    setLocation = function() end,
    isInLocation = function() return false end,
    countInLocation = function() return 0 end,
    getAppearance = function() return "NORMAL" end,
    setAppearance = function() end,
    distance = function() return 2 end,
}
local et_obj = persona.npc_object("event_test_npc", et_adapter)

-- 1st real occurrence: no reaction exists yet → onEvent no-ops (unchanged
-- behaviour), but the occurrence gets counted, no generation fires yet
-- (skip the 1st — a one-off shouldn't burn a call for something that may
-- never repeat).
local r1 = et_obj:onEvent("porta_forzata", { type = "broadcast" }, nil)
ok(r1.activity == nil, "1st occurrence: onEvent still no-ops (unchanged)")
ok(event_gen_calls == 0, "1st occurrence: no generation call yet")
local etd = persona.get("event_test_npc")
ok(etd.event_occurrence_count and etd.event_occurrence_count.porta_forzata == 1,
   "1st occurrence counted and persisted")

-- 2nd real occurrence of the SAME event: fires generation, guaranteed
-- connectivity (the event_name is verbatim, not invented)
local r2 = et_obj:onEvent("porta_forzata", { type = "broadcast" }, nil)
ok(event_gen_calls == 1, "2nd occurrence fires exactly one generation call")
etd = persona.get("event_test_npc")
ok(etd.npc_event_reactions and etd.npc_event_reactions.porta_forzata ~= nil,
   "reaction persisted for the REAL event name")
ok(et_obj.config.event_reactions and et_obj.config.event_reactions.porta_forzata ~= nil,
   "live tick object mirrored immediately (no reload needed)")

-- 3rd occurrence: the reaction now EXISTS, so npc.lua's own onEvent uses it
-- directly — closes the loop end-to-end, and no more generation is needed
-- for an event_name that already has a reaction.
local r3 = et_obj:onEvent("porta_forzata", { type = "broadcast" }, "npce_ingresso")
ok(r3.activity == "si blocca un istante, poi si guarda intorno nervosa",
   "3rd occurrence: the EARNED reaction actually fires through onEvent")
ok(event_gen_calls == 1, "no further generation once a reaction exists")

-- Atomic sequence pairing: the same call can ALSO produce a fresh sequence,
-- referenced by name — closes the old dream-schema gap (it could never
-- produce this at all, even though npc.lua's onEvent has always supported
-- reaction.sequence).
last_event_include_sequence = true
et_obj:onEvent("vicino_bussa", {}, nil)         -- 1st (counted only)
et_obj:onEvent("vicino_bussa", {}, nil)         -- 2nd (fires generation)
etd = persona.get("event_test_npc")
ok(etd.npc_event_reactions.vicino_bussa.sequence == "checks_the_lock",
   "reaction references a sequence generated in the SAME call")
ok(etd.npc_sequences and etd.npc_sequences.checks_the_lock ~= nil,
   "that sequence was actually created — guaranteed match, no orphan risk")
ok(et_obj.config.sequences and et_obj.config.sequences.checks_the_lock ~= nil,
   "live tick object's sequences mirrored too")
et_obj:onEvent("vicino_bussa", {}, "npce_ingresso")   -- 3rd: reaction fires...
ok(et_obj.current_sequence == "checks_the_lock",
   "...and npc.lua's own onEvent starts the referenced sequence (real consumption path)")

-- Cap: pre-fill event_reactions to _max_event_reactions worth of entries
-- (direct table manipulation — simulating the cap without 6 full two-
-- occurrence cycles) and verify a brand new event stops generating once at cap.
for i = 1, 10 do etd.npc_event_reactions["filler_" .. i] = { activity = "x" } end
event_gen_calls = 0
et_obj:onEvent("evento_capped", {}, nil)   -- 1st
et_obj:onEvent("evento_capped", {}, nil)   -- would be 2nd, but cap is full
ok(event_gen_calls == 0, "no generation once the event_reactions cap is reached")

print("== H) Routine crystallization stage 3: earned from repeated interaction ==")

local crystallize_calls = 0
_G.query_llm = function(sys, hist, user, schema, model, provider, label)
    if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    if schema:find('"pattern_found"', 1, true) then
        crystallize_calls = crystallize_calls + 1
        return json.encode({
            pattern_found  = true,
            time_from      = "07:00", time_to = "07:30",
            location_id    = "npch_cucina", activity = "beve un caffè da sola",
            narrative_hint = "la trovi in cucina con la tazza in mano",
        })
    end
    return "ok"   -- plain dialogue response; shape irrelevant to this test
end

persona.register_static("crystal_test_npc", {
    name = "Lidia", personality = "riservata",
    agent_system = string.rep("Sono Lidia. ", 8),
})
local ct_adapter = {
    getLocation = function() return "npch_cucina" end, setLocation = function() end,
    isInLocation = function() return false end, countInLocation = function() return 0 end,
    getAppearance = function() return "NORMAL" end, setAppearance = function() end,
    distance = function() return 2 end,
}
local ct_obj   = persona.npc_object("crystal_test_npc", ct_adapter)
local ct_agent = persona.agent_object("crystal_test_npc", { npc = ct_obj })

-- 4 interactions: below the threshold, no crystallization attempt yet
for i = 1, 4 do ct_agent:decide("il giocatore le fa una domanda numero " .. i, "") end
ok(crystallize_calls == 0, "no crystallization attempt before the 5th interaction")
local ctd = persona.get("crystal_test_npc")
ok(#ctd.interaction_log == 4, "interaction_log tracks each dialogue turn")

-- 5th interaction fires the attempt
ct_agent:decide("il giocatore le fa una domanda numero 5", "")
ok(crystallize_calls == 1, "5th interaction fires exactly one crystallization attempt")
ctd = persona.get("crystal_test_npc")
ok(#ctd.routine == 1 and ctd.routine[1].location_id == "npch_cucina",
   "pattern_found=true → a real routine slot gets written")
ok(ctd.routine[1].crystallized == true, "crystallized slot tagged for transparency")
ok(ctd.interaction_count == 0, "counter reset after the attempt")
ok(ct_obj.config.routine and #ct_obj.config.routine == 1,
   "live tick object mirrored immediately (no reload needed) — she can now be ticked")

-- Overlap guard: a second crystallization proposing an overlapping slot is
-- discarded, never corrupting the routine that already exists
for i = 1, 5 do
    if i == 5 then
        _G.query_llm = function(sys, hist, user, schema)
            if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
            if schema:find('"pattern_found"', 1, true) then
                crystallize_calls = crystallize_calls + 1
                return json.encode({
                    pattern_found = true,
                    time_from = "07:15", time_to = "07:45",   -- overlaps 07:00-07:30
                    location_id = "npch_salotto", activity = "si distrae",
                })
            end
            return "ok"
        end
    end
    ct_agent:decide("secondo giro " .. i, "")
end
ctd = persona.get("crystal_test_npc")
ok(#ctd.routine == 1, "overlapping proposed slot discarded — existing routine untouched")
ok(#ct_obj.config.routine == 1, "live object also untouched by the discarded overlap")

-- pattern_found=false is a normal outcome — nothing added, no error
for i = 1, 5 do
    if i == 5 then
        _G.query_llm = function(sys, hist, user, schema)
            if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
            if schema:find('"pattern_found"', 1, true) then
                return json.encode({ pattern_found = false })
            end
            return "ok"
        end
    end
    ct_agent:decide("terzo giro " .. i, "")
end
ctd = persona.get("crystal_test_npc")
ok(#ctd.routine == 1, "pattern_found=false adds nothing (normal, expected outcome)")

-- npc_life_event also counts toward the same interaction counter
local tools_lib2 = require("lib/tools")
local T2 = {}
for _, t in ipairs(tools_lib2.build({ persona.as_tool_life_event() })) do T2[t.name] = t.fn end
ctd.interaction_count = 0
_G.query_llm = function(sys, hist, user, schema)
    if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    if schema:find('"pattern_found"', 1, true) then
        crystallize_calls = crystallize_calls + 1
        return json.encode({ pattern_found = false })
    end
    return "ok"
end
crystallize_calls = 0
for i = 1, 5 do
    T2.npc_life_event(json.encode({ id = "crystal_test_npc", date = "day 1",
                                     event = "evento numero " .. i }))
end
ok(crystallize_calls == 1, "npc_life_event calls ALSO count toward crystallization")

print("== I) Stat generation stage 4: common baseline + personal safety net ==")

-- Common stats: deterministic backfill, no LLM call, applied regardless of
-- what a hand-authored config or the generator produced.
persona.register_static("stats_test_npc1", { name = "Bea", personality = "p",
    agent_system = string.rep("Sono Bea. ", 8) })
local st1 = persona.get("stats_test_npc1")
ok(st1.stats_defaults.energy == 0.7 and st1.stats_defaults.mood == 0.6
   and st1.stats_defaults.stress == 0.3, "common stats backfilled with no config at all")

-- Never overwrites an explicit value for the same name
persona.register_static("stats_test_npc2", { name = "Tom", personality = "p",
    agent_system = string.rep("Sono Tom. ", 8), stats_defaults = { energy = 0.1 } })
local st2 = persona.get("stats_test_npc2")
ok(st2.stats_defaults.energy == 0.1, "explicit common-name stat is NOT overwritten")
ok(st2.stats_defaults.mood == 0.6, "missing common stats still backfilled alongside it")

-- register_static's "file exists on disk" branch (step 2) also backfills —
-- simulates an OLD file written before this feature existed.
os.execute('rm -f /tmp/test_robustness_npcs/stats_test_npc3.lua')
persona.register_static("stats_test_npc3", { name = "Old", personality = "p",
    agent_system = string.rep("Sono Old. ", 8) })
-- Hand-edit the just-written file to strip stats_defaults, simulating a
-- pre-stage-4 file, then reload it via the disk-load path.
local f = io.open("/tmp/test_robustness_npcs/stats_test_npc3.lua", "r")
local content = f:read("*a"); f:close()
content = content:gsub("stats_defaults = {[^}]*},", "stats_defaults = {},")
f = io.open("/tmp/test_robustness_npcs/stats_test_npc3.lua", "w")
f:write(content); f:close()
persona.reload_all()
local st3 = persona.get("stats_test_npc3")
ok(st3.stats_defaults.energy == 0.7, "M.load (via reload_all) backfills an old file missing stats")

-- Personal-stat cap: generation producing more than _max_stats personal
-- stats gets trimmed, common stats still backfilled alongside them.
local real_vc2 = llm_util.validated_call
llm_util.validated_call = function()
    return {
        name = "Capped", age = 40, job = "x", personality = "p",
        agent_system = string.rep("Sono Capped. ", 8),
        routine = { { time_from="08:00", time_to="20:00", location_id="npci_studio", activity="a" } },
        stats_defaults = { gelosia=0.5, avidita=0.5, sospetto=0.5, paranoia=0.5, rancore=0.5, orgoglio=0.5 },
    }, nil
end
local capped = persona.generate("stats_capped_test", "un personaggio qualunque")
llm_util.validated_call = real_vc2
local personal_count = 0
for k in pairs(capped.stats_defaults) do
    if k ~= "energy" and k ~= "mood" and k ~= "stress" then personal_count = personal_count + 1 end
end
ok(personal_count == 4, "personal stats trimmed to _max_stats (6 generated → 4 kept)")
ok(capped.stats_defaults.energy == 0.7, "common stats backfilled alongside capped personal ones")

-- Personal-stat async safety net: an NPC with ONLY common stats gets one
-- fired when she becomes live (M.npc_object), and it's mirrored immediately.
persona.register_static("stats_test_npc4", { name = "Nessuno", personality = "p",
    agent_system = string.rep("Sono Nessuno. ", 8) })
local backfill_calls = 0
_G.query_llm = function(sys, hist, user, schema)
    if schema:find('"required": ["stats"]', 1, true) then
        backfill_calls = backfill_calls + 1
        return json.encode({ stats = { gelosia = 0.65 } })
    end
    return "{}"
end
local st4_adapter = {
    getLocation = function() return "npci_casa" end, setLocation = function() end,
    isInLocation = function() return false end, countInLocation = function() return 0 end,
    getAppearance = function() return "NORMAL" end, setAppearance = function() end,
    distance = function() return 2 end,
}
local st4_obj = persona.npc_object("stats_test_npc4", st4_adapter)
ok(backfill_calls == 1, "npc_object build fires exactly one personal-stat backfill")
local st4 = persona.get("stats_test_npc4")
ok(st4.stats_defaults.gelosia == 0.65, "backfilled personal stat persisted to the file")
ok(st4_obj.stats.gelosia == 0.65, "backfilled personal stat mirrored onto the live tick object")

-- Idempotent: building another npc_object for the SAME (now-personal-stat-
-- having) NPC does not fire again.
backfill_calls = 0
persona.npc_object("stats_test_npc4", st4_adapter)
ok(backfill_calls == 0, "no further backfill once a personal stat already exists")

-- Regression (real live failure, 2026-07-18): empty stats_defaults must
-- NEVER block the NPC from being created — even with a rich context that
-- explicitly names candidate stats, a real model kept returning an empty
-- stats_defaults through every retry, and the OLD hard-reject check meant
-- M.generate gave up and the character never existed at all. That's worse
-- than missing personal stats: the async safety net exists precisely to
-- fix this AFTER birth, non-blocking — it must get the chance to run.
llm_util.validated_call = function()
    return {
        name = "Ingegner Cavallaro", age = 52, job = "uomo d'affari",
        personality = "Autoritario in pubblico, sottomesso in privato.",
        agent_system = string.rep("Sono l'ingegner Cavallaro. ", 6),
        routine = { { time_from="09:00", time_to="18:00", location_id="g_ufficio", activity="lavora" } },
        stats_defaults = {},   -- exactly the observed failure mode
    }, nil
end
local backfill_calls2 = 0
_G.query_llm = function(sys, hist, user, schema)
    if schema:find('"required": ["stats"]', 1, true) then
        backfill_calls2 = backfill_calls2 + 1
        return json.encode({ stats = { segretezza = 0.9 } })
    end
    return "{}"
end
local cavallaro = persona.generate("ingegner_cavallaro_test", "un uomo d'affari")
llm_util.validated_call = real_vc2
ok(cavallaro ~= nil, "generation SUCCEEDS despite empty stats_defaults (no hard-reject)")
ok(cavallaro.stats_defaults.energy == 0.7, "common stats present immediately regardless")
ok(backfill_calls2 == 1,
   "personal-stat backfill fires from M.generate itself, unconditionally (no npc_object needed)")
ok(persona.get("ingegner_cavallaro_test").stats_defaults.segretezza == 0.9,
   "personal stat actually lands in the file")

os.execute('rm -f /tmp/test_robustness_npcs/stats_test_npc1.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc2.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc3.lua'
        .. ' /tmp/test_robustness_npcs/stats_capped_test.lua'
        .. ' /tmp/test_robustness_npcs/stats_test_npc4.lua'
        .. ' /tmp/test_robustness_npcs/ingegner_cavallaro_test.lua')

print("== J) Routine variation stage 3b: earned from WITNESSED repetition ==")

local variation_calls = 0
local var_include_bad_stat = false
_G.query_llm = function(sys, hist, user, schema)
    if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    if schema:find('"required": ["activity","prob"]', 1, true) then
        variation_calls = variation_calls + 1
        local resp = { activity = "si ferma a guardare il mare invece di lavorare",
                       prob = 0.3 }
        if var_include_bad_stat then
            resp.prob_boost_when = { stat = "statistica_inventata_x", boosted_prob = 0.8 }
        end
        return json.encode(resp)
    end
    return "{}"
end

persona.register_static("variation_test_npc", {
    name = "Rosa", personality = "sognatrice",
    agent_system = string.rep("Sono Rosa. ", 8),
    stats_defaults = { nostalgia = 0.5 },
    routine = { { time_from = "06:00", time_to = "12:00",
                  location_id = "npcv_molo", activity = "sistema le reti" } },
})
local vt_adapter = {
    getLocation = function() return "npcv_molo" end, setLocation = function() end,
    isInLocation = function() return false end, countInLocation = function() return 0 end,
    getAppearance = function() return "NORMAL" end, setAppearance = function() end,
    distance = function() return 2 end,
}
local vt_obj = persona.npc_object("variation_test_npc", vt_adapter)

-- Unwitnessed occurrences: free, count for nothing, never generate
for i = 1, 10 do
    persona.track_routine_variation("variation_test_npc", "07:00", "monday", false, vt_obj)
end
ok(variation_calls == 0, "unwitnessed occurrences never fire generation")
local vtd = persona.get("variation_test_npc")
ok((vtd.routine[1].witness_count or 0) == 0, "unwitnessed occurrences are not even counted")

-- Below threshold: witnessed but not enough yet
persona.track_routine_variation("variation_test_npc", "07:00", "monday", true, vt_obj)
persona.track_routine_variation("variation_test_npc", "07:00", "monday", true, vt_obj)
ok(variation_calls == 0, "no generation before the witness threshold (3)")
ok(vtd.routine[1].witness_count == 2, "witnessed occurrences ARE counted")

-- 3rd witnessed occurrence fires generation
persona.track_routine_variation("variation_test_npc", "07:00", "monday", true, vt_obj)
ok(variation_calls == 1, "3rd witnessed occurrence fires exactly one generation call")
vtd = persona.get("variation_test_npc")
ok(vtd.routine[1].witness_count == 0, "witness counter reset after the attempt")
ok(#vtd.routine[1].variations == 1 and vtd.routine[1].variations[1].activity
   == "si ferma a guardare il mare invece di lavorare",
   "variation written onto the slot")
ok(vtd.routine[1].variations[1].prob_boost_when == nil,
   "no prob_boost_when this round (LLM didn't propose one)")
ok(vt_obj.config.routine[1].variations and #vt_obj.config.routine[1].variations == 1,
   "variation mirrored onto the live tick object immediately")

-- Orphan stat reference: prob_boost_when naming a stat that doesn't exist
-- on this NPC is DROPPED, but the variation itself is kept (tolerate, not
-- all-or-nothing) — same class of fix as stages 2a/2b.
var_include_bad_stat = true
for i = 1, 3 do
    persona.track_routine_variation("variation_test_npc", "07:00", "monday", true, vt_obj)
end
ok(variation_calls == 2, "2nd batch of 3 witnessed occurrences fires again")
vtd = persona.get("variation_test_npc")
ok(#vtd.routine[1].variations == 2, "second variation added")
ok(vtd.routine[1].variations[2].prob_boost_when == nil,
   "invented stat name in prob_boost_when dropped — never referenced blindly")
ok(vtd.routine[1].variations[2].activity ~= nil,
   "the variation's activity/prob survive even when prob_boost_when is dropped")

-- Cap: _MAX_SLOT_VARIATIONS (2) reached — no further generation for this slot
var_include_bad_stat = false
variation_calls = 0
for i = 1, 3 do
    persona.track_routine_variation("variation_test_npc", "07:00", "monday", true, vt_obj)
end
ok(variation_calls == 0, "no generation once the slot's variation cap is reached")

-- A REAL stat in prob_boost_when is kept
persona.register_static("variation_test_npc2", {
    name = "Nico", personality = "ansioso",
    agent_system = string.rep("Sono Nico. ", 8),
    stats_defaults = { ansia = 0.4 },
    routine = { { time_from = "18:00", time_to = "22:00",
                  location_id = "npcv_bar", activity = "serve ai tavoli" } },
})
local vt2_obj = persona.npc_object("variation_test_npc2", vt_adapter)
_G.query_llm = function(sys, hist, user, schema)
    if schema:find('"required": ["stats"]', 1, true) then return '{"stats":{}}' end
    if schema:find('"required": ["activity","prob"]', 1, true) then
        return json.encode({
            activity = "si distrae e rompe un bicchiere", prob = 0.2,
            prob_boost_when = { stat = "ansia", min = 0.6, boosted_prob = 0.8 },
        })
    end
    return "{}"
end
for i = 1, 3 do
    persona.track_routine_variation("variation_test_npc2", "19:00", "monday", true, vt2_obj)
end
local vtd2 = persona.get("variation_test_npc2")
ok(vtd2.routine[1].variations[1].prob_boost_when
   and vtd2.routine[1].variations[1].prob_boost_when.stat == "ansia",
   "a REAL stat name in prob_boost_when is kept")

os.execute('rm -f /tmp/test_robustness_npcs/variation_test_npc.lua'
        .. ' /tmp/test_robustness_npcs/variation_test_npc2.lua')

print(string.format("\n== RESULT: %d passed, %d failed ==", pass, fail))
os.exit(fail == 0 and 0 or 1)
