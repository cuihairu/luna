-- http: HTTP client.
--
-- Backed by luasocket's socket.http (request(), request_to(), generic
-- form POST). HTTPS needs ssl.https from LuaSec, which luna does not
-- bundle; plain http:// is what this module serves.
return require "socket.http"
