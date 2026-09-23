-- luna entry point: embedded in the binary, loaded by luna_main.c.
-- __LUNA_REPL_SRC holds the source of luna/luna/repl.lua.
local repl = assert(load(__LUNA_REPL_SRC, "=(luna/repl)"))()
return repl.run(arg)
