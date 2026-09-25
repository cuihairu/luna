-- loop.http: an async HTTP/1.1 client over loop.net / connectTls.
--
-- One request, one aggregated callback: cb(err, res) with
-- res = {status = 200, headers = {lowercase keys}, body = "..."}.
-- Bodies arrive by content-length, chunked transfer decoding, or —
-- when the response carries neither — by reading to EOF (every request
-- sends Connection: close, so servers end the response there). No
-- redirects, no timeouts, no streaming bodies: a later batch can add
-- those. Plain http rides net.connect, https rides net.connectTls with
-- opts.insecure / opts.ca passed through.
--
-- Usage:
--   local http = require "loop.http"
--   http.get("https://example.com/", function(err, res) ... end)
--   http.request({url = "http://h/p", method = "POST",
--                 headers = {["X-A"] = "1"}, body = "hi"}, cb)

local net = require("loop").net

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

-- the per-request state machine: everything the feed loop needs
local function run(opts, cb)
    local u, perr = parse_url(opts.url)
    if not u then
        error(perr, 3)
    end
    local wire, method = build_request(u, opts)

    local st = {
        buf = "",
        head = nil,      -- parsed status/headers once the blank line lands
        body_len = nil,  -- content-length, when declared
        chunked = false,
        done = false,
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
        cb(err, res)
    end

    -- called for every payload byte batch after the head has been
    -- peeled off; decides whether the body is complete yet
    local function try_complete()
        local body = st.buf
        if st.chunked then
            local decoded, derr, done = decode_chunked(body)
            if decoded then
                finish(nil, { status = st.head.status,
                              reason = st.head.reason,
                              headers = st.head.headers,
                              body = decoded })
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
            -- no framing: the peer's close is the end marker, handled
            -- at EOF
        end
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
            local res, perr = parse_head(head_text)
            if not res then
                finish(perr)
                return
            end
            st.head = res
            st.buf = st.buf:sub(split + 4)
            local te = res.headers["transfer-encoding"] or ""
            st.chunked = te:lower():find("chunked", 1, true) ~= nil
            local cl = res.headers["content-length"]
            st.body_len = cl and tonumber(cl) or nil
            if st.head.status >= 100 and st.head.status < 200
                or st.head.status == 204 or st.head.status == 304 then
                -- no body by definition: the head is the whole response
                finish(nil, { status = st.head.status,
                              reason = st.head.reason,
                              headers = st.head.headers,
                              body = "" })
                return
            end
            if not st.chunked and not st.body_len then
                -- no framing headers: rely on the peer closing
                return
            end
        else
            st.buf = st.buf .. chunk
        end
        try_complete()
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
        elseif st.chunked or (st.body_len and #st.buf < st.body_len) then
            finish("loop.http: connection closed mid-body")
        else
            finish(nil, { status = st.head.status,
                          reason = st.head.reason,
                          headers = st.head.headers,
                          body = st.buf })
        end
    end

    local function connected(err, s)
        if err then
            finish(err)
            return
        end
        sock = s
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

return M
