/* luna_loop.c — an opt-in libuv event loop, exposed to Lua as "loop".
 *
 * First slice of the event-loop batch, per the architecture note:
 * scripts require "loop" explicitly and drive uv themselves — the REPL
 * and the sync stdlib are untouched. Timers and immediates carry
 * {fn, ...args} in a registry table; callbacks run under pcall and a
 * raising callback prints to stderr instead of taking the loop down.
 *
 * Keep-alive contract: every scheduled callback holds its handle in
 * the registry until the handle is fully closed (uv_close is async, so
 * the userdata must outlive the close callback). While any handle is
 * alive, a prepare hook polls the attach socket and checks for a
 * pending ^C; when the last handle closes the hook stops and
 * loop.run('default') falls through — Node's "empty loop exits".
 * A pending ^C stops the run and surfaces as an "interrupted" error,
 * which the entry chunk maps to exit code 130 like a busy script. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <uv.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luna_kernel.h"
#include "luna_loop.h"

enum { LBOX_TIMER = 1, LBOX_IMMEDIATE = 2 };

struct loopbox {
    union {
        uv_timer_t timer;   /* first member: same address as the union */
        uv_check_t check;
    } h;
    int kind;
    int repeating;          /* timers only */
    int closed;             /* cleared handles reject a second close */
    lua_State *L;           /* single-threaded loop: state is fixed */
    int selfref;            /* registry -> userdata, dropped on close */
    int fref;               /* registry -> {fn, ...args} */
};

static uv_loop_t g_loop;
static int g_loop_ready;
static int g_user_handles;
static uv_prepare_t g_prep;
static int g_prep_inited;
static int g_interrupted;   /* ^C landed mid-run: stop, then raise */
static lua_State *g_L;      /* the state the serve poll runs in */

/* box from its handle (the union is the first member) */
static struct loopbox *box_of(void *handle)
{
    return (struct loopbox *)((char *)handle - offsetof(struct loopbox, h));
}

/* A callback that raises must not take the loop down: report on stderr
 * and keep going, the way the REPL isolates a chunk error. */
static void box_call(struct loopbox *box)
{
    lua_State *L = box->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, box->fref); /* {fn, ...args} */
    int tbl = lua_gettop(L); /* fixed index: -1 moves as values land */
    int nargs = (int)lua_rawlen(L, tbl) - 1; /* table minus the function */
    for (int i = 1; i <= nargs + 1; i++) {
        lua_rawgeti(L, tbl, i);
    }
    lua_remove(L, tbl); /* drop the args table */
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

/* Every handle is unref'd exactly here, after uv_close completed: the
 * registry reference is what keeps the userdata alive until then. When
 * the last handle goes, the prepare hook stops with it and an idle
 * uv_run('default') returns. */
static void on_closed(uv_handle_t *handle)
{
    struct loopbox *box = box_of(handle);
    g_user_handles--;
    if (g_user_handles == 0 && g_prep_inited) {
        uv_prepare_stop(&g_prep);
    }
    luaL_unref(box->L, LUA_REGISTRYINDEX, box->selfref);
}

static void box_close(struct loopbox *box)
{
    if (box->closed) {
        return;
    }
    box->closed = 1;
    uv_close((uv_handle_t *)&box->h.timer, on_closed);
}

/* One-shot timers and all immediates close themselves after firing;
 * repeating intervals run until loop.clear* — like Node. */
static void on_timer(uv_timer_t *t)
{
    struct loopbox *box = box_of(t);
    box_call(box);
    if (!box->repeating) {
        box_close(box);
    }
}

static void on_check(uv_check_t *c)
{
    struct loopbox *box = box_of(c);
    box_call(box);
    box_close(box);
}

/* The keep-alive hook: runs once per loop iteration while handles are
 * alive. Two jobs — translate a ^C that landed while uv_run was
 * blocking into "stop, then interrupt", and give the attach socket its
 * poll (serve.step, the same entry the REPL idle gap uses). */
static void on_prepare(uv_prepare_t *p)
{
    (void)p;
    if (luna_kernel_take_interrupt()) {
        uv_stop(&g_loop);
        g_interrupted = 1;
        return;
    }
    if (g_L) {
        lua_getglobal(g_L, "__LUNA_SERVE_STEP");
        if (lua_isfunction(g_L, -1)) {
            if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
                lua_pop(g_L, 1); /* a broken attach poll never stops time */
            }
        } else {
            lua_pop(g_L, 1);
        }
    }
}

/* Allocate the userdata, keep it alive in the registry, start the
 * prepare hook: the loop is keep-alive for exactly the live handles. */
static struct loopbox *box_new(lua_State *L, int kind)
{
    struct loopbox *box = lua_newuserdata(L, sizeof(*box));
    memset(box, 0, sizeof(*box));
    box->kind = kind;
    box->L = L;
    luaL_getmetatable(L, "loop.handle");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    box->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (!g_prep_inited) {
        uv_prepare_init(&g_loop, &g_prep);
        g_prep_inited = 1;
    }
    if (++g_user_handles > 0) {
        uv_prepare_start(&g_prep, on_prepare);
    }
    return box;
}

