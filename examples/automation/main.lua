-- automation — react to a directory as it changes: fs.watch (libuv's
-- inotify face, via require("loop").fs) delivers one event per
-- create/write/rename/delete; loop.run() keeps the process alive
-- until ^C (exit 130). Not recursive: watch child directories
-- separately.
--
--   luna main.lua /tmp/watchme &
--   touch /tmp/watchme/note.txt
local loop = require "loop"
local lfs = loop.fs

local dir = ({ ... })[1] or "."

local w = lfs.watch(dir, function(err, filename, event)
    if err then
        print("watch error: " .. err)
        return
    end
    print(string.format("%s  %-8s %s", os.date("!%H:%M:%S"), event, filename))
end)

-- The idle heartbeat exists for ^C: a quiet loop blocks in epoll and
-- libuv retries an EINTR inside its poll without ending the iteration,
-- so the prepare hook (which turns a pending ^C into the "interrupted"
-- error) never sees it — the loop has to turn on its own. Same trick
-- http.serve uses; 100ms is far below human ^C latency.
local beat = loop.setInterval(function() end, 100)

print("watching " .. dir .. " — touch files there; ^C stops (exit 130)")
loop.run()
