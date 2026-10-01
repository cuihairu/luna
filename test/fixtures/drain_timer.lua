-- Batch 8: the script tail drains the loop — a plain setTimeout with
-- no loop.run() still fires (Node's "run until empty then exit"), then
-- the process exits 0 on its own.
local loop = require "loop"
loop.setTimeout(function()
    io.stderr:write("drained\n")
end, 100)
