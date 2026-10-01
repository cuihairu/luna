-- Batch 8: the drain segment keeps the ^C/130 contract. The main
-- chunk ends normally, a pending timer holds the drain open, and an
-- interrupt landing there must surface as "interrupted" with exit 130
-- (the timer is short because epoll resumes on EINTR only at the next
-- wakeup — the flag is read by the prepare hook of the next turn).
local loop = require "loop"
io.stderr:write("ready\n")
loop.setTimeout(function() end, 1200)
