-- plugin extension points: a REPL command and an injected module
require("luna.magic").register("plug", function(session, arg)
    kernel.write("plug ran: " .. (arg == "" and "no args" or arg) .. "\n")
end, "added by the good plugin")
return { modules = { plugmod = { n = 42 } } }
