-- http: HTTP client + one-line servers.
--
-- Client face: luasocket's socket.http, re-exported unchanged
-- (request(), request_to(), ...). HTTPS needs LuaSec, which luna does
-- not bundle; plain http:// is what this face serves. For loop-
-- integrated requests (redirects, timeouts, streaming, TLS) see
-- require("loop").http instead.
--
-- Server face: http.serve() — an http server in one call, two modes:
--
--   http.serve("./docs")                    static dir on port 8000
--   http.serve("./docs", 9000)              explicit port
--   http.serve(function(req, res) ... end)  programmable handler
--   http.serve{ dir = "...", port = 9000, host = "127.0.0.1",
--               quiet = true, handler = fn }   table form
--
-- Defaults follow `python -m http.server`: directory "." (or the
-- handler's view of it), port 8000, host 0.0.0.0. The returned server
-- keeps the event loop alive until :close() (the loop.net contract)
-- and reports the resolved port — pass 0 for an ephemeral one:
--
--   srv = http.serve("./docs")   -- prints the url to stderr
--   srv.port                     -- actual port (ephemeral resolved)
--   srv.url                      -- "http://localhost:8000/"
--   srv:close()
--
-- Programmable handlers get (req, res), Node/Express-shaped:
--   req = { method, url (raw target), path (query stripped),
--           query (decoded table), headers (lowercase keys),
--           body (string or nil) }
--   res.status = 404            -- default 200; read before :send
--   res:setHeader("X-A", "1")   -- custom headers, lowercase inside
--   res:send("text")            -- text/plain unless overridden
--   res:json(obj)               -- json-encoded, application/json
-- An error raised inside a handler that has not sent yet becomes a 500
-- (loop.http's contract); the request is one connection, served and
-- closed — no keep-alive bookkeeping.

local socket_http = require "socket.http"

local M = setmetatable({}, { __index = socket_http })

-- -- shared helpers ------------------------------------------------------

-- percent-decode a url component ('+' becomes a space in query strings
-- only, decided by the caller)
local function urldecode(s, plus_space)
    local out = s:gsub("%%(%x%x)", function(h)
        return string.char(tonumber(h, 16))
    end)
    if plus_space then
        out = out:gsub("+", " ")
    end
    return out
end

-- "GET /a/b.html?x=1&y=two HTTP/1.1" already split by loop.http: here
-- just "/a/b.html?x=1&y=two" -> "/a/b.html", {x = "1", y = "two"}
local function split_query(target)
    local path, query = target:match("^([^?]*)(.*)$")
    local params = {}
    if query and #query > 1 then
        for k, v in query:sub(2):gmatch("([^=&]+)=?([^=&]*)") do
            params[urldecode(k, true)] = urldecode(v, true)
        end
    end
    return path, params
end

local function html_escape(s)
    return (s:gsub("[&<>\"]", {
        ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;",
    }))
end

-- percent-encode a path segment for use inside an href
local function href_escape(s)
    return (s:gsub("[^%w%-%._~/]", function(c)
        return string.format("%%%02X", c:byte())
    end))
end

-- -- the static face -----------------------------------------------------

local TYPES = {
    html = "text/html; charset=utf-8",
    htm  = "text/html; charset=utf-8",
    css  = "text/css; charset=utf-8",
    js   = "text/javascript; charset=utf-8",
    mjs  = "text/javascript; charset=utf-8",
    json = "application/json; charset=utf-8",
    map  = "application/json",
    txt  = "text/plain; charset=utf-8",
    md   = "text/markdown; charset=utf-8",
    xml  = "application/xml; charset=utf-8",
    csv  = "text/csv; charset=utf-8",
    lua  = "text/plain; charset=utf-8",
    yaml = "text/yaml; charset=utf-8",
    yml  = "text/yaml; charset=utf-8",
    toml = "text/plain; charset=utf-8",
    svg  = "image/svg+xml",
    png  = "image/png",
    jpg  = "image/jpeg",
    jpeg = "image/jpeg",
    gif  = "image/gif",
    webp = "image/webp",
    ico  = "image/x-icon",
    woff = "font/woff",
    woff2 = "font/woff2",
    ttf  = "font/ttf",
    otf  = "font/otf",
    pdf  = "application/pdf",
    wasm = "application/wasm",
    zip  = "application/zip",
    gz   = "application/gzip",
    mp3  = "audio/mpeg",
    mp4  = "video/mp4",
    webm = "video/webm",
}

local function content_type_of(file)
    return TYPES[file:match("%.([%w]+)$") or ""] or "application/octet-stream"
end

-- a minimal directory listing, python -m http.server shaped: entries
-- link-encoded, directories first, "/" suffixed
local function send_listing(res, full, url_path)
    local fs = require "fs"
    local dirs, files = {}, {}
    for name in fs.dir(full) do
        if name ~= "." and name ~= ".." then
            if fs.isDirectory(full .. "/" .. name) then
                dirs[#dirs + 1] = name .. "/"
            else
                files[#files + 1] = name
            end
        end
    end
    local function bytename(a, b)
        return a:lower() < b:lower()
    end
    table.sort(dirs, bytename)
    table.sort(files, bytename)
    local parts = {
        "<!doctype html><html><head><meta charset=\"utf-8\">",
        "<title>Index of " .. html_escape(url_path) .. "</title></head><body>",
        "<h1>Index of " .. html_escape(url_path) .. "</h1><hr><ul>",
    }
    if url_path ~= "/" then
        parts[#parts + 1] = "<li><a href=\"../\">../</a></li>"
    end
    for _, name in ipairs(dirs) do
        parts[#parts + 1] = "<li><a href=\"" .. href_escape(name)
            .. "\">" .. html_escape(name) .. "</a></li>"
    end
    for _, name in ipairs(files) do
        parts[#parts + 1] = "<li><a href=\"" .. href_escape(name)
            .. "\">" .. html_escape(name) .. "</a></li>"
    end
    parts[#parts + 1] = "</ul><hr>luna serve</body></html>"
    res:setHeader("Content-Type", "text/html; charset=utf-8")
    res:send(table.concat(parts, "\n"))
end

local function send_file(res, full, head_only)
    local fh, err = io.open(full, "rb")
    if not fh then
        res.status = 403
        res:send("403 Forbidden: cannot read " .. full .. "\n")
        return
    end
    local body = fh:read("a")
    fh:close()
    res:setHeader("Content-Type", content_type_of(full))
    res:send(head_only and "" or body)
end

local function static_handler(root)
    return function(req, res)
        if req.method ~= "GET" and req.method ~= "HEAD" then
            res.status = 405
            res:setHeader("Allow", "GET, HEAD")
            res:send("405 Method Not Allowed: " .. req.method .. "\n")
            return
        end
        local head_only = req.method == "HEAD"
        local rel = urldecode(req.path)
        if rel:find("\0", 1, true) then
            res.status = 400
            res:send("400 Bad Request\n")
            return
        end
        -- traversal is refused outright: any ".." segment never reaches
        -- the filesystem, whatever encoding wrapped it
        local segs = {}
        for seg in rel:gmatch("[^/]+") do
            if seg == ".." then
                res.status = 403
                res:send("403 Forbidden: " .. req.path .. "\n")
                return
            end
            segs[#segs + 1] = seg
        end
        local full = root
        if #segs > 0 then
            full = root .. "/" .. table.concat(segs, "/")
        end
        local fs = require "fs"
        local mode = fs.attributes(full, "mode")
        if not mode then
            res.status = 404
            res:send("404 Not Found: " .. req.path .. "\n")
            return
        end
        if mode == "directory" then
            -- link correctness wants the trailing slash: redirect bare
            -- directory urls once instead of serving a listing whose
            -- relative links resolve against the wrong base
            if not rel:match("/$") then
                res.status = 301
                res:setHeader("Location", req.path .. "/")
                res:send("")
                return
            end
            local index = full .. "/index.html"
            if fs.isFile(index) then
                return send_file(res, index, head_only)
            end
            return send_listing(res, full, rel)
        end
        return send_file(res, full, head_only)
    end
end

-- -- the server face -----------------------------------------------------

-- wrap a user handler into loop.http's raw (req, res) shape: query
-- parsed off the path, res gains status/setHeader/json over the raw
-- send-then-close
local function adapt(handler)
    local json = require "json"
    return function(req_raw, res_raw)
        local path, query = split_query(req_raw.path)
        local headers = {}
        local res
        res = {
            status = 200,
        }
        function res:setHeader(k, v)
            headers[k:lower()] = tostring(v)
        end
        local function commit(status, body, default_type)
            headers["content-type"] = headers["content-type"] or default_type
            res_raw.send(status, body, headers)
        end
        function res:send(body)
            commit(self.status or 200, body or "",
                   "text/plain; charset=utf-8")
        end
        function res:json(value, status)
            if status ~= nil then
                self.status = status
            end
            commit(self.status or 200, json.encode(value),
                   "application/json; charset=utf-8")
        end
        handler({
            method = req_raw.method,
            url = req_raw.path,
            path = path,
            query = query,
            headers = req_raw.headers,
            body = req_raw.body,
        }, res)
    end
end

-- http.serve(dir_or_handler [, port]) or http.serve{...} -> server
function M.serve(a, b)
    local opts = {}
    if type(a) == "table" then
        for k, v in pairs(a) do
            opts[k] = v
        end
    else
        if type(a) == "function" then
            opts.handler = a
        elseif type(a) == "string" then
            opts.dir = a
        elseif a ~= nil then
            error("http.serve: directory path or handler function "
                  .. "expected, got " .. type(a), 2)
        end
        if b ~= nil then
            if type(b) ~= "number" then
                error("http.serve: port must be a number, got "
                      .. type(b), 2)
            end
            opts.port = b
        end
    end
    local port = opts.port or 8000
    local host = opts.host or "0.0.0.0"
    local kind, target
    if opts.handler ~= nil then
        if type(opts.handler) ~= "function" then
            error("http.serve: handler must be a function, got "
                  .. type(opts.handler), 2)
        end
        kind, target = "handler", opts.handler
    else
        kind = "static"
        target = require("path").resolve(opts.dir or ".")
        if require("fs").attributes(target, "mode") ~= "directory" then
            error("http.serve: no such directory: "
                  .. (opts.dir or "."), 2)
        end
    end

    local lhttp = require "loop.http"
    local handler = kind == "handler" and adapt(target)
                    or adapt(static_handler(target))
    local ok, listener = pcall(lhttp.listen, host, port, handler)
    if not ok then
        -- loop.net's own wording ("listen failed: address already in
        -- use") keeps the cause; anything before it is our chunkname,
        -- which has no business in a user-facing message
        local raw = tostring(listener)
        local cause = raw:match("loop%.net: (.*)$") or raw
        error("http.serve: cannot listen on " .. host .. ":" .. port
              .. " (" .. cause .. ")", 2)
    end

    local display = (host == "0.0.0.0" or host == "::") and "localhost"
                    or host
    -- the idle heartbeat exists for ^C: a quiet server blocks in epoll
    -- and libuv retries an EINTR inside its poll loop without ending
    -- the iteration, so the loop must turn on its own for the prepare
    -- hook to see the interrupt flag — whether that run is serveCli's
    -- loop.run() or a script's tail drain. 100ms sits far below human
    -- ^C latency and costs nothing measurable; cleared on close so a
    -- closed server lets the loop (and the process) wind down.
    local loop = require("loop")
    local beat = loop.setInterval(function() end, 100)
    local srv = {
        host = host,
        port = listener:port(),
        url = "http://" .. display .. ":" .. listener:port() .. "/",
    }
    if kind == "static" then
        srv.dir = target
    end
    function srv:close(cb)
        loop.clearInterval(beat)
        return listener:close(cb)
    end
    function srv:address()
        return listener:address()
    end
    if not opts.quiet then
        io.stderr:write("luna: serving "
            .. (kind == "static" and target or "(handler)")
            .. " at " .. srv.url .. " (^C to stop)\n")
    end
    return srv
end

-- -- the CLI face (luna serve [directory] [port]) ------------------------

local USAGE = [[
usage: luna serve [directory] [port]

  luna serve             serve . on port 8000
  luna serve docs        serve ./docs on port 8000
  luna serve docs 9000   serve ./docs on port 9000
  luna serve 9000        serve . on port 9000

The server keeps running until ^C. Programmable servers take one line:

  luna -e 'http = require("http") http.serve(function(req, res)
    res:json({hello = req.path}) end, 8000)']]

local function fail(msg, code)
    io.stderr:write(msg .. "\n")
    return code
end

-- serveCli({dir?, port?} as raw strings/numbers from arg[]) -> exit code
function M.serveCli(argv)
    local dir, port
    for _, a in ipairs(argv or {}) do
        if a == "-h" or a == "--help" then
            print(USAGE)
            return 0
        elseif a:sub(1, 1) == "-" then
            return fail("luna serve: unknown option '" .. a .. "'\n\n"
                        .. USAGE, 2)
        elseif tonumber(a) then
            if port ~= nil then
                return fail("luna serve: port given twice ("
                            .. port .. " and " .. a .. ")", 2)
            end
            port = tonumber(a)
        else
            if dir ~= nil then
                return fail("luna serve: directory given twice ('"
                            .. dir .. "' and '" .. a .. "')", 2)
            end
            dir = a
        end
    end
    if port ~= nil and (port < 1 or port > 65535 or port % 1 ~= 0) then
        return fail("luna serve: port must be a whole number 1-65535, got "
                    .. tostring(port), 2)
    end
    if dir ~= nil and require("fs").attributes(dir, "mode") ~= "directory" then
        return fail("luna serve: no such directory: " .. dir, 1)
    end
    local ok, srv = pcall(M.serve, dir, port)
    if not ok then
        return fail("luna: " .. tostring(srv), 1)
    end
    -- the loop runs until :close() or ^C; a ^C lands here as
    -- loop.run's "interrupted" error — the same 128+SIGINT contract
    -- the rest of luna keeps (the heartbeat that makes it land lives
    -- in serve(), so every serve shape gets it, not just the CLI)
    local okrun, err = pcall(function()
        require("loop").run()
    end)
    pcall(srv.close, srv)
    if not okrun then
        if tostring(err):find("interrupted", 1, true) then
            io.stderr:write("luna: ^C — shutting down\n")
            return 130
        end
        return fail("luna serve: " .. tostring(err), 1)
    end
    return 0
end

return M
