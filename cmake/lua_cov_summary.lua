-- Echo luacov's per-file summary table to stdout (the reporter itself
-- only writes it into the report file, and a build-log-visible total is
-- the point of the lua_coverage target). Runs after the reporter.
local report = arg and arg[1] or "luacov.report.out"
local printing, rows = false, false
for line in io.lines(report) do
    if line:find("^File%s+Hits%s+Missed%s+Coverage") then
        printing = true
    elseif printing and line:find("^%-+$") then
        if rows then
            printing = false -- trailing rule: only the Total row follows
        end
    elseif line:find("^Total%s") then
        io.write(line, "\n")
        break
    end
    if printing then
        io.write(line, "\n")
        rows = rows or line:sub(1, 1) == "/"
    end
end
