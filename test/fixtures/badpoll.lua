-- Attach-channel fixture: a poll target that always raises.
--
-- The kernel's count hook pcall's __LUNA_SERVE_STEP every ~200k
-- instructions while a chunk runs; the contract is that a broken poll
-- is swallowed rather than killing the chunk. This script installs one
-- that raises on every tick and then spins until its marker file
-- appears, so the run ends by itself (status 0, coverage flushed)
-- instead of being killed from outside.
local marker = assert(arg[1], "badpoll.lua needs a marker path")
_G.__LUNA_SERVE_STEP = function()
    error("poll boom")
end
io.stderr:write("spin\n")
while not io.open(marker, "r") do end
io.stderr:write("done\n")
