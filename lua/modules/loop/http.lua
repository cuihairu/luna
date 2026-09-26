-- loop.http: an async HTTP/1.1 client AND server over loop.net.
--
-- Client: one request, one aggregated callback: cb(err, res) with
-- res = {status = 200, headers = {lowercase keys}, body = "..."}.
-- Bodies arrive by content-length, chunked transfer decoding, or —
-- when the response carries neither — by reading to EOF (every request
-- sends Connection: close, so servers end the response there).
--
-- Streaming: when opts.onData(chunk) is set the body flows through it
-- instead of res.body (which comes back as "" — every byte is handed
-- to onData exactly once). content-length and EOF frames deliver
-- incrementally without buffering the whole body; chunked still
-- buffers internally and hands the decoded body over in one piece at
-- the terminating chunk. opts.onHead(res) fires when the final
-- response head lands (redirect hops stay internal and never reach
-- it); an error raised inside onData/onHead becomes cb(err).
--
-- Redirects: 301/302/303/307/308 responses carrying Location are
-- followed, up to opts.maxRedirects hops (default 5). 301/302/303 fold
-- non-GET/HEAD methods down to GET and drop the body; 307/308 keep the
-- method and body. Absolute, //host/path and relative Location values
-- all resolve. maxRedirects = 0 disables following and hands the raw
-- 3xx through. Timeouts: opts.timeoutMs caps the whole request —
-- connection, redirects and body alike — and fires
-- cb("loop.http: timed out ...") while closing the socket. No
-- streaming bodies yet: a later batch can add that.
-- Plain http rides net.connect, https rides net.connectTls with
-- opts.insecure / opts.ca passed through.
--
-- Server: http.listen(host, port, handler) serves one connection per
-- request; the handler(req, res) runs once the whole request has
-- landed (req = {method, path, headers, body}), and res.send(
-- status?, body?, headers?) writes the whole response and closes.
-- A handler error becomes a 500 when nothing was sent yet.
--
-- Usage:
--   local http = require "loop.http"
--   http.get("https://example.com/", function(err, res) ... end)
--   http.request({url = "http://h/p", method = "POST",
--                 headers = {["X-A"] = "1"}, body = "hi",
--                 maxRedirects = 3, timeoutMs = 5000}, cb)
--   http.listen("127.0.0.1", 8080, function(req, res)
--     res.send("hello " .. req.path)
--   end)

local loop = require("loop")
local net = loop.net

local M = {}

-- split "scheme://host[:port]/path?query" into its parts; IPv6 hosts
-- (bracket syntax) are out of scope for now
local function parse_url(url)
    if type(url) ~= "string" then
        return nil, "loop.http: url must be a string"
    end
    local scheme, rest = url:match("^(https?)://(.+)$")
    if not scheme then
        return nil, "loop.http: cannot parse url '" .. url
            .. "' (scheme://host[:port]/path expected)"
    end
    local hostport, path = rest:match("^([^/]*)(.*)$")
    if hostport == "" then
        return nil, "loop.http: no host in url '" .. url .. "'"
    end
    if path == "" then
        path = "/"
    end
    local host, port = hostport:match("^(.-):(%d+)$")
    if not host then
        host, port = hostport, scheme == "https" and 443 or 80
    end
    return {
        https = scheme == "https",
        host = host,
        port = tonumber(port),
        path = path,
    }
end

