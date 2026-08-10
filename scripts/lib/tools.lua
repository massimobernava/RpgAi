-- tools.lua — ready-made tool definitions for RpgAi scripts.
--
-- Usage in your script:
--
--   local tools = require("tools")
--
--   function get_tools()
--       return tools.build({
--           tools.roll_dice(state),
--           tools.skill_check(state),
--           tools.inventory_check(state),
--           tools.buy_item(state),
--       })
--   end
--
-- Each helper returns a ToolDef table: { name, description, params, fn }.
-- Pass your mutable `state` table so tool functions can read/write it.
-- Add custom tools by appending ToolDef tables to the list.

local json = require("json")
local M = {}

-- Wrap a ToolDef list for use as get_tools() return value.
-- Validates every ToolDef and FAILS FAST with a clear message — the engine
-- logs get_tools() errors at startup, so a broken tool is visible immediately
-- instead of silently misbehaving mid-game:
--   - duplicate names: the engine map keeps only the last one, silently
--   - params as Lua table: the engine reads params with get_or<string>,
--     a table silently becomes "{}" (no parameters visible to the LLM)
--   - missing fn: the tool gets registered but every call fails
function M.build(list)
    local seen   = {}
    local errors = {}
    for i, t in ipairs(list or {}) do
        if type(t) ~= "table" then
            table.insert(errors, "tool #" .. i .. ": not a table")
        else
            local name = t.name
            if type(name) ~= "string" or name == "" then
                table.insert(errors, "tool #" .. i .. ": missing or empty 'name'")
                name = "?"
            elseif seen[name] then
                table.insert(errors, "duplicate tool name '" .. name
                    .. "' (tool #" .. seen[name] .. " and #" .. i
                    .. ") — the engine would keep only the last one")
            else
                seen[name] = i
            end
            if type(t.fn) ~= "function" then
                table.insert(errors, "tool #" .. i .. " ('" .. name
                    .. "'): 'fn' missing or not a function")
            end
            if t.params ~= nil and type(t.params) ~= "string" then
                table.insert(errors, "tool #" .. i .. " ('" .. name
                    .. "'): 'params' must be a JSON STRING — a Lua table "
                    .. "silently becomes '{}' in the engine")
            end
        end
    end
    if #errors > 0 then
        error("tools.build: invalid tool definitions:\n  - "
            .. table.concat(errors, "\n  - "), 2)
    end

    -- Defensive fn wrapper (single chokepoint for ALL tools). The LLM side is
    -- unreliable: models send empty/whitespace/malformed `arguments`, and a raw
    -- json.decode in a tool fn would throw — surfacing a useless Lua error that
    -- the model just retries, looping. We normalize here so every tool fn runs
    -- against a valid object string and never crashes the turn:
    --   • empty / whitespace args        → "{}"  (fn's own missing-arg guidance runs)
    --   • malformed JSON                  → clear actionable error, fn NOT called
    --   • fn throws                       → clear error, no loop-inducing raw trace
    --   • fn returns non-string           → coerced to a JSON string
    -- Idempotent: a fn already wrapped (double build) is left alone.
    for _, t in ipairs(list) do
        if type(t) == "table" and type(t.fn) == "function" and not t._wrapped then
            local orig, tname = t.fn, t.name or "?"
            t.fn = function(args_json)
                if type(args_json) ~= "string" or args_json:match("^%s*$") then
                    args_json = "{}"
                end
                if not pcall(json.decode, args_json) then
                    return json.encode({ error = "Argomenti JSON non validi per '" .. tname
                        .. "'. Invia un oggetto JSON valido con i campi richiesti." })
                end
                local ok, res = pcall(orig, args_json)
                if not ok then
                    return json.encode({ error = "Tool '" .. tname .. "' fallito: "
                        .. tostring(res) })
                end
                if type(res) ~= "string" then
                    return json.encode(res ~= nil and { ok = true, result = res }
                                                   or { ok = true })
                end
                return res
            end
            t._wrapped = true
        end
    end
    return list
end

-- ---------------------------------------------------------------------------
-- Dice
-- ---------------------------------------------------------------------------

function M.roll_dice(_state)
    return {
        name = "roll_dice",
        description = "Roll one or more dice. Use for combat, skill checks, random events, and any outcome requiring real randomness. Always prefer this over inventing a result.",
        params = [[{
            "type": "object",
            "required": ["sides", "count"],
            "properties": {
                "sides": { "type": "integer", "description": "Faces per die (e.g. 6, 10, 20)" },
                "count": { "type": "integer", "description": "Number of dice to roll" }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local sides = math.max(2, math.floor(a.sides or 6))
            local count = math.max(1, math.floor(a.count or 1))
            local rolls, total = {}, 0
            for i = 1, count do
                local r = math.random(1, sides)
                rolls[i] = r
                total = total + r
            end
            return json.encode({ total = total, rolls = rolls, sides = sides })
        end
    }
end

-- ---------------------------------------------------------------------------
-- Skill check — rolls d20, adds skill bonus, compares to difficulty
-- ---------------------------------------------------------------------------

-- state must have: state.player.skills[skill_name] = integer bonus
function M.skill_check(state)
    return {
        name = "skill_check",
        description = "Perform a skill check. Rolls d20 + player's skill bonus vs difficulty. Returns success, roll, bonus, total, and whether it was a critical hit or fumble.",
        params = [[{
            "type": "object",
            "required": ["skill", "difficulty"],
            "properties": {
                "skill":      { "type": "string",  "description": "Skill name (must match a key in player.skills)" },
                "difficulty": { "type": "integer", "description": "Target number to meet or exceed" }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local bonus = (state.player.skills and state.player.skills[a.skill]) or 0
            local roll  = math.random(1, 20)
            local total = roll + bonus
            return json.encode({
                roll      = roll,
                bonus     = bonus,
                total     = total,
                difficulty = a.difficulty,
                success   = total >= a.difficulty,
                critical  = roll == 20,
                fumble    = roll == 1,
            })
        end
    }
end

-- ---------------------------------------------------------------------------
-- Inventory check
-- ---------------------------------------------------------------------------

-- state must have: state.inventory[item_id] = quantity (integer or nil)
function M.inventory_check(state)
    return {
        name = "inventory_check",
        description = "Check whether the player has a specific item and how many. Call before narrating trades, crafting, or item use.",
        params = [[{
            "type": "object",
            "required": ["item_id"],
            "properties": {
                "item_id": { "type": "string", "description": "Item identifier" }
            }
        }]],
        fn = function(args_json)
            local a   = json.decode(args_json)
            local qty = (state.inventory and state.inventory[a.item_id]) or 0
            return json.encode({ item_id = a.item_id, quantity = qty, has_item = qty > 0 })
        end
    }
end

-- ---------------------------------------------------------------------------
-- Buy item — atomic purchase with gold check
-- ---------------------------------------------------------------------------

-- state must have: state.player.gold, state.inventory
-- prices is a table: { item_id = price, ... }
function M.buy_item(state, prices)
    return {
        name = "buy_item",
        description = "Attempt to purchase an item. Deducts gold only if the player can afford it. Returns success or failure with reason.",
        params = [[{
            "type": "object",
            "required": ["item_id", "quantity"],
            "properties": {
                "item_id":  { "type": "string",  "description": "Item to purchase" },
                "quantity": { "type": "integer", "description": "How many to buy" }
            }
        }]],
        fn = function(args_json)
            local a     = json.decode(args_json)
            local price = (prices and prices[a.item_id]) or 0
            local total = price * (a.quantity or 1)
            if (state.player.gold or 0) < total then
                return json.encode({ success = false, reason = "not enough gold",
                                     gold = state.player.gold, cost = total })
            end
            state.player.gold = state.player.gold - total
            state.inventory = state.inventory or {}
            state.inventory[a.item_id] = (state.inventory[a.item_id] or 0) + (a.quantity or 1)
            return json.encode({ success = true, gold_remaining = state.player.gold, cost = total })
        end
    }
end

-- ---------------------------------------------------------------------------
-- Sell item — atomic sale
-- ---------------------------------------------------------------------------

function M.sell_item(state, prices)
    return {
        name = "sell_item",
        description = "Attempt to sell an item from the player's inventory. Adds gold only if the player has enough of that item.",
        params = [[{
            "type": "object",
            "required": ["item_id", "quantity"],
            "properties": {
                "item_id":  { "type": "string",  "description": "Item to sell" },
                "quantity": { "type": "integer", "description": "How many to sell" }
            }
        }]],
        fn = function(args_json)
            local a    = json.decode(args_json)
            local qty  = a.quantity or 1
            local have = (state.inventory and state.inventory[a.item_id]) or 0
            if have < qty then
                return json.encode({ success = false, reason = "not enough items", have = have })
            end
            local price = (prices and prices[a.item_id]) or 0
            local total = price * qty
            state.inventory[a.item_id] = have - qty
            state.player.gold = (state.player.gold or 0) + total
            return json.encode({ success = true, gold_gained = total, gold_total = state.player.gold })
        end
    }
end

-- ---------------------------------------------------------------------------
-- Query state — exposes a JSON fragment to the LLM on demand
-- ---------------------------------------------------------------------------

-- field_fn() must return a JSON-serialisable Lua table
function M.query_state(field_name, description, field_fn)
    return {
        name = "query_" .. field_name,
        description = description,
        params = [[{ "type": "object", "properties": {} }]],
        fn = function(_args_json)
            local ok, val = pcall(field_fn)
            if not ok then return json.encode({ error = tostring(val) }) end
            return json.encode(val)
        end
    }
end

-- ---------------------------------------------------------------------------
-- Remember / Forget — persistent GM notes across turns
-- ---------------------------------------------------------------------------
-- Usage: pass state table that has a `notes` field (table of strings).
-- Initialize in your script: state.notes = {}
--
-- These two tools let the LLM record/remove short narrative facts that don't
-- fit as flags (e.g. "Jenny is annoyed about yesterday", "player lied to Diane").
-- Notes are injected into the system prompt via get_system_prompt() when present.

function M.remember(state)
    return {
        name = "remember",
        description = "Store a short narrative fact to remember across future turns. Use for plot details, NPC mood changes, past events, or anything the GM should keep in mind that doesn't fit as a flag. Keep each note under 15 words.",
        params = [[{
            "type": "object",
            "required": ["note"],
            "properties": {
                "note": { "type": "string", "description": "Short fact to remember (max ~15 words)." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local note = (a.note or ""):match("^%s*(.-)%s*$")
            if note == "" then
                return json.encode({ ok = false, error = "empty note" })
            end
            state.notes = state.notes or {}
            table.insert(state.notes, note)
            if #state.notes > 25 then table.remove(state.notes, 1) end  -- FIFO: keep 25 most recent
            return json.encode({ ok = true, note = note, total = #state.notes })
        end
    }
end

function M.forget(state)
    return {
        name = "forget",
        description = "Remove a previously remembered note that is no longer relevant or has been superseded. Provide the exact text of the note to remove.",
        params = [[{
            "type": "object",
            "required": ["note"],
            "properties": {
                "note": { "type": "string", "description": "Exact text of the note to remove." }
            }
        }]],
        fn = function(args_json)
            local a = json.decode(args_json)
            local target = (a.note or ""):match("^%s*(.-)%s*$")
            state.notes = state.notes or {}
            local removed = false
            for i = #state.notes, 1, -1 do
                if state.notes[i] == target then
                    table.remove(state.notes, i)
                    removed = true
                    break
                end
            end
            return json.encode({ ok = removed, remaining = #state.notes })
        end
    }
end

return M
