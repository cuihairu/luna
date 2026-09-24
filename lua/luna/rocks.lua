-- luna.rocks: `luna install/search/list/update` — a thin wrapper over
-- the vendored LuaRocks sources (deps/luarocks), run in-process in
-- luna's own VM. No system luarocks needed.
--
-- Layout: a project tree keeps rocks under <root>/.luna/rocks (used as
-- a LuaRocks --tree; found by walking up from the working directory).
-- After every install the tree is snapshotted into <root>/luna.lock:
-- for each rock its exact version plus the sha256 of its downloaded
-- .src.rock, which is kept under .luna/cache. `luna install
-- --from-lock` reproduces the tree from that file alone — offline
-- first (the cache), online fallback (name+version, sha verified).
--
-- LuaRocks' command layer exits the process on both success and
-- failure (os.exit). That is fine for a one-shot CLI command but not
-- for the post-install lock step: around each embedded call os.exit
-- is swapped for a table-carrying error, so we regain control with
-- luarocks' own status code.
--
-- Note on interactivity: luarocks prompts before installing missing
-- dependencies; with stdin not a TTY it proceeds automatically. The
-- rocks commands are one-shot CLI invocations, so that is the expected
-- mode here.

local rocks = {}

local lfs = require "lfs"

local ok_json, json = pcall(require, "dkjson")
if not ok_json then
    error("luna.rocks needs dkjson (bundled)", 0)
end

local LUA_VERSION = _VERSION:match("%d+%.%d+")  -- "5.5"
local LOCK_VERSION = 1
local CACHE_DIR = ".luna/cache"

-- ----------------------------------------------------------------
-- small fs / digest helpers
-- ----------------------------------------------------------------

local function parent(dir)
    local up = dir:match("^(.*)/[^/]+")
    if up == nil or up == dir then
        return nil
    end
    return up
end

local function is_dir(path)
    return lfs.attributes(path, "mode") == "directory"
end

local function is_file(path)
    return lfs.attributes(path, "mode") == "file"
end

local function ensure_dir(path)
    if is_dir(path) then
        return true
    end
    local base = parent(path)
    if base and not is_dir(base) then
        ensure_dir(base)
    end
    lfs.mkdir(path)
    return is_dir(path)
end

local function tohex(s)
    return (s:gsub(".", function(c)
        return string.format("%02x", c:byte())
    end))
end

local function sha256_file(path)
    local ok_digest, digest = pcall(require, "openssl.digest")
    if not ok_digest then
        return nil, "sha256 needs the crypto backend (openssl.digest)"
    end
    local fh, open_err = io.open(path, "rb")
    if not fh then
        return nil, tostring(open_err)
    end
    local ctx = digest.new("sha256")
    while true do
        local chunk = fh:read(65536)
        if not chunk then
            break
        end
        ctx:update(chunk)
    end
    fh:close()
    return tohex(ctx:final())
end

-- ----------------------------------------------------------------
-- project tree / lock file locations
-- ----------------------------------------------------------------

-- Walk up from the working directory looking for an existing
-- .luna/rocks directory. With `create`, fall back to one next to the
-- working directory so a fresh project has a tree to fill.
local function find_tree(create)
    local dir = lfs.currentdir()
    while dir do
        local candidate = dir .. "/.luna/rocks"
        if is_dir(candidate) then
            return candidate
        end
        dir = parent(dir)
    end
    if not create then
        return nil
    end
    local cwd = lfs.currentdir()
    return cwd .. "/.luna/rocks"
end

-- luna.lock sits next to .luna/ (the project root), like package.json.
local function lock_path_for(tree)
    local root = parent(parent(tree))
    return root and (root .. "/luna.lock") or "luna.lock"
end

-- ----------------------------------------------------------------
-- the vendored LuaRocks
-- ----------------------------------------------------------------

local function vendor_dir()
    local ok_kernel, kernel = pcall(require, "kernel")
    if ok_kernel and type(kernel.vendor_dir) == "string" then
        return kernel.vendor_dir
    end
    local env = os.getenv("LUNA_VENDOR_DIR")
    return env and (env ~= "" and env or nil) or nil
