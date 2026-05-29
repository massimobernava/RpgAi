-- =============================================================================
-- scripts/lib/npc.lua  —  Generic autonomous NPC engine for RpgAi
-- =============================================================================
--
-- QUICK START:
--   local NPC = require("lib/npc")
--   local elara = NPC.new("Elara", config, world_adapter)
--
--   -- each game tick:
--   local r = elara:update("14:30", "monday", protagonist_location)
--   -- r.activity        → string description of what Elara did
--   -- r.narrative_hint  → string|nil, set when protagonist witnesses the action
--   -- r.location        → Elara's location after update
--
--   -- collect & route events:
--   local events = elara:pop_events()
--   NPC.dispatch(events, all_npcs, protagonist_loc, dist_fn)
--
-- =============================================================================
-- CONFIG STRUCTURE
-- =============================================================================
--
-- config.stats_defaults  = { hunger=0.2, trust=0.1, ... }
-- config.stat_bounds     = { hunger={min=0,max=1}, ... }   (optional)
-- config.idle_activity   = "gazes around, enjoying a quiet moment"  (optional)
--
-- config.routine = {
--   { time={"07:00","08:30"}, day={"monday","tuesday"},
--     location="EllaraChamber", activity="sharpens her sword by the window",
--     outfit="ARMORED",
--     stats={trust=0.05},
--     narrative_hint="...",   -- shown to LLM only when protagonist is present
--     variations={
--       { prob=0.4, activity="...", stats={}, condition={}, event="...", info={},
--         narrative_hint="..." }
--     }
--   }
-- }
--
-- config.needs = {
--   { stat="hunger", threshold=0.7, time={"12:00","14:00"}, day={},
--     options={
--       { condition={{target="Innkeeper", at="Tavern"}}, sequence="seek_meal",
--         description="feels the pull of hunger and heads to the tavern" }
--     }
--   }
-- }
--
-- config.sequences = {
--   seek_meal = {
--     { location="Tavern", activity="orders a bowl of stew from the innkeeper",
--       outfit="ROBES",
--       condition={}, stats={hunger=-0.6}, object="Innkeeper", action="TRADE",
--       event="meal_purchased", info={type="direct", target="Innkeeper"},
--       narrative_hint="Elara sits at a corner table, eating quietly." },
--     ...
--   }
-- }
--
-- config.event_reactions = {
--   ["meal_purchased"] = {
--     activity="nods with satisfaction, hunger sated",
--     stats={hunger=-0.6}, outfit="ROBES", sequence=nil,
--     narrative_hint="Elara looks more relaxed after her meal."
--   }
-- }
--
-- =============================================================================
-- CONDITION ATOMS
-- =============================================================================
-- { stat="hunger",    min=0.5 }              stat >= 0.5
-- { stat="trust",     max=0.8 }              stat <= 0.8
-- { target="Guard",   at="Gatehouse" }       Guard is in Gatehouse
-- { target="self",    at="current" }         self in own current loc (always true)
-- { target="Library", is="empty" }           no agents in Library
-- { target="self",    is="alone" }           no one else in own location
-- { target="Guard",   is="nude" }            Guard's outfit == "NAKED"
--   is values: "alone" | "empty" | "occupied" | "nude"
--
-- =============================================================================
-- EVENT INFO TYPES
-- =============================================================================
-- { type="broadcast" }
-- { type="direct",   target="NpcName" }
-- { type="location", target_location="Tavern" }
-- { type="area",     origin_location="EllaraChamber", radius=20 }
--   radius is abstract (matches dist_fn units you supply to NPC.dispatch)
--
-- =============================================================================
-- WORLD ADAPTER  (you implement this per-script)
-- =============================================================================
-- {
--   getLocation    = function(name)         → string          (NPC or player)
--   setLocation    = function(name, loc)
--   isInLocation   = function(name, loc)    → bool
--   countInLocation= function(loc)          → int (all agents, including player)
--   getAppearance  = function(name)         → string
--   setAppearance  = function(name, out)
--   distance       = function(loc_a, loc_b) → number          (optional)
-- }
-- =============================================================================
-- ENGAGEMENT  (freeze NPC in place, skip routine & needs)
-- =============================================================================
-- Call npc:setEngaged(true) to prevent the NPC from following routine/needs.
-- Active sequences (already started) still run to completion.
-- Call npc:setEngaged(false) to resume normal behaviour.
--
-- Typical wiring pattern:
--   1. Add to your JSON schema:
--        "engage_npc": {
--          "type": "object",
--          "properties": { "Elara": {"type":"boolean"} },
--          "description": "true = freeze NPC movement during a scene, false = resume routine"
--        }
--   2. In process_ai_response():
--        if sc.engage_npc then
--          for name, flag in pairs(sc.engage_npc) do
--            if npcs[name] then npcs[name]:setEngaged(flag) end
--          end
--        end
--   3. In get_system_prompt() tell the LLM:
--        "Use engage_npc:{\"Elara\":true} to hold Elara in the current scene.
--         Set false when the scene ends so she resumes her normal schedule."
-- =============================================================================
-- LORE MEMORY  (lightweight event log per NPC)
-- =============================================================================
-- Stores significant events the NPC has witnessed or experienced.
-- No embeddings required — recency + importance filtering keeps tokens low.
--
--   npc:pushMemory("witnessed the hero steal from the market", 2)
--   npc:pushMemory("shared a meal at the inn", 1)
--   local ctx = npc:getMemoryContext(5)   → bullet list string, inject into prompt
--
-- importance: integer (default 1). Higher = survives trimming longer.
-- Max stored entries: config.memory_size (default 20).
--
-- Inject into get_status_for_ai() or get_system_prompt():
--   status.elara_memories = elara:getMemoryContext(5)
-- =============================================================================

