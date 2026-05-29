-- scripts/lib/memory.lua
-- Persistent structured memory for game scripts.
-- Backed by a JSON file: {script_name}_memory.json in the script base path.
--
-- Usage:
--   local memory = require("memory")
--   memory.init("my_script", "./scripts/")   -- call once at startup
--
--   memory.write("jenny", "relationship", "trusts Anon but is jealous of Debbie")
--   memory.write("jenny", "secret",       "knows Anon read her diary")
--   local rel = memory.read("jenny", "relationship")
--   local all = memory.read_all("jenny")     -- returns table of all categories
--   memory.forget("jenny", "secret")         -- remove one entry
--   memory.forget_entity("jenny")            -- remove all entries for entity
--   memory.list_entities()                   -- returns array of known entity names
--   memory.dump()                            -- returns full DB as JSON string

local json = require("json")

local M = {}

local _data = {}   -- { entity -> { category -> content } }
local _path = nil  -- absolute path to JSON file

--- Initialize the memory system. Must be called before read/write.
-- @param script_name  Base name of the script (e.g. "my_adventure")
-- @param base_path    Directory where the file will be saved (e.g. "./scripts/")
function M.init(script_name, base_path)
    _path = (base_path or "./") .. script_name .. "_memory.json"
    local f = io.open(_path, "r")
    if f then
        local content = f:read("*all")
        f:close()
        if content and content ~= "" then
            local ok, parsed = pcall(json.decode, content)
            if ok and type(parsed) == "table" then
                _data = parsed
            end
        end
    end
end

--- Write a memory entry for an entity and category.
-- Overwrites any existing entry with the same entity+category.
-- @param entity    String key (e.g. "jenny", "casa", "player")
-- @param category  String sub-key (e.g. "relationship", "secret", "mood")
-- @param content   String value to store
function M.write(entity, category, content)
    if not _data[entity] then _data[entity] = {} end
    _data[entity][category] = tostring(content)
    M._save()
end

--- Read a memory entry.
-- @param entity    String key
-- @param category  String sub-key
-- @return          Stored string, or nil if not found
function M.read(entity, category)
    if not _data[entity] then return nil end
    return _data[entity][category]
end

--- Read all memory entries for an entity.
-- @param entity  String key
-- @return        Table of { category -> content }, or empty table
function M.read_all(entity)
    return _data[entity] or {}
end

--- Remove one memory entry.
-- @param entity    String key
-- @param category  String sub-key
function M.forget(entity, category)
    if _data[entity] then
        _data[entity][category] = nil
        if next(_data[entity]) == nil then _data[entity] = nil end
        M._save()
    end
end

--- Remove all memory entries for an entity.
-- @param entity  String key
function M.forget_entity(entity)
    if _data[entity] then
        _data[entity] = nil
        M._save()
    end
end

--- Returns a list of all entity names that have at least one memory.
function M.list_entities()
    local list = {}
    for k in pairs(_data) do table.insert(list, k) end
    table.sort(list)
    return list
end

--- Returns the full memory database as a JSON string (for debugging).
function M.dump()
    return json.encode(_data)
end

--- Format all memories for a given entity as a readable string.
-- Useful for injecting into a system prompt.
-- @param entity  String key
-- @return        Human-readable multi-line string
function M.format_entity(entity)
    local entries = _data[entity]
    if not entries then return "" end
    local lines = {}
    for cat, val in pairs(entries) do
        table.insert(lines, "- " .. cat .. ": " .. val)
    end
    table.sort(lines)
    return table.concat(lines, "\n")
end

--- Persist the current state to disk.
function M._save()
    if not _path then return end
    local f = io.open(_path, "w")
    if f then
        f:write(json.encode(_data))
        f:close()
    end
end

return M
