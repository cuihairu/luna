#include <stdio.h>
#include "luna.h"

int main(int argc, char *argv[])
{
    lua_State *l = luaL_newstate();
    luaL_openlibs(l);
    luaL_loadfile(l, "script.lua");
    lua_pcall(l, 0, 0, 0);
    lua_close(l);
    fprintf(stdout, "hello luna");
}