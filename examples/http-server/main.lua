-- http-server — a programmable HTTP service: routes, JSON replies,
-- query params, and the built-in 500 contract (a handler that raises
-- before sending anything answers 500 with an empty body).
--
--   luna main.lua [port]      # default 8000, ^C stops (exit 130)
--   curl 'localhost:8000/api/echo?q=hi'
local http = require "http"

local port = tonumber(({ ... })[1]) or 8000
local started = os.time()

http.serve(function(req, res)
    if req.path == "/" then
        res:send("luna http-server example\ntry: /api/health /api/echo?q=hi /nope\n")
    elseif req.path == "/api/health" then
        res:json({ ok = true, uptime = os.time() - started })
    elseif req.path == "/api/echo" then
        res:json({ method = req.method, q = req.query.q, headers = #req.headers })
    else
        res:json({ err = req.path .. " not found" }, 404)
    end
end, port)
