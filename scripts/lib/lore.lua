-- =============================================================================
-- lib/lore.lua  —  Keyword/context-triggered lore injection
--
-- Usage:
--   local lore = require("lore")
--
--   local WORLD_LORE = lore.new({
--       { always    = true,  entry = "The kingdom is at war." },
--       { location  = "tavern",           entry = "The tavern was built in 1402." },
--       { keywords  = {"dragon","fire"},   entry = "Dragons vanished 300 years ago." },
--       { npc_present = "rossella",        entry = "Rossella is controlling by nature." },
--       { condition = function(ctx) return ctx.has_item("map") end,
--         entry     = "The map shows a secret path north." },
--   })
--
--   -- In get_system_prompt():
--   local ctx = {
--       location  = state.player.location,         -- current location id (string)
--       npcs_here = get_npcs_at_player_location(),  -- list of npc ids present (table)
--       has_item  = function(id) ... end,           -- item check function
--       text      = state.last_input or "",         -- player's last input (for keyword match)
--   }
--   return base_prompt .. WORLD_LORE:match(ctx)
--
-- Entry fields (all optional, pick one trigger per entry):
--   always      = true          — always included
--   location    = "loc_id"      — included when ctx.location matches
--   locations   = {"a","b"}     — included when ctx.location is in the list
--   keywords    = {"word",...}  — included when ctx.text contains any keyword
--   npc_present = "npc_id"      — included when npc_id is in ctx.npcs_here
--   npcs_any    = {"a","b"}     — included when any of the npc ids is in ctx.npcs_here
--   condition   = function(ctx) return bool end  — custom condition
--
--   entry       = "string"      — the lore text to inject
--   header      = "string"      — optional section header (default: none)
--   priority    = number        — sort order (lower = first). Default: 50.
-- =============================================================================

local Lore = {}
Lore.__index = Lore

function Lore.new(entries)
    local self = setmetatable({}, Lore)
    self.entries = entries or {}
    return self
end

-- Returns a string to append to the system prompt.
-- Pass section_title to wrap matched entries in a titled block (default: "LORE").
function Lore:match(ctx, section_title)
    ctx = ctx or {}
    local loc       = ctx.location or ""
    local text      = (ctx.text or ""):lower()
    local npcs_here = ctx.npcs_here or {}

    -- Build a fast lookup set for npcs_here
    local npc_set = {}
    for _, id in ipairs(npcs_here) do npc_set[id] = true end

    local matched = {}

    for _, e in ipairs(self.entries) do
        local include = false

        if e.always then
            include = true
        elseif e.location then
            include = (loc == e.location)
        elseif e.locations then
            for _, l in ipairs(e.locations) do
                if loc == l then include = true; break end
            end
        elseif e.keywords then
            for _, kw in ipairs(e.keywords) do
                if text:find(kw:lower(), 1, true) then include = true; break end
            end
        elseif e.npc_present then
            include = npc_set[e.npc_present] == true
        elseif e.npcs_any then
            for _, id in ipairs(e.npcs_any) do
                if npc_set[id] then include = true; break end
            end
        elseif e.condition then
            local ok, result = pcall(e.condition, ctx)
            include = ok and result == true
        end

        if include and e.entry then
            table.insert(matched, { entry = e.entry, header = e.header,
                                    priority = e.priority or 50 })
        end
    end

    if #matched == 0 then return "" end

    -- Sort by priority
    table.sort(matched, function(a, b) return a.priority < b.priority end)

    local parts = {}
    local title = section_title or "LORE"
    table.insert(parts, "\n\n════════════════════════════════════════")
    table.insert(parts, title)
    table.insert(parts, "════════════════════════════════════════")
    for _, m in ipairs(matched) do
        if m.header then
            table.insert(parts, "\n[" .. m.header .. "]")
        end
        table.insert(parts, m.entry)
    end

    return table.concat(parts, "\n")
end

return Lore
