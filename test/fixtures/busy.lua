-- Attach-channel fixture: a script that keeps the CPU busy until a
-- marker file shows up, then leaves on its own.
--
-- Two jobs: the busy window is exactly what `luna --attach` has to
-- reach (no line editor, so the only poll points are the kernel's
-- count hook and the client's wake signal), and exiting instead of
-- being killed lets the process flush its own coverage data.
local marker = assert(arg[1], "busy.lua needs a marker path")
io.stderr:write("spin\n")
while not io.open(marker, "r") do end
io.stderr:write("done\n")
