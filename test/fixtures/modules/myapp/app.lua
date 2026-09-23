-- exercises bare luna_modules lookup + manifest + relative require
local dep1 = require "dep1"
local dep2 = require "dep2"
local util = require "./lib/util"
return "app:" .. dep1.name .. ":" .. dep2.name .. ":" .. util.name