/* Collect fn + trailing args into a registry table (the callback and
 * what the call will pass). Stack: [.., box] in, [.., box] out; the box
 * sits on top and never joins the args. fn_idx is where the function
 * lives, arg_idx the first trailing argument (right after the timeout's
 * ms, right after the immediate's fn). */
static void box_ref_args(lua_State *L, struct loopbox *box, int fn_idx,
                         int arg_idx)
{
    int top = lua_gettop(L) - 1; /* exclude the box */
    lua_createtable(L, top - arg_idx + 2, 0);
    lua_pushvalue(L, fn_idx);
    lua_rawseti(L, -2, 1);
    for (int i = arg_idx; i <= top; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i - arg_idx + 2);
    }
    box->fref = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* loop.setTimeout(fn, ms?, ...) -> handle */
static int l_set_timeout(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_Integer ms = luaL_optinteger(L, 2, 0);
    luaL_argcheck(L, ms >= 0, 2, "timeout must be >= 0");
    struct loopbox *box = box_new(L, LBOX_TIMER);
    box_ref_args(L, box, 1, 3);
    uv_timer_init(&g_loop, &box->h.timer);
    uv_timer_start(&box->h.timer, on_timer, (uint64_t)ms, 0);
    return 1;
}

/* loop.setInterval(fn, ms?, ...) -> handle */
static int l_set_interval(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_Integer ms = luaL_optinteger(L, 2, 1);
    luaL_argcheck(L, ms >= 1, 2, "interval must be >= 1");
    struct loopbox *box = box_new(L, LBOX_TIMER);
    box->repeating = 1;
    box_ref_args(L, box, 1, 3);
    uv_timer_init(&g_loop, &box->h.timer);
    uv_timer_start(&box->h.timer, on_timer, (uint64_t)ms, (uint64_t)ms);
    return 1;
}

/* loop.setImmediate(fn, ...) -> handle: runs at the check phase of the
 * next loop iteration (before the run ends, after pending I/O). */
static int l_set_immediate(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    struct loopbox *box = box_new(L, LBOX_IMMEDIATE);
    box_ref_args(L, box, 1, 2);
    uv_check_init(&g_loop, &box->h.check);
    uv_check_start(&box->h.check, on_check);
    return 1;
}

/* clearTimeout / clearInterval / clearImmediate share one body: closing
 * is idempotent, so clearing an already-fired one-shot is a no-op. */
static int l_clear(lua_State *L)
{
    struct loopbox *box =
        luaL_checkudata(L, 1, "loop.handle");
    box_close(box);
    return 0;
}

/* loop.run(mode?) -> true | error("interrupted")
 *   "default": until nothing is scheduled (or stop/interrupt)
 *   "once":    one iteration, blocking until something is ready
 *   "nowait":  one iteration, no blocking */
static int l_run(lua_State *L)
{
    static const struct { const char *name; uv_run_mode mode; } modes[] = {
        { "default", UV_RUN_DEFAULT },
        { "once",    UV_RUN_ONCE },
        { "nowait",  UV_RUN_NOWAIT },
    };
    const char *name = luaL_optstring(L, 1, "default");
    unsigned m;
    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        if (strcmp(name, modes[m].name) == 0) {
            break;
        }
    }
    if (m == sizeof(modes) / sizeof(modes[0])) {
        return luaL_error(L, "bad run mode '%s' (default|once|nowait)", name);
    }
    g_L = L; /* the attach poll runs in the state that drives the loop */
    g_interrupted = 0;
    uv_run(&g_loop, modes[m].mode);
    if (g_interrupted) {
        return luaL_error(L, "interrupted");
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State *L)
{
    (void)L;
    uv_stop(&g_loop);
    return 0;
}

/* loop.now() -> loop milliseconds (uv_now; timeouts are relative to it) */
static int l_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)uv_now(&g_loop));
    return 1;
}

static int handle_tostring(lua_State *L)
{
    struct loopbox *box = luaL_checkudata(L, 1, "loop.handle");
    const char *kind = box->kind == LBOX_IMMEDIATE ? "immediate" : "timer";
    lua_pushfstring(L, "loop.%s: %p", kind, (void *)box);
    return 1;
}

static const luaL_Reg loop_funcs[] = {
    { "setTimeout", l_set_timeout },
    { "setInterval", l_set_interval },
    { "setImmediate", l_set_immediate },
    { "clearTimeout", l_clear },
    { "clearInterval", l_clear },
    { "clearImmediate", l_clear },
    { "run", l_run },
    { "stop", l_stop },
    { "now", l_now },
    { NULL, NULL },
};

int luaopen_luna_loop(lua_State *L)
{
    if (!g_loop_ready) {
        if (uv_loop_init(&g_loop) != 0) {
            return luaL_error(L, "loop: cannot init uv loop");
        }
        g_loop_ready = 1;
    }
    if (luaL_newmetatable(L, "loop.handle")) {
        lua_pushcfunction(L, handle_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    luaL_newlib(L, loop_funcs);
    return 1;
}
