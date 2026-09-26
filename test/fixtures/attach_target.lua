-- Attach-channel fixture for a pty client: defines one distinctive
-- global for remote completion, then idles until the marker file shows
-- up and leaves on its own (so it flushes its own coverage data).
-- The deadline keeps a leaked spinner from burning CPU forever: if
-- the driving test aborts before writing the marker, this exits
-- within a minute instead of spinning for hours.
local marker = assert(arg[1], "attach_target.lua needs a marker path")
local deadline = os.clock() + 60
zebra_crossing = 42
io.stderr:write("spin\n")
while not io.open(marker, "r") do
    if os.clock() > deadline then
        io.stderr:write("deadline\n")
        os.exit(1)
    end
end
io.stderr:write("done\n")
