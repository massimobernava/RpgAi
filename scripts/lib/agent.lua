-- scripts/lib/agent.lua
-- LLM-agent layer for RpgAi. Adds LLM-driven reactions to an existing npc.lua object
-- via COMPOSITION (not inheritance/proxy): pass npc_obj as config.npc.
--
-- DESIGN PRINCIPLES:
--   - npc.lua  = code-driven behavior (routines, needs, events, lore memory)
--   - agent.lua = LLM-driven reactions (decide, as_tool)
--   - Memory: agent READS from memory.lua; WRITING stays with the main LLM (via memory tools)
--   - Idempotent per-turn limiting: first call makes LLM request and caches response;
--     subsequent calls in the same turn return the same cached response (no extra LLM calls)
--   - Shared turn counter: one object shared across all agents; when the limit is hit,
--     fallback to a structured response derived from npc state (or a "decidi tu" signal)
--
-- QUICK START:
--
--   memory.init("my_script", "./scripts/")          -- if using persistent memory
--
--   local tc      = agent.new_turn_counter(3)        -- max 3 LLM calls/turn across ALL agents
--   local npc_obj = NPC.new("jenny", cfg, adapter)   -- optional npc.lua object
--
--   local jenny = agent.new("jenny", {
--       system       = "Sei Jenny, cuoca allegra...",
--       model        = "llama3.2",                   -- nil = engine default
--       provider     = "ollama",                     -- nil = engine default
--       npc          = npc_obj,                      -- optional: enables structured fallback
--       turn_counter = tc,                           -- optional: shared cap across all agents
--       short_term_goals = { "preparare la cena" },
--       long_term_goals  = { "aprire un ristorante" },
--   })
--
--   -- In before_ai_turn():
--   agent.reset_all_turns({ jenny, debbie }, tc)
--   NPC.tick({ jenny = npc_obj }, time, day, player_loc)
--
--   -- In get_tools():
--   return tools.build({ jenny:as_tool("think_as_jenny", "Consulta Jenny su come reagisce.") })

local json = require("json")

local M = {}
M.__index = M

local function _get_mem()
    local ok, mem = pcall(require, "lib/memory")
    return ok and mem or nil
end

-- ---------------------------------------------------------------------------
-- Shared turn counter
-- ---------------------------------------------------------------------------

--- Create a shared turn counter for a group of agents.
-- One counter limits total LLM calls across all agents in a turn.
-- Pass the same object to every agent via config.turn_counter.
--
-- @param max  Maximum total LLM calls across all agents per turn. Default: 1.
-- @return     Counter object with :increment(), :exhausted(), :reset() methods.
function M.new_turn_counter(max)
    local tc = { _count = 0, _max = max or 1 }
    function tc:increment() self._count = self._count + 1 end
    function tc:exhausted() return self._count >= self._max end
    function tc:reset()     self._count = 0 end
    return tc
end

-- ---------------------------------------------------------------------------
-- Constructor
-- ---------------------------------------------------------------------------

--- Create a new LLM agent.
-- @param id      Unique string (e.g. "jenny"). Used as key for memory and logging.
-- @param config  Table with optional fields:
--   system            string   LLM system prompt for this NPC.
--   model             string   Model override (e.g. "llama3.2"). nil = engine default.
--   provider          string   Provider ("ollama","openrouter","gemini","openai","claude"). nil = engine default.
--   npc               table    npc.lua NPC object (composition). Enables structured fallback + lore memory.
--   turn_counter      table    Shared turn counter from agent.new_turn_counter(). Optional.
--   max_per_turn      int      Max LLM calls for THIS agent per turn (default 1). Independent of shared counter.
--   max_history       int      Max messages kept before compression (default 20).
--   memory_enabled    bool     Inject persistent memory from memory.lua into prompt (default true).
--   short_term_goals  table    Current objective strings, injected into prompt.
--   long_term_goals   table    Long-term objective strings, injected into prompt.
function M.new(id, config)
    config = config or {}
    local self = setmetatable({}, M)

    self.id       = id
    self.system   = config.system   or ""
    self.model    = config.model    or nil
    self.provider = config.provider or nil

    self._npc          = config.npc          or nil
    self._turn_counter = config.turn_counter or nil

    self.max_per_turn   = config.max_per_turn  or 1
    self.max_history    = config.max_history   or 20
    self.memory_enabled = (config.memory_enabled ~= false)

    self.short_term_goals = config.short_term_goals or {}
    self.long_term_goals  = config.long_term_goals  or {}
    -- state_phrases: { stat_name → { threshold=N, threshold_low=N, phrase="..." } }
    -- When a stat crosses its threshold, the phrase is injected into the system prompt.
    self.state_phrases    = config.state_phrases    or {}

    self.history          = {}
    self._calls_this_turn = 0
    self._cached_response = nil

    return self
end