local NPC = {}
NPC.__index = NPC

-- ---------------------------------------------------------------------------
-- Time helpers
-- ---------------------------------------------------------------------------

local function _hhmm(t)
    local h, m = t:match("^(%d+):(%d+)")
    return (tonumber(h) or 0) * 60 + (tonumber(m) or 0)
end

local function time_between(cur, from, to)
    local c, f, t = _hhmm(cur), _hhmm(from), _hhmm(to)
    if f <= t then return c >= f and c <= t
    else           return c >= f or  c <= t   -- midnight wrap
    end
end

local function day_ok(days, cur)
    if not days or #days == 0 then return true end
    cur = (cur or ""):lower()
    for _, d in ipairs(days) do
        if d:lower() == cur then return true end
    end
    return false
end

-- ---------------------------------------------------------------------------
-- Condition evaluator
-- stat_fn(stat_name) → number   (resolves stats for self)
-- world adapter used for agent/location checks
-- ---------------------------------------------------------------------------

local function check_conditions(conditions, world, self_name, stat_fn)
    if not conditions or #conditions == 0 then return true end

    for _, c in ipairs(conditions) do

        -- stat bounds
        if c.stat then
            local v = stat_fn(c.stat)
            if c.min and v < c.min then return false end
            if c.max and v > c.max then return false end
        end

        -- target / location checks
        if c.target then
            local tgt = (c.target == "self") and self_name or c.target

            -- determine reference location
            local loc
            if c.at then
                loc = (c.at == "current") and world.getLocation(self_name) or c.at
                -- if tgt is an agent (not the location itself), check they're there
                if tgt ~= loc and not world.isInLocation(tgt, loc) then
                    return false
                end
            else
                loc = world.getLocation(tgt) or tgt
            end

            -- "is" checks
            if c.is then
                local cnt = world.countInLocation(loc)
                if c.is == "alone"    and cnt > 1  then return false end
                if c.is == "empty"    and cnt > 0  then return false end
                if c.is == "occupied" and cnt == 0 then return false end
                if c.is == "nude" then
                    if world.getAppearance(tgt) ~= "NAKED" then return false end
                end
            end
        end
    end
    return true
