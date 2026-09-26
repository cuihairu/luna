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

-- fs.appendFileSync(path, data) — create-or-append; true on success
function fs.appendFileSync(path, data)
    local fh, err = io.open(path, "a")
    if not fh then
        error("fs.appendFileSync: cannot open " .. path .. ": " .. tostring(err), 2)
    end
    fh:write(data)
    fh:close()
    return true
end

-- fs.readdirSync(dir) -> sorted array of entry names ("."/".."
-- excluded, like Node). Unreadable dirs raise from lfs itself.
function fs.readdirSync(dir)
    local out = {}
    for entry in lfs.dir(dir) do
        if entry ~= "." and entry ~= ".." then
            out[#out + 1] = entry
        end
    end
    table.sort(out)
    return out
end

-- fs.mkdirSync(path, [recursive]) — create a directory; with recursive,
-- missing parents are created too and an existing directory is fine
-- (Node's mkdir -p semantics). Returns true.
function fs.mkdirSync(path, recursive)
    if lfs.mkdir(path) then
        return true
    end
    if lfs.attributes(path, "mode") == "directory" then
        if recursive then
            return true -- already there: recursive mkdir is idempotent
        end
        error("fs.mkdirSync: already exists: " .. path, 2)
    end
    if recursive then
        local parent_dir = path:match("^(.*)/[^/]+")
        if parent_dir and parent_dir ~= path then
            fs.mkdirSync(parent_dir, true)
            if lfs.mkdir(path) or lfs.attributes(path, "mode") == "directory" then
                return true
            end
        end
    end
    error("fs.mkdirSync: cannot create " .. path, 2)
end

return fs
