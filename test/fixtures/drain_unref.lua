-- Batch 8: all-unref'd handles never hold the process. The drain sees
-- no keep-alive handle and returns at once — the callback must never
-- run (it "runs while the loop turns but never keeps it turning").
local loop = require "loop"
local t = loop.setTimeout(function()
    io.stderr:write("NEVER\n")
end, 5000)
t:unref()