end

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

-- NPC.new(name, config, world_adapter [, opts])
-- opts.stat_clamp = false  → disable auto-clamp to [0,1] (default: true)
function NPC.new(name, config, world_adapter, opts)
    opts = opts or {}
    local self  = setmetatable({}, NPC)
    self.name   = name
    self.config = config
    self.world  = world_adapter
    self.opts   = opts

    self.stats            = {}
    self.current_sequence = nil
    self.current_step     = nil
    self.pending_events   = {}
    self.engaged          = false
    self.memory           = {}

    -- Init stats from config defaults
    for k, v in pairs(config.stats_defaults or {}) do
        self.stats[k] = v
    end

    return self
end

-- ---------------------------------------------------------------------------
-- Stat management
-- ---------------------------------------------------------------------------

function NPC:getStat(s)
    return self.stats[s] or 0
end

function NPC:setStat(s, v)
    -- optional per-stat bounds from config
    if self.config.stat_bounds and self.config.stat_bounds[s] then
        local b = self.config.stat_bounds[s]
        if b.min then v = math.max(b.min, v) end
        if b.max then v = math.min(b.max, v) end
    elseif self.opts.stat_clamp ~= false then
        v = math.max(0, math.min(1, v))
    end
    self.stats[s] = v
end

function NPC:addStat(s, delta)
    self:setStat(s, self:getStat(s) + delta)
end

-- ---------------------------------------------------------------------------
-- Internal helpers
-- ---------------------------------------------------------------------------

function NPC:_myLoc()
    return self.world.getLocation(self.name) or ""
end

function NPC:_statFn()
    return function(s) return self:getStat(s) end
end

function NPC:_check(conditions)
    return check_conditions(conditions, self.world, self.name, self:_statFn())
end

function NPC:_applyStats(stats)
    if not stats then return end
    for s, v in pairs(stats) do self:addStat(s, v) end
end

function NPC:_emit(event_name, info)
    if not event_name then return end
    table.insert(self.pending_events, {
        name   = event_name,
        info   = info or {},
        source = self.name,
    })
end

-- protagonist_loc is nil/""  → protagonist not tracked this tick
function NPC:_protagonistHere(loc, protagonist_loc)
    return protagonist_loc and protagonist_loc ~= "" and protagonist_loc == loc
end

-- Build update result table
local function make_result(activity, loc, hint)
    return { activity = activity or "", location = loc or "", narrative_hint = hint }
end

-- ---------------------------------------------------------------------------
-- update(time_str, day_str, protagonist_loc) → result
--
-- result.activity        string   what the NPC did this tick
-- result.location        string   NPC's location after update
-- result.narrative_hint  string?  set when protagonist witnesses the action
-- ---------------------------------------------------------------------------

