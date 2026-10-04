-- tcp-server — a line-protocol TCP service on net.serve (sync
-- accept loop; one handler call per connection, handler errors land
-- on stderr and the server keeps accepting).
--
--   luna main.lua [port]      # default 9000, ^C stops (exit 130)
--   echo time | nc 127.0.0.1 9000
local net = require "net"

local port = tonumber(({ ... })[1]) or 9000

net.serve("127.0.0.1", port, function(c)
    c:send("luna tcp-server ready — help / time / echo <text> / quit\n")
    while true do
        local line = c:receive("*l")
        if not line then
            break -- peer closed
        end
        local cmd, rest = line:match("^(%S+)%s*(.-)$")
        if cmd == "quit" then
            c:send("bye\n")
            break
        elseif cmd == "time" then
            c:send(os.date("!%Y-%m-%dT%H:%M:%SZ") .. "\n")
        elseif cmd == "echo" then
            c:send(rest .. "\n")
        elseif cmd == "help" then
            c:send("commands: help / time / echo <text> / quit\n")
        elseif cmd then
            c:send("unknown command: " .. cmd .. "\n")
        end
    end
end)
