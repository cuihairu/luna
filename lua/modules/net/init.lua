-- net: TCP/UDP sockets.
--
-- Backed by luasocket; the core table is re-exported with the common
-- factories up front. The full socket namespace stays reachable via
-- require "socket".
local socket = require "socket"

return {
    tcp = socket.tcp,
    udp = socket.udp,
    connect = socket.connect, -- net.connect(host, port) -> socket
    bind = socket.bind,       -- net.bind(address, port) -> master socket
    select = socket.select,
    dns = socket.dns,
}