end

local warned_vendor = false

-- Run one luarocks command in-process; returns its (intercepted) exit
-- status. Dies (os.exit 1) when the vendored sources are missing.
local function run_luarocks(argv)
    local vendor = vendor_dir()
    if not vendor or not is_file(vendor .. "/luarocks/cmd.lua") then
        io.stderr:write("luna: vendored LuaRocks sources not found (" ..
            tostring(vendor) .. ") — build the tree or set LUNA_VENDOR_DIR\n")
        os.exit(1)
    end
    package.path = vendor .. "/?.lua;" .. package.path

    -- luarocks bare-requires "argparse" expecting its own fork (it
    -- adds add_help_command & co); luna's argparse holds that name.
    -- The swap must happen BEFORE any luarocks module loads: they
    -- capture the value into locals at load time. Restored after the
    -- run.
    local luna_argparse = package.loaded["argparse"]
    package.loaded["argparse"] = require("luarocks.vendor.argparse")

    if not warned_vendor then
        warned_vendor = true
        local cfg = require("luarocks.core.cfg")
        cfg.init()
    end
    local cmd = require("luarocks.cmd")

    local real_exit = os.exit
    os.exit = function(code)
        error({ luna_rocks_exit = code or 0 }, 0)
    end
    local ok, err = pcall(cmd.run_command, "luna package management", {
        install = "luarocks.cmd.install",
        search = "luarocks.cmd.search",
        list = "luarocks.cmd.list",
        download = "luarocks.cmd.download",
    }, "luarocks.cmd.external", table.unpack(argv, 1, #argv))
    os.exit = real_exit
    package.loaded["argparse"] = luna_argparse

    if not ok then
        if type(err) == "table" and err.luna_rocks_exit then
            return err.luna_rocks_exit
        end
        error(err, 0) -- a genuine bug, not luarocks' exit protocol
    end
    return 0
end

-- luarocks global flags every call needs: pin the Lua version so cfg
-- never has to guess, and pin the project tree.
local function run(tree, argv)
    return run_luarocks({ "--lua-version", LUA_VERSION, "--tree", tree,
                          table.unpack(argv, 1, #argv) })
end

-- run + abort the command on a nonzero luarocks status, so a failed
-- install never reaches the lock step
local function run_or_die(tree, argv, what)
    local rc = run(tree, argv)
    if rc ~= 0 then
        io.stderr:write("luna: " .. what .. " failed (luarocks exit " .. rc .. ")\n")
        os.exit(rc)
    end
    return rc
end

-- ----------------------------------------------------------------
-- tree snapshot -> luna.lock
-- ----------------------------------------------------------------

-- All rocks installed in the tree: {{name=..., version=...}, ...}.
local function tree_rocks(tree)
    local base = tree .. "/lib/luarocks/rocks-" .. LUA_VERSION
    local out = {}
    if not is_dir(base) then
        return out
    end
    for name in lfs.dir(base) do
        if name:sub(1, 1) ~= "." and is_dir(base .. "/" .. name) then
            for version in lfs.dir(base .. "/" .. name) do
                if version:sub(1, 1) ~= "." and is_dir(base .. "/" .. name .. "/" .. version) then
                    out[#out + 1] = { name = name, version = version }
                end
            end
        end
    end
    table.sort(out, function(a, b)
        if a.name == b.name then
            return a.version < b.version
        end
        return a.name < b.name
    end)
    return out
end

local function cache_file(tree, name, version)
    local root = parent(parent(tree))
    return (root or ".") .. "/" .. CACHE_DIR .. "/" .. name .. "-" .. version .. ".src.rock"
end

-- Fetch a rock's source file into .luna/cache (for offline reproduce)
-- and return its sha256. The canonical .src.rock URL is fetched
-- directly: luarocks' `download` command filters its manifest query by
-- Lua version and the 5.5 index on the server is still sparse, while
-- the file itself is version-independent. Not every rock ships a
-- .src.rock (git-sourced rocks don't) — those lock by rockspec sha
-- only, which still pins the exact source URL + version.
-- os.execute returns true/nil (+ "exit"/code) since Lua 5.2 — compare
-- booleans, not the 5.1-era numeric status
local function shell_ok(cmd)
    return os.execute(cmd) == true
end

local function fetch_src_rock(name, version, target)
    local url = "https://luarocks.org/" .. name .. "-" .. version .. ".src.rock"
    for _, fetcher in ipairs({
        { "curl", "-fsSL", "--max-time", "120", "-o", target, url },
        { "wget", "-q", "-T", "120", "-O", target, url },
    }) do
        local bin = fetcher[1]
        if shell_ok("command -v " .. bin .. " >/dev/null 2>&1") then
            local quoted = {}
            for i, part in ipairs(fetcher) do
                quoted[i] = "'" .. tostring(part):gsub("'", "'\\''") .. "'"
            end
            if shell_ok(table.concat(quoted, " ") .. " >/dev/null 2>&1")
                and is_file(target) then
                return true
            end
        end
    end
    return false
end

-- sha of the rockspec luarocks stored in the tree: the authoritative
-- lockable fingerprint of what was installed (source URL + version)
local function rockspec_sha(tree, name, version)
    return sha256_file(tree .. "/lib/luarocks/rocks-" .. LUA_VERSION ..
        "/" .. name .. "/" .. version .. "/" .. name .. "-" .. version .. ".rockspec")
end

local function cache_and_hash(tree, name, version)
    local target = cache_file(tree, name, version)
    if is_file(target) then
        local hex = sha256_file(target)
        if hex then
            return hex
        end
    end
    ensure_dir(parent(target))
    if not fetch_src_rock(name, version, target) then
        return nil -- no packaged source: rockspec sha still locks it
    end
    local hex, hash_err = sha256_file(target)
    if not hex then
        io.stderr:write("luna: warning: cannot hash " .. target .. ": " .. tostring(hash_err) .. "\n")
        return nil
    end
    return hex
end

local function write_lock(path, entries)
    local text = json.encode({
        version = LOCK_VERSION,
        lua = LUA_VERSION,
        rocks = entries,
    }, { indent = true })
    local fh = assert(io.open(path, "w"))
    fh:write(text, "\n")
    fh:close()
end

local function read_lock(path)
    local fh = io.open(path, "r")
    if not fh then
        io.stderr:write("luna: no luna.lock here — run `luna install <rock>` first\n")
        os.exit(1)
    end
    local text = fh:read("a")
    fh:close()
    local lock, pos, jerr = json.decode(text)
    if not lock then
        io.stderr:write("luna: luna.lock is not valid JSON at position " ..
            tostring(pos) .. ": " .. tostring(jerr) .. "\n")
        os.exit(1)
    end
    return lock
end

-- Snapshot the whole tree into luna.lock (all rocks: the dependency
-- closure came along with what was installed).
local function lock_tree(tree)
    local installed = tree_rocks(tree)
    local entries = {}
    for _, rock in ipairs(installed) do
        local rsha, rsha_err = rockspec_sha(tree, rock.name, rock.version)
        if not rsha then
            io.stderr:write("luna: warning: cannot hash rockspec of " ..
                rock.name .. " " .. rock.version .. ": " .. tostring(rsha_err) .. "\n")
        end
        entries[#entries + 1] = {
            name = rock.name,
            version = rock.version,
            rockspec = rsha,
            sha256 = cache_and_hash(tree, rock.name, rock.version),
        }
    end
    local path = lock_path_for(tree)
    write_lock(path, entries)
    io.write(string.format("luna: locked %d rock%s -> %s\n",
        #entries, #entries == 1 and "" or "s", path))
end

-- ----------------------------------------------------------------
-- the four commands
-- ----------------------------------------------------------------

local function cmd_install(tree, argv)
    local from_lock = false
    local names = {}
    for _, a in ipairs(argv) do
        if a == "--from-lock" then
            from_lock = true
        else
            names[#names + 1] = a
        end
    end

    if from_lock then
        local lock = read_lock(lock_path_for(tree))
        local rocks_in_lock = lock.rocks or {}
        for _, rock in ipairs(rocks_in_lock) do
            local cached = cache_file(tree, rock.name, rock.version)
            local via = { "install" }
            if rock.sha256 and is_file(cached) then
                local hex = sha256_file(cached)
                if hex ~= rock.sha256 then
                    io.stderr:write("luna: sha256 mismatch for cached " ..
                        rock.name .. " " .. rock.version .. " — refusing\n")
                    os.exit(1)
                end
                via[#via + 1] = cached -- offline: install the cached source
            else
                if rock.sha256 then
                    io.write("luna: no cached source for " .. rock.name ..
                        " " .. rock.version .. ", fetching\n")
                end
                via[#via + 1] = rock.name
                via[#via + 1] = rock.version
            end
            run_or_die(tree, via, "install " .. rock.name)
            -- the authoritative check: the rockspec now in the tree
            -- must be byte-identical to the locked one
            if rock.rockspec then
                local now = rockspec_sha(tree, rock.name, rock.version)
                if now ~= rock.rockspec then
                    io.stderr:write("luna: rockspec sha mismatch for " ..
                        rock.name .. " " .. rock.version .. " — refusing\n")
                    os.exit(1)
                end
            end
            io.write("luna: installed " .. rock.name .. " " .. rock.version .. "\n")
        end
        io.write(string.format("luna: lock reproduced (%d rocks)\n", #rocks_in_lock))
        return 0
    end

    if #names == 0 then
        io.stderr:write("luna: usage: luna install <rock> ... [--from-lock]\n")
        os.exit(1)
    end
    -- one luarocks call per rock: `luarocks install` takes a single
    -- <name> [<version>]; extra arguments would read as version numbers
    for _, name in ipairs(names) do
        run_or_die(tree, { "install", name }, "install " .. name)
    end
    lock_tree(tree)
    return 0
end

local function cmd_update(tree, argv)
    local lock = read_lock(lock_path_for(tree))
    local names = #argv > 0 and argv or {}
    if #names == 0 then
        for _, rock in ipairs(lock.rocks or {}) do
            names[#names + 1] = rock.name
        end
    end
    for _, name in ipairs(names) do
        run_or_die(tree, { "install", name }, "update " .. name) -- latest
        io.write("luna: updated " .. name .. "\n")
    end
    lock_tree(tree)
    return 0
end

-- ----------------------------------------------------------------
-- dispatch + runtime path injection
-- ----------------------------------------------------------------

-- luna install|search|list|update <args...>; argv is _G.arg (argv[1]
-- is the subcommand). luarocks' own output (and intercepted exit
-- code) is the user-visible result.
function rocks.dispatch(argv)
    local cmd = argv[1] or ""
    local rest = {}
    for i = 2, #argv do
        rest[#rest + 1] = argv[i]
    end

    if cmd == "install" then
        return cmd_install(find_tree(true), rest)
    elseif cmd == "update" then
        return cmd_update(find_tree(true), rest)
    elseif cmd == "search" or cmd == "list" then
        -- search works without a tree (queries the manifest server);
        -- list shows the given tree, defaulting to one next to the cwd
        local tree = find_tree(true)
        return run(tree, { cmd, table.unpack(rest, 1, #rest) })
    end
    io.stderr:write("luna: unknown rocks command '" .. cmd ..
        "' (install/search/list/update)\n")
    return 1
end

-- Point package.path/cpath at any project rocks tree so `require`
-- finds installed rocks. Read-only: called at startup for every mode.
function rocks.inject_paths()
    local tree = find_tree(false)
    if not tree then
        return
    end
    local lv = LUA_VERSION
    package.path = tree .. "/share/lua/" .. lv .. "/?.lua;" ..
        tree .. "/share/lua/" .. lv .. "/?/init.lua;" .. package.path
    -- the native default element on this platform ends in .so; keep
    -- the same convention for the tree
    package.cpath = tree .. "/lib/lua/" .. lv .. "/?.so;" .. package.cpath
end

return rocks
