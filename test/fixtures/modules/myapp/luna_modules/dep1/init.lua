-- re-runs must not happen: package.loaded caches the result
local loads = _G.__DEP1_LOADS or 0
_G.__DEP1_LOADS = loads + 1
return { name = "dep1" }