function NPC:update(time_str, day_str, protagonist_loc)
    time_str = time_str or "12:00"
    day_str  = day_str  or ""

    -- -----------------------------------------------------------------------
    -- 1. ACTIVE SEQUENCE — highest priority
    -- -----------------------------------------------------------------------
    if self.current_sequence then
        local seq = self.config.sequences and self.config.sequences[self.current_sequence]
        if not seq then
            -- sequence removed from config while running
            self.current_sequence = nil
            self.current_step     = nil
        else
            local step = seq[self.current_step]
            if not step then
                -- sequence finished
                self.current_sequence = nil
                self.current_step     = nil
            else
                -- check step guard conditions
                if step.condition and not self:_check(step.condition) then
                    self.current_sequence = nil
                    self.current_step     = nil
                    return make_result(
                        "stops abruptly — the moment has passed",
                        self:_myLoc(), nil)
                end

                -- execute step
                if step.location then
                    self.world.setLocation(self.name, step.location)
                end
                if step.outfit then
                    self.world.setAppearance(self.name, step.outfit)
                end
                if step.object then
                    if self.world.agentInteract then
                        self.world.agentInteract(self.name, step.object, step.action or "USARE")
                    end
                end
                self:_applyStats(step.stats)
                self:_emit(step.event, step.info)

                local loc  = self:_myLoc()
                local hint = nil
                if self:_protagonistHere(loc, protagonist_loc) then
                    hint = step.narrative_hint or step.activity
                end

                -- advance sequence
                self.current_step = self.current_step + 1
                if self.current_step > #seq then
                    self.current_sequence = nil
                    self.current_step     = nil
                end

                return make_result(step.activity, loc, hint)
            end
        end
    end

    -- -----------------------------------------------------------------------
    -- ENGAGEMENT CHECK — skip routine and needs while engaged
    -- -----------------------------------------------------------------------
    if self.engaged then
        return make_result(
            self.config.idle_activity or "gazes around, enjoying a quiet moment",
            self:_myLoc(), nil)
    end

    -- -----------------------------------------------------------------------
    -- 2. NEEDS — trigger sequences based on stat thresholds
    -- -----------------------------------------------------------------------
    for _, need in ipairs(self.config.needs or {}) do
        local stat_ok = self:getStat(need.stat) >= (need.threshold or 0.5)
        local time_ok = (not need.time) or time_between(time_str, need.time[1], need.time[2])
        local d_ok    = day_ok(need.day, day_str)

        if stat_ok and time_ok and d_ok then
            for _, opt in ipairs(need.options or {}) do
                if self:_check(opt.condition) then
                    self.current_sequence = opt.sequence
                    self.current_step     = 1
                    return make_result(
                        opt.description or "feels a pressing need and acts on it",
                        self:_myLoc(), nil)
                end
            end
        end
    end

    -- -----------------------------------------------------------------------
    -- 3. ROUTINE — time and day based activities
    -- -----------------------------------------------------------------------
    for _, r in ipairs(self.config.routine or {}) do
        if day_ok(r.day, day_str) and time_between(time_str, r.time[1], r.time[2]) then

            -- move to location
            if r.location then
                self.world.setLocation(self.name, r.location)
            elseif r.agent_location then
                local follow = self.world.getLocation(r.agent_location)
                if follow then self.world.setLocation(self.name, follow) end
            end
            if r.outfit then
                self.world.setAppearance(self.name, r.outfit)
            end

            -- try variations (first match wins by default; random otherwise)
            if r.variations then
                for _, v in ipairs(r.variations) do
                    -- prob_boost_when: { stat="stress", min=0.5, boosted_prob=0.7 }
                    -- when the condition is met, replaces the base prob with boosted_prob
                    local prob_ok
                    if v.prob_boost_when then
                        local pb  = v.prob_boost_when
                        local sv  = self:getStat(pb.stat or "")
                        local hit = (pb.min and sv >= pb.min) or (pb.max and sv <= pb.max)
                        local eff = (hit and pb.boosted_prob) or v.prob or 1.0
                        prob_ok   = math.random() < eff
                    else
                        prob_ok = (not v.prob) or math.random() < v.prob
                    end
                    if prob_ok and self:_check(v.condition) then
                        self:_applyStats(v.stats)
                        self:_emit(v.event, v.info)

                        local loc  = self:_myLoc()
                        local act  = (r.activity or "") .. (v.activity or "")
                        local hint = nil
                        if self:_protagonistHere(loc, protagonist_loc) then
                            hint = v.narrative_hint or act
                        end
                        return make_result(act, loc, hint)
                    end
                end
            end

            -- base routine (no variation matched)
            self:_applyStats(r.stats)
            local loc  = self:_myLoc()
            local hint = nil
            if self:_protagonistHere(loc, protagonist_loc) then
                hint = r.narrative_hint or r.activity
            end
            return make_result(r.activity, loc, hint)
        end
    end

    -- -----------------------------------------------------------------------
    -- 4. IDLE
    -- -----------------------------------------------------------------------
    return make_result(
        self.config.idle_activity or "gazes around, enjoying a quiet moment",
        self:_myLoc(), nil)
