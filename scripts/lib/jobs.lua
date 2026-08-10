-- scripts/lib/jobs.lua
-- Async LLM job queue for RpgAi scripts.
--
-- Built on the engine's query_llm_async / query_llm_poll (background C++
-- thread, never blocks the turn). Fire a call now, harvest the result in a
-- later turn — this is what makes off-screen generation "breathable":
-- ambient NPC↔NPC events, background world expansion, dream pre-generation.
--
-- DESIGN: handlers are registered PER KIND at script load, not as closures
-- captured at submit time. After a restore_state the handler re-registered
-- by the script reads the CURRENT state — no stale-closure bugs.
-- Jobs are runtime-only: they live in the engine process and are NOT
-- serialized; pending jobs are lost on engine restart (by design).
--
-- USAGE:
--
--   local jobs = require("lib/jobs")
--
--   -- once, at script load (module level):
--   jobs.on("ambient", function(data, meta, err)
--       if not data then return end   -- err = decode/LLM failure
--       world.apply_ambient_result(data, meta.npc_ids, {
--           persona = persona, npc_objects = npc_objects,
--           location_id = meta.location_id, time = meta.time, date = meta.date,
--       })
--   end)
--
--   -- fire (e.g. inside the tick hook):
--   jobs.submit("ambient", sys, user, {
--       schema = world.AMBIENT_SCHEMA,
--       tier   = "ambient",            -- or model=/provider= explicit
--       meta   = { npc_ids = g.npc_ids, location_id = g.location_id,
--                  time = time_str, date = date_str },
--   })
--
--   -- harvest: adv.before_turn() calls jobs.poll_all() automatically.
--   -- Without adventure.lua, call jobs.poll_all() from before_ai_turn.

local llm_util = require("lib/llm_util")
local wlog     = require("lib/log")

local M = {}

local _handlers = {}   -- kind -> fn(data_or_raw, meta, err)
local _pending  = {}   -- array of { job_id, kind, meta, schema }

--- Register the handler for a job kind. Call at script load (module level)
-- so a reload/restore always re-registers against current state.
function M.on(kind, handler)
    _handlers[kind] = handler
end

--- Submit an async LLM job.
-- @param kind  Handler key (see M.on).
-- @param sys   System prompt.
-- @param user  User prompt.
-- @param opts  Optional {
--   schema   = JSON schema string ("" = free text; with a schema the result
--              is decoded via llm_util.decode before reaching the handler),
--   history  = history JSON (default "[]"),
--   model, provider,            -- explicit overrides
--   tier     = "gen"|"agent"|"ambient",  -- fallback when model/provider nil
--   meta     = any table, passed verbatim to the handler,
-- }
-- @return job_id string, or nil if the call could not be started.
-- Engines without query_llm_async fall back to a SYNCHRONOUS call with the
-- handler invoked immediately (slower but functionally identical).
function M.submit(kind, sys, user, opts)
    opts = opts or {}
    local schema = opts.schema or ""

    local model, provider = opts.model, opts.provider
    if opts.tier and (not model or not provider) then
        local tm, tp = llm_util.tier(opts.tier)
        model    = model    or tm
        provider = provider or tp
    end

    if type(query_llm_async) == "function" then
        local ok, job_id = pcall(query_llm_async,
            sys, opts.history or "[]", user, schema, model, provider)
        if not ok or type(job_id) ~= "string" then
            wlog.warn("jobs", "submit failed (" .. kind .. "): " .. tostring(job_id))
            return nil
        end
        table.insert(_pending, {
            job_id = job_id, kind = kind, meta = opts.meta, schema = schema,
        })
        return job_id
    end

    -- Fallback: synchronous (engine without async support, or test harness)
    local ok, raw = pcall(query_llm,
        sys, opts.history or "[]", user, schema, model, provider)
    M._dispatch(kind, opts.meta, schema, ok and raw or nil,
        ok and nil or tostring(raw))
    return "sync"
end

-- Decode (if schema) and invoke the handler, pcall-protected.
function M._dispatch(kind, meta, schema, raw, err)
    local handler = _handlers[kind]
    if not handler then
        wlog.warn("jobs", "no handler registered for kind '" .. kind .. "'")
        return
    end
    local data = raw
    if raw and schema ~= "" then
        local decoded, derr = llm_util.decode(raw)
        if decoded then data = decoded
        else data, err = nil, derr end
    end
    local ok, herr = pcall(handler, data, meta, err)
    if not ok then
        wlog.warn("jobs", "handler error (" .. kind .. "): " .. tostring(herr))
    end
end

--- Poll all pending jobs; dispatch the completed ones.
-- Call once per turn (adv.before_turn does it automatically).
-- @return number of jobs completed this poll.
function M.poll_all()
    if #_pending == 0 or type(query_llm_poll) ~= "function" then return 0 end
    local done = 0
    for i = #_pending, 1, -1 do
        local j = _pending[i]
        local ok, res = pcall(query_llm_poll, j.job_id)
        if not ok then
            table.remove(_pending, i)
            M._dispatch(j.kind, j.meta, j.schema, nil, "poll error: " .. tostring(res))
            done = done + 1
        elseif res == false then     -- job errored in the engine
            table.remove(_pending, i)
            M._dispatch(j.kind, j.meta, j.schema, nil, "LLM job failed")
            done = done + 1
        elseif type(res) == "string" then   -- done
            table.remove(_pending, i)
            M._dispatch(j.kind, j.meta, j.schema, res, nil)
            done = done + 1
        end
        -- res == nil → still pending, keep
    end
    return done
end

function M.pending() return #_pending end

--- Discard all pending jobs (results will never be dispatched).
-- Use on script swap if stale results must not touch the new state.
function M.clear() _pending = {} end

return M