-- ---------------------------------------------------------------------------
-- Turn lifecycle
-- ---------------------------------------------------------------------------

--- Reset per-turn call counter and response cache for this agent.
function M:new_turn()
    self._calls_this_turn = 0
    self._cached_response = nil
end

--- Reset all agents in a list or name-keyed table, and optionally the shared turn counter.
-- Call from before_ai_turn() for every active agent.
-- @param agents  List or name-keyed table of agent objects.
-- @param tc      Optional shared turn counter to reset alongside agents.
function M.reset_all_turns(agents, tc)
    for _, a in pairs(agents) do
        if type(a) == "table" and a.new_turn then a:new_turn() end
    end
    if tc then tc:reset() end
end

-- ---------------------------------------------------------------------------
-- Prompt context builders
-- ---------------------------------------------------------------------------

function M:_format_states()
    local src = self._npc and self._npc.stats or nil
    if not src then return "" end

    -- If state_phrases configured: collect only phrases for stats currently over threshold.
    if self.state_phrases and next(self.state_phrases) then
        local active = {}
        for stat_name, def in pairs(self.state_phrases) do
            local val = src[stat_name]
            if val ~= nil then
                local triggered = (def.threshold     and val >  def.threshold)
                               or (def.threshold_low and val <  def.threshold_low)
                if triggered and type(def.phrase) == "string" then
                    table.insert(active, def.phrase)
                end
            end
        end
        if #active > 0 then
            return "\n[How I feel right now: " .. table.concat(active, " ") .. "]"
        end
        return ""
    end

    -- Fallback: raw stat dump when no phrases configured.
    local parts = {}
    for k, v in pairs(src) do
        table.insert(parts, k .. "=" .. string.format("%.2f", v))
    end
    if #parts == 0 then return "" end
    table.sort(parts)
    return "\nStatus: " .. table.concat(parts, ", ") .. "."
end

function M:_format_goals()
    local lines = {}
    if #self.short_term_goals > 0 then
        table.insert(lines, "Obiettivi correnti:")
        for _, g in ipairs(self.short_term_goals) do table.insert(lines, "- " .. g) end
    end
    if #self.long_term_goals > 0 then
        table.insert(lines, "Obiettivi a lungo termine:")
        for _, g in ipairs(self.long_term_goals) do table.insert(lines, "- " .. g) end
    end
    return #lines > 0 and ("\n" .. table.concat(lines, "\n")) or ""
end

function M:_format_lore_memory()
    -- In-session, importance-weighted memory from npc.lua
    if self._npc and self._npc.getMemoryContext then
        local ctx = self._npc:getMemoryContext(6)
        return ctx ~= "" and ("\nMemoria di gioco:\n" .. ctx) or ""
    end
    return ""
end

function M:_format_persistent_memory()
    -- Cross-session, disk-backed memory from memory.lua (read-only for agent)
    if not self.memory_enabled then return "" end
    local mem = _get_mem()
    if not mem then return "" end
    local text = mem.format_entity(self.id)
    return text ~= "" and ("\nRicordi permanenti:\n" .. text) or ""
end

-- ---------------------------------------------------------------------------
-- Persistent memory — READ ONLY
-- Writing to memory is the responsibility of the main LLM via memory tools.
-- ---------------------------------------------------------------------------

--- Read a cross-session memory entry for this NPC.
function M:recall(category)
    local mem = _get_mem()
    return mem and mem.read(self.id, category) or nil
end

-- ---------------------------------------------------------------------------
-- Fallback: structured response when LLM call limit is reached
-- ---------------------------------------------------------------------------

--- Return a response without making an LLM call.
-- If an npc object is linked (C): describe current npc state (location, activity).
-- Otherwise (A): return a signal string so the main LLM can decide.
function M:_fallback()
    if self._npc then
        local parts = { "[" .. self.id .. "]" }
        if self._npc.location then
            table.insert(parts, "Si trova: " .. tostring(self._npc.location))
        end
        if self._npc.current_action then
            table.insert(parts, "Sta: " .. tostring(self._npc.current_action))
        elseif self._npc.state and type(self._npc.state) == "string" then
            table.insert(parts, "Stato: " .. self._npc.state)
        end
        if self._npc.mood then
            table.insert(parts, "Umore: " .. tostring(self._npc.mood))
        end
        return table.concat(parts, ". ") .. "."
    end
    return "[" .. self.id .. ": decidi tu]"
end

-- ---------------------------------------------------------------------------
-- History compression
-- ---------------------------------------------------------------------------

