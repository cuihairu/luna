-- web-scraper — fetch a page and pull structure out of it: title and
-- href links via pattern matching. Plain HTTP only (the luasocket
-- client has no TLS; see docs: HTTPS goes through loop.http).
--
--   luna main.lua http://localhost:8000/    # or any http:// URL
local http = require "http"

local url = ({ ... })[1]
if not url then
    io.stderr:write("usage: luna main.lua http://<host>[:port]/\n")
    os.exit(1)
end

local body, code = http.request(url)
if not body then
    io.stderr:write("fetch failed: " .. tostring(code) .. "\n")
    os.exit(1)
end

local title = body:match("<title[^>]*>(.-)</title>")
print(string.format("fetched %s — HTTP %s, %d bytes", url, tostring(code), #body))
print("title: " .. (title and title:gsub("%s+$", "") or "(none)"))

local links = {}
for href in body:gmatch('href=[\'"]([^\'"]+)[\'"]') do
    links[#links + 1] = href
end
print("links: " .. #links)
for i = 1, math.min(#links, 10) do
    print("  " .. links[i])
end
if #links > 10 then
    print(("  ... %d more"):format(#links - 10))
end
