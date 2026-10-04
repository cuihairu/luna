-- file-tool — a wc-shaped command-line tool: sync fs reads, table
-- formatting, totals. Missing files raise with the path in the
-- message (fs.readFileSync contract), so the usage guard is the only
-- failure path this script owns.
--
--   luna main.lua ../../README.md ../../todo.md
local fs = require "fs"

local files = { ... }
if #files == 0 then
    io.stderr:write("usage: luna main.lua <file>...\n")
    os.exit(1)
end

local tl, tw, tb = 0, 0, 0
for _, f in ipairs(files) do
    local text = fs.readFileSync(f)
    local lines = 0
    if #text > 0 then
        lines = select(2, text:gsub("\n", ""))
        if text:sub(-1) ~= "\n" then
            lines = lines + 1
        end
    end
    local words = select(2, text:gsub("%S+", ""))
    print(string.format("%7d %7d %7d  %s", lines, words, #text, f))
    tl, tw, tb = tl + lines, tw + words, tb + #text
end

if #files > 1 then
    print(string.format("%7d %7d %7d  total", tl, tw, tb))
end
