-- hello-cli — the shape of a real command-line tool.
--
-- argparse ships with luna (require "argparse"), so options, flags,
-- and --help come for free: run `luna main.lua -h` for the usage page.
local argparse = require "argparse"

local parser = argparse("hello", "greet someone from the command line")
parser:option("-n --name", "who to greet", "world")
parser:flag("-q --quiet", "skip the status line")
parser:argument("extra", "extra words to append"):args("*")

local opts = parser:parse({ ... })

if not opts.quiet then
    print("hello-cli: greeting issued")
end
local suffix = #opts.extra > 0 and ", " .. table.concat(opts.extra, " ") or ""
print("hello, " .. opts.name .. suffix .. "!")
