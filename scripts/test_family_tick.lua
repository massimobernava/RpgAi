-- test_family_tick.lua — Phase 1 of the multi-narrator design (headless).
-- An autonomous NPC group ticks its scripted behaviour; beats flagged llm=true
-- are rendered by the agent (LLM), the rest stay coded. All goes into a log.
-- Asserts: flag routing, coded fallback, and the shared per-turn LLM cap.
--
-- Run: luajit scripts/test_family_tick.lua   (no network: query_llm stubbed)

package.path = "./scripts/lib/?.lua;./scripts/?.lua;" .. package.path

-- Stub the engine-provided LLM bridge BEFORE agents call it.
local llm_calls = 0
_G.query_llm = function(sys, hist, user, schema, model, provider)
    llm_calls = llm_calls + 1
    return "REAZIONE_LLM"
end

local NPC   = require("lib/npc")
local agent = require("lib/agent")

local pass, fail = 0, 0
local function ok(c, label) if c then pass=pass+1 else fail=fail+1; print("  FAIL: "..label) end end

-- Minimal world adapter (in-memory positions).
local pos = {}
local world = {
    setLocation   = function(name, loc) pos[name] = loc end,
    getLocation   = function(name) return pos[name] or "casa" end,
    setAppearance = function() end,
}

-- Three family members. 'a' and 'c' flag the breakfast beat llm=true; 'b' never.
local function cfg(activity, flagged)
    return { routine = { {
        time = {"00:00","23:59"},
        activity = activity,
        llm = flagged or nil,
        situation = flagged and (activity .. " — descrivi la scena") or nil,
    } } }
end
pos.a, pos.b, pos.c = "cucina", "salotto", "cucina"
local npc_a = NPC.new("a", cfg("A prepara la colazione", true),  world)
local npc_b = NPC.new("b", cfg("B legge il giornale",   false), world)
local npc_c = NPC.new("c", cfg("C apparecchia",          true),  world)

-- Shared counter caps total LLM reactions per turn at 1.
local tc = agent.new_turn_counter(1)
local function mk(id) return agent.new(id, { system = "Sei "..id, turn_counter = tc }) end
local members = {
    { id="a", npc=npc_a, agent=mk("a") },
    { id="b", npc=npc_b, agent=mk("b") },
    { id="c", npc=npc_c, agent=mk("c") },
}

local log = agent.tick_and_log(members,
    { time="08:00", day="lunedì", player_loc="strada" }, {}, {})

print("== log ==")
for _, e in ipairs(log) do print(string.format("  [%s] %s: %s", e.kind, e.npc, e.text)) end

-- a: flagged + counter available → LLM
ok(log[1] and log[1].npc=="a" and log[1].kind=="llm" and log[1].text=="REAZIONE_LLM",
   "a flagged → LLM reaction logged")
-- b: not flagged → coded activity
ok(log[2] and log[2].npc=="b" and log[2].kind=="coded" and log[2].text=="B legge il giornale",
   "b unflagged → coded activity logged")
-- c: flagged but counter exhausted (cap=1) → no LLM, logs coded activity
ok(log[3] and log[3].npc=="c" and log[3].kind=="coded"
   and log[3].text=="C apparecchia",
   "c flagged but over cap → no LLM, logs coded activity")
-- exactly ONE LLM call this turn despite two flagged beats
ok(llm_calls == 1, "shared cap honored: exactly 1 LLM call (got "..llm_calls..")")

print(string.format("\n== RESULT: %d passed, %d failed ==", pass, fail))
os.exit(fail == 0 and 0 or 1)
