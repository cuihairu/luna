-- net: TCP/UDP sockets.
--
-- Backed by luasocket; the core table is re-exported with the common
-- factories up front. The full socket namespace stays reachable via
-- require "socket".
local socket = require "socket"

local M = {
    tcp = socket.tcp,
    udp = socket.udp,
    connect = socket.connect, -- net.connect(host, port) -> socket
    bind = socket.bind,       -- net.bind(address, port) -> master socket
    select = socket.select,
    dns = socket.dns,
}

-- net.serve(host, port, handler) — a blocking one-line TCP server,
-- the script-side sibling of http.serve:
--
--   net.serve("*", 9000, function(c)
--     c:send("echo: " .. (c:receive("*l") or "") .. "\n")
--   end)
--
-- handler runs once per accepted connection with the raw luasocket
-- client (same methods as the table above). An error inside it goes to
-- stderr and the server keeps accepting; returning false stops the
-- server. The resolved address is announced on stderr, so port 0
-- (ephemeral) is still discoverable. Ctrl-C stops it too.
function M.serve(host, port, handler)
    if type(handler) ~= "function" then
        error("net.serve: handler(function) required", 2)
    end
    local master, err = socket.bind(host or "*", port, 64)
    if not master then
        error("net.serve: cannot bind " .. tostring(host or "*") .. ":"
            .. tostring(port) .. " (" .. tostring(err) .. ")", 2)
    end
    local ip, bound = master:getsockname()
    io.stderr:write(string.format("net.serve: listening on %s:%s\n",
        tostring(ip), tostring(bound)))
    io.stderr:flush()
    while true do
        local client, aerr = master:accept()
        if not client then
            io.stderr:write("net.serve: accept failed ("
                .. tostring(aerr) .. ")\n")
            io.stderr:flush()
            break
        end
        local ok, stop = pcall(handler, client)
        if not ok then
            io.stderr:write("net.serve: " .. tostring(stop) .. "\n")
            io.stderr:flush()
        elseif stop == false then
            break
        end
    end
    master:close()
end

return M
