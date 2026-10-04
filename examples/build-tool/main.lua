-- build-tool — a staged data build: data/*.csv -> dist/*.json.
--
-- Each stage is a plain function in a table; the runner gives every
-- stage a wall time and stops at the first failure. Edit the stages
-- freely — the shape (declare, run, time, report) is the point.
--
--   luna main.lua             # clean, compile, report
--   luna main.lua compile     # run one stage only
local fs = require "fs"
local csv = require "csv"
local json = require "json"

local stages = {}

stages.clean = function()
    if fs.exists("dist") then
        for _, f in ipairs(fs.readdirSync("dist")) do
            assert(os.remove("dist/" .. f))
        end
        assert(fs.rmdir("dist"))
    end
    fs.mkdirSync("dist")
end

stages.compile = function()
    for _, f in ipairs(fs.readdirSync("data")) do
        if f:match("%.csv$") then
            local rows = csv.decode(fs.readFileSync("data/" .. f))
            assert(rows, "csv decode failed: " .. f)
            local out = f:gsub("%.csv$", ".json")
            fs.writeFileSync("dist/" .. out, json.encode(rows) .. "\n")
        end
    end
end

stages.report = function()
    for _, f in ipairs(fs.readdirSync("dist")) do
        local size = fs.attributes("dist/" .. f, "size")
        print(string.format("  %-16s %d bytes", f, size))
    end
end

local only = ({ ... })[1]
local order = { "clean", "compile", "report" }
for _, name in ipairs(order) do
    if not only or only == name then
        local t0 = os.clock()
        stages[name]()
        print(string.format("%-8s ok  %.3fs", name, os.clock() - t0))
    end
end
