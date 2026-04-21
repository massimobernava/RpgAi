-- =====================================================================
-- json_repair.lua
-- Attempts to repair a malformed JSON string produced by an LLM.
-- Returns: repaired JSON string, or nil + error message if impossible.
-- =====================================================================

local json = require("json")

local function json_repair(input)
    if not input or type(input) ~= "string" then
        return nil, "Invalid input"
    end

    local s = input

    -- ── PASS 1: Extract first JSON block if extra text is present ────
    -- Handles ```json ... ``` fences and leading/trailing text
    local extracted = s:match("```json%s*(.-)%s*```")
                   or s:match("```%s*(.-)%s*```")
    if extracted then s = extracted end

    -- Find the first { or [ and the last } or ]
    local obj_start = s:find("{")
    local arr_start = s:find("%[")
    local start_pos

    if obj_start and arr_start then
        start_pos = math.min(obj_start, arr_start)
    else
        start_pos = obj_start or arr_start
    end

    if start_pos then
        -- Find the matching closing bracket
        local open_char  = s:sub(start_pos, start_pos)
        local close_char = open_char == "{" and "}" or "]"
        local last_close = s:find(".*%" .. close_char)
        if last_close then
            s = s:sub(start_pos, last_close)
        else
            s = s:sub(start_pos)
        end
    end

    -- ── PASS 2: Single quotes → double quotes (outside strings) ──────
    -- Only if single quotes are more dominant than double quotes
    local double_count = select(2, s:gsub('"', ''))
    local single_count = select(2, s:gsub("'", ''))
    if single_count > double_count then
        s = s:gsub("'", '"')
    end

    -- ── PASS 3: Unescaped double quotes inside string values ─────────
    -- Strategy: rebuild the string character by character,
    -- tracking whether we are inside a JSON string or not.
    local function fix_unescaped_quotes(str)
        local result    = {}
        local in_string = false
        local i         = 1
        local len       = #str

        while i <= len do
            local c = str:sub(i, i)

            if c == "\\" and in_string then
                -- Escaped character: pass through as-is with the next char
                result[#result+1] = c
                i = i + 1
                if i <= len then
                    result[#result+1] = str:sub(i, i)
                end
            elseif c == '"' then
                if not in_string then
                    in_string = true
                    result[#result+1] = c
                else
                    -- We are inside a string: is this a closing quote or an internal one?
                    -- Lookahead: after a legitimate closing quote we expect , } ] : or end-of-string
                    local rest           = str:sub(i+1):match("^%s*") -- skip whitespace
                    local next_meaningful = str:sub(i + 1 + #rest, i + 1 + #rest)
                    if next_meaningful == "," or next_meaningful == "}"
                    or next_meaningful == "]" or next_meaningful == ":"
                    or next_meaningful == "" then
                        -- Legitimate closing quote
                        in_string = false
                        result[#result+1] = c
                    else
                        -- Internal unescaped quote → escape it
                        result[#result+1] = '\\"'
                    end
                end
            elseif (c == "\n" or c == "\r") and in_string then
                -- Literal newline inside a string → replace with \n escape
                result[#result+1] = "\\n"
            else
                result[#result+1] = c
            end

            i = i + 1
        end

        -- If string was never closed, close it
        if in_string then
            result[#result+1] = '"'
        end

        return table.concat(result)
    end

    s = fix_unescaped_quotes(s)

    -- ── PASS 4: Trailing commas before } or ] ────────────────────────
    s = s:gsub(",%s*}", "}")
    s = s:gsub(",%s*%]", "]")

    -- ── PASS 5: Truncated JSON — close any open structures ───────────
    -- Tracks open brackets and appends missing closing ones
    local function close_open_structures(str)
        local stack     = {}
        local in_string = false
        local i         = 1

        while i <= #str do
            local c = str:sub(i, i)
            if c == "\\" and in_string then
                i = i + 2  -- skip escaped char
            elseif c == '"' then
                in_string = not in_string
            elseif not in_string then
                if c == "{" then
                    stack[#stack+1] = "}"
                elseif c == "[" then
                    stack[#stack+1] = "]"
                elseif c == "}" or c == "]" then
                    if stack[#stack] == c then
                        stack[#stack] = nil
                    end
                end
            end
            i = i + 1
        end

        -- Append closing brackets in reverse order
        local closing = {}
        for j = #stack, 1, -1 do
            closing[#closing+1] = stack[j]
        end
        return str .. table.concat(closing)
    end

    s = close_open_structures(s)

    -- ── PASS 6: Final validation ──────────────────────────────────────
    local ok, result = pcall(json.decode, s)
    if ok then
        return s, nil  -- successfully repaired
    else
        -- Last resort: strip everything after the last valid } or ]
        local last_obj = s:match("^(.*})")
        local last_arr = s:match("^(.*%])")
        local candidate = last_obj or last_arr
        if candidate then
            ok, result = pcall(json.decode, candidate)
            if ok then
                return candidate, nil
            end
        end
        return nil, "Unable to repair: " .. tostring(result)
    end
end

-- ── Public wrapper for process_ai_move ───────────────────────────────
-- Attempts direct parse first, then repair if needed.

function safe_json_decode(raw_string)
    if not raw_string or type(raw_string) ~= "string" then
        return nil, false, "Empty or nil response from model"
    end

    -- Strip UTF-8 BOM (\xEF\xBB\xBF) if present
    if raw_string:sub(1,3) == "\xEF\xBB\xBF" then
        raw_string = raw_string:sub(4)
    end

    -- Strip leading/trailing whitespace and control characters
    raw_string = raw_string:match("^%s*(.-)%s*$")

    -- Empty string after cleaning
    if raw_string == "" then
        return nil, false, "Empty response from model"
    end

    -- Direct parse attempt
    local ok, data = pcall(json.decode, raw_string)
    if ok then
        return data, false, nil
    end

    -- Repair attempt
    local repaired, err = json_repair(raw_string)
    if repaired then
        local ok2, data2 = pcall(json.decode, repaired)
        if ok2 then
            return data2, true, nil  -- was_repaired = true
        end
    end

    return nil, false, err or "Parse failed"
end