function M:_compress_history()
    if #self.history < 6 then return end
    local keep_recent  = math.max(4, math.floor(#self.history / 2))
    local to_summarize = #self.history - keep_recent
    local prompt = "Riassumi in 3-5 frasi queste interazioni dal punto di vista del personaggio:\n\n"
    for i = 1, to_summarize do
        local m = self.history[i]
        prompt = prompt .. (m.role or "?") .. ": " .. (m.content or "") .. "\n"
    end
    local summary = query_llm(
        "Sei un sintetizzatore conciso di memoria narrativa per un personaggio. Rispondi solo con il riassunto.",
        "[]", prompt, "", self.model, self.provider
    )
    local new_history = {{ role="system", content="[Memoria compressa] " .. summary }}
    for i = to_summarize + 1, #self.history do
        table.insert(new_history, self.history[i])
    end
    self.history = new_history
end

-- ---------------------------------------------------------------------------
-- Core: decide() — LLM call with idempotency guard and fallback
-- ---------------------------------------------------------------------------

--- Ask the agent to react to a situation string.
-- First call per turn: makes an LLM request and caches the response.
-- Subsequent calls in the same turn: return the same cached response (no extra LLM call).
-- When per-agent or shared counter limit is exceeded: structured fallback without LLM call.
--
-- @param situation  String describing the current situation / question for the NPC.
-- @return           String response.
function M:decide(situation, schema)
    self._calls_this_turn = self._calls_this_turn + 1

    -- Idempotency: duplicate call in the same turn → return cached response
    if self._calls_this_turn > 1 then
        return self._cached_response or self:_fallback()
    end

    -- Per-agent hard cap
    if self._calls_this_turn > self.max_per_turn then
        return self:_fallback()
    end

    -- Shared counter cap (checked before the call, not after)
    if self._turn_counter and self._turn_counter:exhausted() then
        return self:_fallback()
    end

    -- Compress history if needed
    if #self.history > self.max_history then
        self:_compress_history()
    end

    -- Assemble system prompt
    local full_system = self.system
                     .. self:_format_states()
                     .. self:_format_goals()
                     .. self:_format_lore_memory()
                     .. self:_format_persistent_memory()

    local response = query_llm(full_system, json.encode(self.history), situation, schema or "", self.model, self.provider)

    -- Increment shared counter after successful call
    if self._turn_counter then self._turn_counter:increment() end

    -- Update history
    table.insert(self.history, { role="user",      content=situation })
    table.insert(self.history, { role="assistant", content=response  })

    -- Cache for idempotency
    self._cached_response = response

    return response
end

-- ---------------------------------------------------------------------------
-- Event notification (no LLM call)
-- ---------------------------------------------------------------------------

--- Notify the agent of a world event without making an LLM call.
-- Appends a system message to history so the next decide() call has context.
-- If an npc.lua object is linked, also forwards to npc:onEvent().
--
-- @param event_type      Short label (e.g. "player_arrived", "fight_started").
-- @param data            Optional string with event details.
-- @param protagonist_loc Optional location, forwarded to npc:onEvent() if linked.
function M:on_event(event_type, data, protagonist_loc)
    local content = "[Evento: " .. event_type .. (data and (": " .. data) or "") .. "]"
    table.insert(self.history, { role="system", content=content })
    if self._npc and self._npc.onEvent then
        self._npc:onEvent(event_type, {}, protagonist_loc)
    end
end

-- ---------------------------------------------------------------------------
-- Tool integration
-- ---------------------------------------------------------------------------

--- Build a ToolDef table compatible with tools.build() / get_tools().
-- @param tool_name   String — tool name exposed to the LLM (e.g. "think_as_jenny").
-- @param description String — description shown to the LLM.
-- @return ToolDef table.
function M:as_tool(tool_name, description)
    local self_ref = self
    return {
        name        = tool_name,
        description = description,
        params      = { situation = "string" },
        fn          = function(args)
            return self_ref:decide(args.situation or "")
        end,
    }
end

-- ---------------------------------------------------------------------------
-- Goals management
-- ---------------------------------------------------------------------------

function M:set_short_term_goal(goal)  self.short_term_goals = { goal }          end
function M:add_short_term_goal(goal)  table.insert(self.short_term_goals, goal) end
function M:clear_short_term_goals()   self.short_term_goals = {}                 end
function M:set_long_term_goal(goal)   self.long_term_goals  = { goal }           end
function M:add_long_term_goal(goal)   table.insert(self.long_term_goals, goal)   end

-- ---------------------------------------------------------------------------
-- Snapshot / restore
-- ---------------------------------------------------------------------------

--- Snapshot agent history and goals (for save/restore alongside main game state).
function M:agent_snapshot()
    return json.encode({
        history          = self.history,
        short_term_goals = self.short_term_goals,
        long_term_goals  = self.long_term_goals,
    })
end

--- Restore agent state from a snapshot string.
function M:agent_restore(data)
    local s = (type(data) == "string") and json.decode(data) or data
    if not s then return end
    if s.history          then self.history          = s.history          end
    if s.short_term_goals then self.short_term_goals = s.short_term_goals end
    if s.long_term_goals  then self.long_term_goals  = s.long_term_goals  end
end

return M
