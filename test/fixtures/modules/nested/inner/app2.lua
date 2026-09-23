-- bare lookup walks up past inner/ to nested/luna_modules/
local dep1 = require "dep1"
return "up:" .. dep1.name
