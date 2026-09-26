-- CLI test fixture: a script that never ends on its own.
--
-- The marker goes to stderr (unbuffered even into a pipe) only once
-- kernel.exec has installed the count hook, so a parent test that
-- waits for it knows its SIGINT cannot arrive before luna's handler
-- exists — nor before the hook that turns the flag into an
-- "interrupted" error.
io.stderr:write("spin\n")
while true do end
