-- exercises in-package subpath resolution (dotted and slashed spellings,
-- init.lua subpaths, and literal-package precedence)
local helper = require("dep1.lib.helper")
local slashed = require("dep1/lib/helper")
local deep = require "dep4.lib.deep"
local literal = require "dep3.sub"
return "subpaths:" .. helper .. ":" .. slashed .. ":" .. deep .. ":" .. literal
