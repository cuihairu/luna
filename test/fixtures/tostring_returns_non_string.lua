-- An error object whose __tostring succeeds but returns a NUMBER: the
-- message handler's metamethod path only accepts a string result, so a
-- non-string one falls through to the same fallback that names hostile
-- objects — and unlike the metamethod-string path, a traceback renders.
error(setmetatable({}, {__tostring = function() return 42 end}))
