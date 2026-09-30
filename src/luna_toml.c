/* toml.core — Lua face over tomlc17 (deps/tomlc17, pinned R260821).
 *
 * decode(s) -> table | nil, err: tomlc17 parses, this file converts the
 * datum tree into plain Lua values, then the result is freed — nothing
 * C-owned escapes the call:
 *   string -> string, int64 -> integer, fp64 -> number (inf/nan kept),
 *   boolean -> boolean, array -> 1-based array table, table -> map.
 *   local date / time / datetime / offset datetime -> a table with the
 *     components that were present: {year, month, day} for a date,
 *     {hour, minute, second, secfrac?} for a time, both for datetimes,
 *     plus offset (minutes from UTC, 0 for Z) on offset datetimes.
 *     secfrac is a fraction of a second and only appears when the
 *     source had sub-second digits (tomlc17 stores whole microseconds,
 *     so "07:32:00.0" and "07:32:00" decode identically).
 *
 * Parse errors normalize to the shared "toml: <reason> at line N"
 * shape — without the column clause: tomlc17 reports line numbers only
 * (datums carry columns, its error strings do not). tomlc17 has no
 * encoder; encode is the wrapper's own Lua face (lua/modules/toml/),
 * which also imposes the scalar-first key ordering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "tomlc17.h"

/* bracket/brace nesting is capped at 30 by tomlc17, but chained table
 * headers ([a.b] under [a] under ...) are not; without a guard the
 * recursive conversion could exhaust the C stack on hostile input */
#define MAX_CONVERT_DEPTH 100

static int convert_datum(lua_State *L, const toml_datum_t *d, int depth)
{
    if (depth > MAX_CONVERT_DEPTH)
        return -1;
    switch (d->type) {
    case TOML_STRING:
        lua_pushlstring(L, d->u.str.ptr, (size_t)d->u.str.len);
        return 0;
    case TOML_INT64:
        lua_pushinteger(L, (lua_Integer)d->u.int64);
        return 0;
    case TOML_FP64:
        lua_pushnumber(L, d->u.fp64);
        return 0;
    case TOML_BOOLEAN:
        lua_pushboolean(L, d->u.boolean ? 1 : 0);
        return 0;
    case TOML_ARRAY:
        lua_createtable(L, d->u.arr.size, 0);
        for (int i = 0; i < d->u.arr.size; i++) {
            if (convert_datum(L, &d->u.arr.elem[i], depth + 1) != 0) {
                lua_pop(L, 1); /* the half-built array */
                return -1;
            }
            lua_rawseti(L, -2, i + 1);
        }
        return 0;
    case TOML_TABLE:
        lua_createtable(L, 0, d->u.tab.size);
        for (int i = 0; i < d->u.tab.size; i++) {
            lua_pushlstring(L, d->u.tab.key[i], (size_t)d->u.tab.len[i]);
            if (convert_datum(L, &d->u.tab.value[i], depth + 1) != 0) {
                lua_pop(L, 2); /* key and the half-built table */
                return -1;
            }
            lua_rawset(L, -3);
        }
        return 0;
    case TOML_DATE:
    case TOML_TIME:
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        lua_createtable(L, 0, 8);
        if (d->u.ts.year >= 0) {
            lua_pushinteger(L, d->u.ts.year);
            lua_setfield(L, -2, "year");
            lua_pushinteger(L, d->u.ts.month);
            lua_setfield(L, -2, "month");
            lua_pushinteger(L, d->u.ts.day);
            lua_setfield(L, -2, "day");
        }
        if (d->u.ts.hour >= 0) {
            lua_pushinteger(L, d->u.ts.hour);
            lua_setfield(L, -2, "hour");
            lua_pushinteger(L, d->u.ts.minute);
            lua_setfield(L, -2, "minute");
            lua_pushinteger(L, d->u.ts.second);
            lua_setfield(L, -2, "second");
        }
        if (d->u.ts.usec > 0) {
            lua_pushnumber(L, (lua_Number)d->u.ts.usec / 1e6);
            lua_setfield(L, -2, "secfrac");
        }
        if (d->type == TOML_DATETIMETZ) {
            lua_pushinteger(L, d->u.ts.tz);
            lua_setfield(L, -2, "offset");
        }
        return 0;
    default:
        return -1; /* TOML_UNKNOWN: unreachable on a parsed document */
    }
}

/* tomlc17's SETERROR writes "(line N) <reason>"; the UTF-8 check writes
 * "<reason> on line N"; the internal fallback writes "Error near line
 * N". Normalize all three to "toml: <reason> at line N". */
static void push_toml_error(lua_State *L, const char *msg)
{
    char reason[224];
    int line = 0;
    if (sscanf(msg, "(line %d) %200[^\n]", &line, reason) == 2) {
        lua_pushfstring(L, "toml: %s at line %d", reason, line);
        return;
    }
    const char *on = strstr(msg, " on line ");
    if (on) {
        snprintf(reason, sizeof(reason), "%.*s", (int)(on - msg), msg);
        lua_pushfstring(L, "toml: %s at line %d", reason, atoi(on + 9));
        return;
    }
    const char *near = strstr(msg, "near line ");
    if (near) {
        line = atoi(near + 10);
        lua_pushfstring(L, "toml: parse error at line %d", line);
        return;
    }
    lua_pushfstring(L, "toml: %s", msg);
}

static int t_decode(lua_State *L)
{
    size_t len;
    const char *src = luaL_checklstring(L, 1, &len);
    toml_result_t r = toml_parse(src, (int)len);
    if (!r.ok) {
        lua_pushnil(L);
        push_toml_error(L, r.errmsg);
        return 2;
    }
    int ok = convert_datum(L, &r.toptab, 0);
    toml_free(r); /* everything of value is already copied out */
    if (ok != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "toml: nesting too deep");
        return 2;
    }
    return 1;
}

static const luaL_Reg toml_core_funcs[] = {
    { "decode", t_decode },
    { NULL, NULL },
};

int luaopen_toml_core(lua_State *L)
{
    /* TOML 1.0 requires valid UTF-8; tomlc17 ships the check off (its
     * own suite feeds legacy bytes) but luna's decode contract says
     * bad UTF-8 is rejected, so switch it on. Process-global in
     * tomlc17 — fine here: one Lua state per luna process, and the
     * test harness stages this module the same way. */
    toml_option_t opt = toml_default_option();
    opt.check_utf8 = true;
    toml_set_option(opt);

    luaL_newlib(L, toml_core_funcs);
    return 1;
}
