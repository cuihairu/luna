-- error(false) inside a __tostring: the message handler runs while the
-- stack is still live, its callmeta raise is absorbed on this path, and
-- its own fallback names the object instead of letting garbage through.
-- Script mode is the one kernel.exec caller; the runner must survive a
-- hostile error object and report it, never crash.
error(setmetatable({}, {__tostring = function() error(false) end}))