end

-- ---------------------------------------------------------------------------
-- onEvent(event_name, info, protagonist_loc) → result
--
-- result.activity       string?  NPC's reaction text (nil = no reaction)
-- result.narrative_hint string?  set when protagonist is present
-- ---------------------------------------------------------------------------

function NPC:onEvent(event_name, info, protagonist_loc)
    local reactions = self.config.event_reactions or {}
    local reaction  = reactions[event_name]
    if not reaction then return { activity = nil, narrative_hint = nil } end

    self:_applyStats(reaction.stats)

    if reaction.outfit then
        self.world.setAppearance(self.name, reaction.outfit)
    end

    -- reaction can trigger a sequence (overrides current one)
    if reaction.sequence then
        self.current_sequence = reaction.sequence
        self.current_step     = 1
    end

    -- emit a follow-up event if the reaction specifies one
    self:_emit(reaction.event, reaction.event_info)

    local loc  = self:_myLoc()
    local hint = nil
    if self:_protagonistHere(loc, protagonist_loc) then
        hint = reaction.narrative_hint or reaction.activity
    end

    return { activity = reaction.activity, narrative_hint = hint }
end

-- ---------------------------------------------------------------------------
-- pop_events() → array of { name, info, source }
-- Call after each update() to collect emitted events for dispatch.
-- ---------------------------------------------------------------------------

function NPC:pop_events()
    local ev            = self.pending_events
    self.pending_events = {}
    return ev
end

-- ---------------------------------------------------------------------------
-- Engagement control
-- ---------------------------------------------------------------------------

function NPC:setEngaged(flag)
    self.engaged = (flag == true)
end

function NPC:isEngaged()
    return self.engaged == true
end

-- ---------------------------------------------------------------------------
-- Lore memory
-- ---------------------------------------------------------------------------

