-- scripts/lib/llm_util.lua
-- Core robustness primitive for LLM-generated structured data.
--
-- validated_call() wraps query_llm with: JSON decode (+ json_repair second
-- chance), structural validation, and repair retries where the validation
-- error is fed back to the LLM. This turns "trust the LLM output" into
-- "verify → explain → retry → fallback".
--
-- USAGE:
--   local llm_util = require("lib/llm_util")
--
--   local data, err = llm_util.validated_call(sys, user, schema, function(d)
--       if type(d.name) ~= "string" or d.name == "" then
--           return false, "field 'name' must be a non-empty string"
--       end
--       return true
--   end, { retries = 2, model = "...", provider = "..." })
--
--   if not data then  -- all attempts failed; err = last validation error
--       ...
--   end

local json = require("json")

local M = {}

-- json_repair is optional — used as a second-chance decoder.
-- It registers the GLOBAL safe_json_decode(raw) -> data, was_repaired, err.
pcall(require, "lib/json_repair")

--- Decode a JSON string, trying json_repair if plain decode fails.
-- @return table or nil, err
function M.decode(raw)
    if type(raw) ~= "string" or raw == "" then
        return nil, "empty LLM response"
    end
    local ok, data = pcall(json.decode, raw)
    if ok and type(data) == "table" then return data end
    if type(safe_json_decode) == "function" then
        local ok2, data2 = pcall(safe_json_decode, raw)
        if ok2 and type(data2) == "table" then return data2 end
    end
    return nil, "invalid JSON: " .. raw:sub(1, 200)
end

--- LLM call with decode + validation + repair retries.
-- The validation error of each failed attempt is appended to the next
-- prompt so the LLM can fix exactly what was wrong.
--
-- @param sys          System prompt.
-- @param user         User prompt.
-- @param schema       JSON schema string (may be "").
-- @param validate_fn  Optional fn(data) -> true | false, "error description".
-- @param opts         Optional { retries=2, model, provider, history="[]" }.
-- @return             data, nil on success — nil, last_err on failure.
function M.validated_call(sys, user, schema, validate_fn, opts)
    opts = opts or {}
    local retries  = opts.retries or 2
    local last_err = "no attempt made"
    local cur_user = user

    -- Tier fallback: opts.tier = "gen"|"agent"|"ambient" fills in model/
    -- provider from the engine tier config when not explicitly passed.
    local model, provider = opts.model, opts.provider
    if opts.tier and (not model or not provider) then
        local tm, tp = M.tier(opts.tier)
        model    = model    or tm
        provider = provider or tp
    end

    -- Token accounting label: use the tier ("gen"/"agent"/"ambient") if given.
    local label = opts.label or opts.tier or "gen"
    for _ = 1, 1 + retries do
        local ok, raw = pcall(query_llm,
            sys, opts.history or "[]", cur_user, schema or "",
            model, provider, label)
        if not ok then
            last_err = "LLM call failed: " .. tostring(raw)
        else
            local data, derr = M.decode(raw)
            if not data then
                last_err = derr
            elseif not validate_fn then
                return data, nil
            else
                local valid, verr = validate_fn(data)
                if valid then return data, nil end
                last_err = tostring(verr or "validation failed")
            end
        end
        -- Feed the error back: the LLM sees what was wrong and fixes it.
        cur_user = user
            .. "\n\nYOUR PREVIOUS ATTEMPT WAS REJECTED: " .. last_err
            .. "\nFix this exact problem and reply again with valid JSON only."
    end
    return nil, last_err
end

-- ── Model tiers ────────────────────────────────────────────────────────────────

--- Resolve an engine-level model tier ("gen" | "agent" | "ambient").
-- Tiers are configured via CLI (--gen-model, --agent-model, --ambient-model
-- and matching --*-provider) or the web Settings panel, and read LIVE from
-- the engine — a settings change applies to the next call.
-- Returns model, provider (nil when unset → caller falls back to the main
-- engine model). Safe on engines without get_tier (returns nil, nil).
function M.tier(name)
    if type(get_tier) ~= "function" then return nil, nil end
    local ok, t = pcall(get_tier, name)
    if not ok or type(t) ~= "table" then return nil, nil end
    local m = (type(t.model)    == "string" and t.model    ~= "") and t.model    or nil
    local p = (type(t.provider) == "string" and t.provider ~= "") and t.provider or nil
    return m, p
end

-- ── Small validation helpers (for writing validate_fn) ────────────────────────

--- Check a value is a non-empty string. Returns true | false, err.
function M.is_str(v, field)
    if type(v) ~= "string" or v == "" then
        return false, "field '" .. field .. "' must be a non-empty string"
    end
    return true
end

--- Check a value is an array (table, all positional entries pass item_fn).
-- item_fn optional: fn(entry) -> true | false, err
function M.is_array(v, field, item_fn)
    if type(v) ~= "table" then
        return false, "field '" .. field .. "' must be an array"
    end
    if item_fn then
        for i, entry in ipairs(v) do
            local ok, err = item_fn(entry)
            if not ok then
                return false, "field '" .. field .. "' entry #" .. i .. ": "
                    .. tostring(err or "invalid")
            end
        end
    end
    return true
end

return M