-- resolve a Location header against the url that produced it:
-- absolute urls, //host/path, root-relative paths and bare relative
-- paths all resolve; ../ segments collapse
local function resolve_location(base, loc)
    if loc:match("^https?://") then
        return parse_url(loc)
    end
    if loc:sub(1, 2) == "//" then
        return parse_url((base.https and "https:" or "http:") .. loc)
    end
    if loc:sub(1, 1) == "/" then
        return { https = base.https, host = base.host,
                 port = base.port, path = loc }
    end
    local dir = base.path:match("^(.*/)[^/]*$") or "/"
    local merged = dir .. loc
    local parts = {}
    for seg in merged:gmatch("[^/]+") do
        if seg == ".." then
            parts[#parts] = nil
        elseif seg ~= "." then
            parts[#parts + 1] = seg
        end
    end
    return { https = base.https, host = base.host,
             port = base.port, path = "/" .. table.concat(parts, "/") }
end

-- method/opts/cb overloads: request(url, cb), request(url, opts, cb),
-- request(opts_table, cb) — the table form carries url/method/headers/
-- body alongside the transport options
local function normalize(a, b, c)
    local opts, cb
    if type(a) == "table" then
        opts = a
        cb = b
    else
        if type(b) == "table" then
            opts = b
            cb = c
        else
            opts = {}
            cb = b
        end
        opts.url = opts.url or a
    end
    if type(cb) ~= "function" then
        error("loop.http: callback required", 3)
    end
    return opts, cb
end

local function build_request(u, opts)
    local method = (opts.method or "GET"):upper()
    local lines = {
        method .. " " .. u.path .. " HTTP/1.1",
        "Connection: close",
    }
    local host = u.host
    -- non-default ports join the Host header
    if (u.https and u.port ~= 443) or (not u.https and u.port ~= 80) then
        host = host .. ":" .. u.port
    end
    lines[#lines + 1] = "Host: " .. host
    local headers = opts.headers or {}
    for k, v in pairs(headers) do
        if k:lower() ~= "host" and k:lower() ~= "connection" then
            lines[#lines + 1] = k .. ": " .. tostring(v)
        end
    end
    local body = opts.body
    if body then
        lines[#lines + 1] = "Content-Length: " .. #body
    end
    local head = table.concat(lines, "\r\n") .. "\r\n\r\n"
    return head .. (body or ""), method
end

-- decode a chunked body accumulated so far: (nil) means "need more
-- data", (body, nil, true) means the terminating chunk has landed,
-- (nil, errmsg) means the framing is broken
local function decode_chunked(data)
    local out, pos = {}, 1
    while true do
        local eol = data:find("\r\n", pos, true)
        if not eol then
            return nil
        end
        local size = tonumber(data:sub(pos, eol - 1), 16)
        if not size then
            return nil, "loop.http: bad chunk size"
        end
        if size == 0 then
            return table.concat(out), nil, true
        end
        local start = eol + 2
        if #data < start + size then
            return nil
        end
        out[#out + 1] = data:sub(start, start + size - 1)
        pos = start + size + 2 -- the CRLF that closes the chunk
    end
end

-- parse the head once the blank line shows up; returns a state table
-- or nil, errmsg on a malformed start line
local function parse_head(head)
    local version, status, reason = head:match("^(HTTP/%d%.%d) (%d%d%d)(.*)$")
    if not status then
        return nil, "loop.http: malformed response head"
    end
    local headers = {}
    local rest = head:gsub("^[^\r\n]*\r?\n", "", 1) -- drop the status line
    for line in rest:gmatch("([^\r\n]*)\r?\n") do
        local k, v = line:match("^([^:]+):%s*(.*)$")
        if k then
            k = k:lower()
            if headers[k] then
                headers[k] = headers[k] .. ", " .. v
            else
                headers[k] = v
            end
        end
    end
    return {
        status = tonumber(status),
        reason = (reason:gsub("^%s+", "")),
        headers = headers,
    }
end

-- the redirect codes; true = fold the method to GET (and drop the
-- body), false = keep the method and body
local REDIRECT_METHODS = {
    [301] = true, [302] = true, [303] = true,
    [307] = false, [308] = false,
}

-- the per-request shell: timeout timer, redirect budget, delivery.
-- attempt() runs one connection and recurses into itself through the
-- redirect chain; everything below shares the single killed flag so a
-- timeout can block any later delivery (late EOF from the closed
-- socket, a queued connect callback) from re-entering cb
local function run(opts, cb)
    -- private shallow copy: redirect folding mutates method/body and
    -- the caller's table must not notice
    local mine = {}
    for k, v in pairs(opts) do
        mine[k] = v
    end
    opts = mine
    local u0, perr = parse_url(opts.url)
    if not u0 then
        error(perr, 3)
    end

    local timeout_ms = tonumber(opts.timeoutMs)
    local max_redirects = opts.maxRedirects == nil and 5
                          or tonumber(opts.maxRedirects)

    local timer
    local active_sock
    local killed = false
    local function deliver(err, res)
        if killed then
            return
        end
        if timer then
            loop.clearTimeout(timer)
            timer = nil
        end
        active_sock = nil
        cb(err, res)
    end

    -- the whole request lives under one timer: connection, every
    -- redirect hop and the body share the budget
    if timeout_ms then
        timer = loop.setTimeout(function()
            timer = nil
            killed = true
            if active_sock then
                active_sock:close()
                active_sock = nil
            end
            cb("loop.http: timed out after " .. timeout_ms .. "ms")
        end, timeout_ms)
    end

    local function attempt(u, left)
        local wire, method = build_request(u, opts)

        -- streaming observers: onData present means the body flows
        -- through it and res.body comes back empty
        local data_cb, head_cb = opts.onData, opts.onHead
        local streaming = data_cb ~= nil

        local st = {
            buf = "",
            head = nil,      -- parsed status/headers after the blank line
            body_len = nil,  -- content-length, when declared
            chunked = false,
            done = false,
            received = 0,    -- body bytes already handed to data_cb
        }
        local sock

        local function finish(err, res)
            if st.done then
                return
            end
            st.done = true
            if sock then
                sock:close()
                sock = nil
            end
            deliver(err, res)
        end

        -- called for every payload byte batch after the head has been
        -- peeled off; decides whether the body is complete yet
        local function try_complete()
            local body = st.buf
            if st.chunked then
                local decoded, derr = decode_chunked(body)
                if decoded then
                    -- chunked streaming still buffers internally; the
                    -- decoded body goes to data_cb in one piece
                    if streaming then
                        local ok, derr2 = pcall(data_cb, decoded)
                        if not ok then
                            finish(derr2)
                            return
                        end
                        finish(nil, { status = st.head.status,
                                      reason = st.head.reason,
                                      headers = st.head.headers,
                                      body = "" })
                    else
                        finish(nil, { status = st.head.status,
                                      reason = st.head.reason,
                                      headers = st.head.headers,
                                      body = decoded })
                    end
                elseif derr then
                    finish(derr)
                end
            elseif st.body_len then
                if #body >= st.body_len then
                    finish(nil, { status = st.head.status,
                                  reason = st.head.reason,
                                  headers = st.head.headers,
                                  body = body:sub(1, st.body_len) })
                end
            else
                -- no framing: the peer's close is the end marker,
                -- handled at EOF
            end
        end

        -- body bytes once the head is out of the way: stream them
        -- through the caller's onData or buffer them for try_complete.
        -- Every path into the body — a later chunk or the tail of the
        -- chunk that carried the head — goes through here, or a
        -- head+body-in-one-packet response would bypass streaming.
        local function feed_body(chunk)
            if streaming and (st.body_len or not st.chunked) then
                -- content-length and EOF streaming: hand every byte to
                -- data_cb as it lands and keep nothing (trimmed to the
                -- declared length when there is one)
                local room = st.body_len and (st.body_len - st.received)
                             or #chunk
                local part = #chunk > room and chunk:sub(1, room) or chunk
                if #part > 0 then
                    st.received = st.received + #part
                    local ok, derr = pcall(data_cb, part)
                    if not ok then
                        finish(derr)
                        return
                    end
                end
                if st.body_len and st.received >= st.body_len then
                    finish(nil, { status = st.head.status,
                                  reason = st.head.reason,
                                  headers = st.head.headers,
                                  body = "" })
                end
                return
            end
            st.buf = st.buf .. chunk
            try_complete()
        end

        local function feed(chunk)
            if st.done then
                return
            end
            if not st.head then
                st.buf = st.buf .. chunk
                local split = st.buf:find("\r\n\r\n", 1, true)
                if not split then
                    return -- still inside the head
                end
                local head_text = st.buf:sub(1, split + 1)
                local rest = st.buf:sub(split + 4)
                local res, perr = parse_head(head_text)
                if not res then
                    finish(perr)
                    return
                end
                st.head = res
                st.buf = ""
                local code = res.status
                local loc = res.headers["location"]
                if loc and REDIRECT_METHODS[code] ~= nil
                    and max_redirects > 0 then
                    if left <= 0 then
                        finish("loop.http: too many redirects")
                        return
                    end
                    local nu, nerr = resolve_location(u, loc)
                    if not nu then
                        finish(nerr)
                        return
                    end
                    -- the old socket is dead to us from here on: mark
                    -- done so its late EOF/err can't reach the next hop
                    st.done = true
                    if sock then
                        sock:close()
                        sock = nil
                    end
                    if REDIRECT_METHODS[code]
                        and method ~= "GET" and method ~= "HEAD" then
                        -- write back through opts: the next hop
                        -- rebuilds its request line from there
                        opts.method = "GET"
                        opts.body = nil
                    end
                    attempt(nu, left - 1)
                    return
                end
                -- the head is final here: redirect hops never reach
                -- the caller's onHead
                if head_cb then
                    local ok, herr = pcall(head_cb, res)
                    if not ok then
                        finish(herr)
                        return
                    end
                end
                local te = res.headers["transfer-encoding"] or ""
                st.chunked = te:lower():find("chunked", 1, true) ~= nil
                local cl = res.headers["content-length"]
                st.body_len = cl and tonumber(cl) or nil
                if code >= 100 and code < 200
                    or code == 204 or code == 304 then
                    -- no body by definition: the head is the response
                    finish(nil, { status = st.head.status,
                                  reason = st.head.reason,
                                  headers = st.head.headers,
                                  body = "" })
                    return
                end
                -- no framing headers: the peer's close is the end
                -- marker, handled at EOF — but the tail bytes already
                -- here still stream through
                feed_body(rest)
            else
                feed_body(chunk)
            end
        end

        local on_data
        on_data = function(err, chunk)
            if st.done then
                return
            end
            if err then
                finish(err)
                return
            end
            if chunk then
                feed(chunk)
                return
            end
            -- EOF: only the unframed case can legitimately end here
            if not st.head then
                finish("loop.http: connection closed before a response")
            elseif st.chunked
                or (st.body_len
                    and (streaming and st.received or #st.buf)
                        < st.body_len) then
                finish("loop.http: connection closed mid-body")
            else
                if streaming then
                    -- unframed streaming: the rest goes to data_cb,
                    -- res.body stays empty
                    local pending = st.buf:sub(st.received + 1)
                    if #pending > 0 then
                        st.received = #st.buf
                        local ok, derr = pcall(data_cb, pending)
                        if not ok then
                            finish(derr)
                            return
                        end
                    end
                    finish(nil, { status = st.head.status,
                                  reason = st.head.reason,
                                  headers = st.head.headers,
                                  body = "" })
                else
                    finish(nil, { status = st.head.status,
                                  reason = st.head.reason,
                                  headers = st.head.headers,
                                  body = st.buf })
                end
            end
        end

        local function connected(err, s)
            if err then
                finish(err)
                return
            end
            sock = s
            active_sock = s
            s:read(on_data)
            s:write(wire, function(werr)
                if werr and not st.done then
                    finish(werr)
                end
            end)
        end

        if u.https then
            net.connectTls(u.host, u.port,
                           { insecure = opts.insecure, ca = opts.ca },
                           connected)
        else
            net.connect(u.host, u.port, connected)
        end
    end

    attempt(u0, max_redirects)
end

-- http.request(url|opts, opts?, cb)
function M.request(a, b, c)
    local opts, cb = normalize(a, b, c)
    return run(opts, cb)
end

-- http.get(url, opts?, cb): GET with no body
function M.get(a, b, c)
    local opts, cb = normalize(a, b, c)
    opts.method = "GET"
    opts.body = nil
    return run(opts, cb)
end

-- -- the server side ---------------------------------------------------
--
-- http.listen(host, port, handler) -> server (net's server: port(),
-- address(), close(), ...). One connection per request (Connection:
-- close both ways, like the client); the handler runs once the whole
-- request has landed — req = {method, path(含查询串), headers(键小写),
-- body}. Bodies arrive by content-length only (chunked request bodies
-- are not a thing our own client sends); a request line that does not
-- parse or a head past 64KiB gets a 400 and a close. A handler error
-- becomes a 500 when the response has not gone out yet.

local REASONS = {
    [200] = "OK", [201] = "Created", [204] = "No Content",
    [301] = "Moved Permanently", [302] = "Found",
    [304] = "Not Modified", [400] = "Bad Request",
    [403] = "Forbidden", [404] = "Not Found",
    [500] = "Internal Server Error", [501] = "Not Implemented",
}

-- build the res object for one connection: send(status?, body?,
-- headers?) writes the whole response and closes; idempotent
local function make_res(sock)
    local sent = false
    local res = {}
    function res.send(a, b, c)
        if sent then
            return
        end
        sent = true
        local status, body, headers
        if type(a) == "number" then
            status, body, headers = a, b, c
        else
            status, body, headers = 200, a, b
        end
        body = body or ""
        local lines = {
            "HTTP/1.1 " .. tostring(status or 200) .. " "
                .. (REASONS[status or 200] or ""),
        }
        for k, v in pairs(headers or {}) do
            local kl = k:lower()
            if kl ~= "content-length" and kl ~= "connection" then
                lines[#lines + 1] = k .. ": " .. tostring(v)
            end
        end
        lines[#lines + 1] = "Content-Length: " .. #body
        lines[#lines + 1] = "Connection: close"
        sock:write(table.concat(lines, "\r\n") .. "\r\n\r\n" .. body,
                   function()
                       sock:close()
                   end)
    end
    return res, function()
        sent = true
    end
end

local function plain_status(sock, status)
    sock:write("HTTP/1.1 " .. tostring(status) .. " "
               .. (REASONS[status] or "") .. "\r\nContent-Length: 0\r\n"
               .. "Connection: close\r\n\r\n",
               function()
                   sock:close()
               end)
end

local function serve_conn(sock, handler)
    local buf = ""
    sock:read(function(err, chunk)
        if err or not chunk then
            sock:close() -- peer gone before a whole request landed
            return
        end
        buf = buf .. chunk
        local split = buf:find("\r\n\r\n", 1, true)
        if not split then
            if #buf > 65536 then
                plain_status(sock, 400) -- head runaways get one 400
            end
            return
        end
        local head = buf:sub(1, split - 1)
        local rest = buf:sub(split + 4)
        local first = head:match("^([^\r\n]+)") or ""
        local method, path = first:match("^(%S+) (%S+) HTTP/%d%.%d$")
        if not method then
            plain_status(sock, 400)
            return
        end
        local headers = {}
        local line = head:match("\r\n(.+)$") or "" -- past the request line
        for h in line:gmatch("([^\r\n]+)") do
            local k, v = h:match("^([^:]+):%s*(.*)$")
            if k then
                k = k:lower()
                if headers[k] then
                    headers[k] = headers[k] .. ", " .. v
                else
                    headers[k] = v
                end
            end
        end
        local cl = tonumber(headers["content-length"]) or 0
        if #rest < cl then
            return -- body still in flight
        end
        local req = {
            method = method,
            path = path,
            headers = headers,
            body = rest:sub(1, cl),
        }
        local res, force_sent = make_res(sock)
        local ok, herr = pcall(handler, req, res)
        if not ok then
            force_sent()
            plain_status(sock, 500) -- the handler erred with nothing sent yet
            return
        end
    end)
end

-- http.listen(host, port, handler) -> server
function M.listen(host, port, handler)
    if type(handler) ~= "function" then
        error("loop.http: handler required", 2)
    end
    return net.listen(host, port, function(cerr, sock)
        if cerr or not sock then
            return
        end
        serve_conn(sock, handler)
    end)
end

return M