-- Push a narrative event into the NPC's memory.
-- importance: integer >= 1 (default 1). Higher survives trimming longer.
function NPC:pushMemory(text, importance)
    importance = importance or 1
    table.insert(self.memory, { text=text, importance=importance, idx=#self.memory + 1 })

    local max = self.config.memory_size or 20
    if #self.memory > max then
        -- drop the oldest entry among those with the lowest importance
        local worst_i     = 1
        local worst_score = self.memory[1].importance * 1000 + self.memory[1].idx
        for i = 2, #self.memory - 1 do  -- never drop the newest entry
            local score = self.memory[i].importance * 1000 + self.memory[i].idx
            if score < worst_score then
                worst_score = score
                worst_i     = i
            end
        end
        table.remove(self.memory, worst_i)
    end
end

-- Return the top N memories as a bullet-list string, sorted chronologically.
-- Selects by importance first, then recency.
function NPC:getMemoryContext(max_entries)
    if not self.memory or #self.memory == 0 then return "" end
    max_entries = max_entries or 5

    -- rank: importance desc, then recency desc
    local ranked = {}
    for i, m in ipairs(self.memory) do
        table.insert(ranked, { text=m.text, importance=m.importance, orig_i=i })
    end
    table.sort(ranked, function(a, b)
        if a.importance ~= b.importance then return a.importance > b.importance end
        return a.orig_i > b.orig_i
    end)

    -- take top N, then re-sort chronologically for the LLM
    local selected = {}
    for i = 1, math.min(max_entries, #ranked) do
        table.insert(selected, ranked[i])
    end
    table.sort(selected, function(a, b) return a.orig_i < b.orig_i end)

    local lines = {}
    for _, m in ipairs(selected) do
        table.insert(lines, "- " .. m.text)
    end
    return table.concat(lines, "\n")
end

-- Wipe the NPC's memory.
function NPC:clearMemory()
    self.memory = {}
end

-- ---------------------------------------------------------------------------
-- snapshot() / restore()  — persistence support
-- ---------------------------------------------------------------------------

function NPC:snapshot()
    local json = require("json")
    return json.encode({
        stats            = self.stats,
        current_sequence = self.current_sequence,
        current_step     = self.current_step,
        engaged          = self.engaged,
        memory           = self.memory,
    })
end

function NPC:restore(data)
    local json = require("json")
    local s = (type(data) == "string") and json.decode(data) or data
    if not s then return end
    if s.stats            then self.stats            = s.stats            end
    if s.current_sequence then self.current_sequence = s.current_sequence end
    if s.current_step     then self.current_step     = s.current_step     end
    if s.engaged ~= nil   then self.engaged          = s.engaged          end
    if s.memory           then self.memory           = s.memory           end
end

-- ---------------------------------------------------------------------------
-- NPC.dispatch(events, npcs, protagonist_loc [, dist_fn]) → reactions
--
-- events         array of { name, info, source }  (from pop_events)
-- npcs           name-keyed table: { "Alice"=alice_npc, "Bob"=bob_npc }
-- protagonist_loc  current location of player
-- dist_fn        optional function(loc_a, loc_b) → number
--                used for "area" event type; if nil, area collapses to location match
--
-- Returns: { npc_name → { activity, narrative_hint } }  (only non-nil reactions)
-- ---------------------------------------------------------------------------

function NPC.dispatch(events, npcs, protagonist_loc, dist_fn)
    local reactions = {}

    for _, ev in ipairs(events) do
        local info  = ev.info  or {}
        local etype = info.type or "broadcast"

        for npc_name, npc in pairs(npcs) do
            if npc_name ~= ev.source then
                local receives = false

                if etype == "broadcast" then
                    receives = true

                elseif etype == "direct" then
                    receives = (npc_name == info.target)

                elseif etype == "location" then
                    receives = (npc:_myLoc() == info.target_location)

                elseif etype == "area" then
                    local npc_loc = npc:_myLoc()
                    local origin  = info.origin_location or ""
                    if npc_loc == origin then
                        receives = true
                    elseif dist_fn then
                        local d = dist_fn(npc_loc, origin)
                        receives = d ~= nil and d <= (info.radius or 0)
                    end
                end

                if receives then
                    local r = npc:onEvent(ev.name, info, protagonist_loc)
                    if r.activity then
                        reactions[npc_name] = r
                    end
                end
            end
        end

        -- also deliver to protagonist location if it matches (for narrative)
        -- the game script reads reactions to build the LLM context
    end

    return reactions
end

-- ---------------------------------------------------------------------------
-- NPC.tick(npcs, time_str, day_str, protagonist_loc [, dist_fn]) → results
--
-- Convenience: update all NPCs, collect and dispatch events in one call.
-- results: { npc_name → update_result }  (activity, location, narrative_hint)
-- All narrative_hints across results and event reactions can be injected
-- into get_system_prompt() to influence the LLM narration.
-- ---------------------------------------------------------------------------

function NPC.tick(npcs, time_str, day_str, protagonist_loc, dist_fn)
    local results    = {}
    local all_events = {}

    -- update phase
    for name, npc in pairs(npcs) do
        results[name] = npc:update(time_str, day_str, protagonist_loc)
        for _, ev in ipairs(npc:pop_events()) do
            table.insert(all_events, ev)
        end
    end

    -- dispatch phase
    if #all_events > 0 then
        local reactions = NPC.dispatch(all_events, npcs, protagonist_loc, dist_fn)
        for name, r in pairs(reactions) do
            -- merge reaction hint into the NPC's result for this tick
            if r.narrative_hint and results[name] then
                local existing = results[name].narrative_hint
                results[name].narrative_hint = existing
                    and (existing .. " | " .. r.narrative_hint)
                    or  r.narrative_hint
            end
        end
    end

    return results, all_events
end

return NPC
