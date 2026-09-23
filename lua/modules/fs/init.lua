-- fs: filesystem access.
--
-- Backed by luafilesystem (lfs), the standard Lua filesystem binding.
-- The whole lfs API is exposed unchanged (dir/attributes/mkdir/...);
-- on top of it a few Node-style conveniences are added.
local lfs = require "lfs"

local fs = setmetatable({}, { __index = lfs })

-- Node-style conveniences (thin: io.open + lfs.attributes only).
function fs.exists(path)
    return lfs.attributes(path, "mode") ~= nil
end

function fs.isFile(path)
    return lfs.attributes(path, "mode") == "file"
end

function fs.isDirectory(path)
    return lfs.attributes(path, "mode") == "directory"
end

-- fs.readFileSync(path) -> string
function fs.readFileSync(path)
    local fh, err = io.open(path, "r")
    if not fh then
        error("fs.readFileSync: cannot open " .. path .. ": " .. tostring(err), 2)
    end
    local data = fh:read("a")
    fh:close()
    return data
end

-- fs.writeFileSync(path, data) — returns true on success
function fs.writeFileSync(path, data)
    local fh, err = io.open(path, "w")
    if not fh then
        error("fs.writeFileSync: cannot open " .. path .. ": " .. tostring(err), 2)
    end
    fh:write(data)
    fh:close()
    return true
end

return fs
