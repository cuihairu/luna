-- Attach-channel fixture for a pty client: defines one distinctive
-- global for remote completion, then idles until the marker file shows
-- up and leaves on its own (so it flushes its own coverage data).
local marker = assert(arg[1], "attach_target.lua needs a marker path")
zebra_crossing = 42
io.stderr:write("spin\n")
while not io.open(marker, "r") do end
io.stderr:write("done\n")
