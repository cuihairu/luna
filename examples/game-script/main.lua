-- game-script — game logic as data: rooms, items, and verbs are
-- plain tables, the engine is one dispatch loop. Single-process
-- scripting shape for a story/logic layer — no server concepts, no
-- entity framework, just tables and a loop. State round-trips
-- through json (save/load).
--
--   printf 'look\nnorth\ntake key\nsouth\nup\nlook\nsave\nquit\n' \
--       | luna main.lua
local json = require "json"

local SAVE_FILE = "luna-save.json"

local rooms = {
    hall = {
        desc = "a pillared hall. Dust, and one flickering lamp.",
        exits = { north = "study", up = "tower" },
        needs = { up = "key" }, -- the tower door is locked
    },
    study = {
        desc = "a cramped study. A desk, an open drawer.",
        exits = { south = "hall" },
        items = { key = "a small brass key" },
    },
    tower = {
        desc = "the tower top. Wind, and the whole valley below.",
        exits = { down = "hall" },
    },
}

local state = { room = "hall", inv = {} }

local function look()
    local r = rooms[state.room]
    print("You are in " .. state.room .. ": " .. r.desc)
    local left = {}
    for name, item in pairs(r.items or {}) do
        left[#left + 1] = name .. " (" .. item .. ")"
    end
    if #left > 0 then
        print("here: " .. table.concat(left, ", "))
    end
    if #state.inv > 0 then
        print("carrying: " .. table.concat(state.inv, ", "))
    end
end

local verbs = {}

verbs.look = look
verbs.l = look

function verbs.go(where)
    local r = rooms[state.room]
    if not where or not r.exits[where] then
        print("no way " .. tostring(where))
        return
    end
    local need = r.needs and r.needs[where]
    if need and not state.inv[need] then
        print("the way " .. where .. " is locked — you need the " .. need)
        return
    end
    state.room = r.exits[where]
    look()
end

function verbs.take(what)
    local items = rooms[state.room].items or {}
    if not what or not items[what] then
        print("nothing like that here")
        return
    end
    state.inv[#state.inv + 1] = what
    state.inv[what] = true
    items[what] = nil
    print("taken: " .. what)
end

function verbs.inv()
    if #state.inv == 0 then
        print("you carry nothing")
    else
        print("carrying: " .. table.concat(state.inv, ", "))
    end
end
verbs.inventory = verbs.inv

function verbs.save(path)
    if not path or path == "" then
        path = SAVE_FILE
    end
    local plain = { room = state.room, inv = {} }
    for i, name in ipairs(state.inv) do
        plain.inv[i] = name
    end
    local fh = assert(io.open(path, "w"))
    fh:write(json.encode(plain) .. "\n")
    fh:close()
    print("saved to " .. path)
end

function verbs.load(path)
    if not path or path == "" then
        path = SAVE_FILE
    end
    local fh = assert(io.open(path, "r"))
    local plain = json.decode(fh:read("a"))
    fh:close()
    state.room = plain.room
    state.inv = {}
    for _, name in ipairs(plain.inv or {}) do
        state.inv[#state.inv + 1] = name
        state.inv[name] = true
    end
    look()
    print("loaded from " .. (path or SAVE_FILE))
end

verbs.help = function()
    print("verbs: look (l) / go <dir> / <dir> / take <item> / inv / " ..
        "save [file] / load [file] / help / quit")
end

print("luna game-script — 'help' lists verbs, 'quit' leaves.")
look()
while true do
    io.write("> ")
    io.stdout:flush()
    local line = io.read("l")
    if not line then
        break
    end
    local verb, arg = line:match("^(%S+)%s*(.-)$")
    verb = verb and verb:lower()
    if verb == "quit" or verb == "q" then
        print("bye.")
        break
    elseif verb and verb ~= "" then
        local fn = verbs[verb]
        if not fn and rooms[state.room].exits[verb] then
            fn, arg = verbs.go, verb -- bare direction: "north" = "go north"
        end
        if fn then
            fn(arg)
        else
            print("unknown verb: " .. verb .. " ('help' lists verbs)")
        end
    end
end
