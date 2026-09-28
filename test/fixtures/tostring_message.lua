-- An error object whose __tostring SUCCEEDS and returns a string: the
-- message handler takes its metamethod early path — the string becomes
-- the message verbatim and NO traceback is attached (the fallback path
-- for hostile/absent tostring does attach one; see non_string_error.lua).
error(setmetatable({}, {__tostring = function() return "tbl: via metamethod" end}))
