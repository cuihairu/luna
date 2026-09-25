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
 * which the entry chunk maps to exit code 130 like a busy script.
 *
 * loop.net: stream sockets with the same err-first callbacks. Client
 * side: connect(host, port) resolves asynchronously (uv_getaddrinfo)
 * then dials TCP; connectPipe(path) dials a unix domain socket. Server
 * side: listen(host, port) / listenPipe(path) bind synchronously (a
 * refused bind throws) and hand each peer to a retained connection
 * callback as onConn(err, sock). A sock offers write/read/end/close;
 * read delivers cb(nil, chunk) per chunk and cb(nil, nil) at EOF; a
 * server offers port()/close(). Each open socket or listener holds the
 * loop alive until its uv_close lands, like Node.
 *
 * loop.process: the two shapes of Node's child_process, on one
 * contract. run(cmd, args, [opts], cb) is the aggregate sibling of
 * exec: no shell, stdout/stderr captured into memory, cb(nil, res)
 * delivered once the child exited AND both capture pipes reached EOF
 * (res = {status, signal?, stdout, stderr}). spawn(cmd, args, [opts],
 * onExit) hands out ordinary loop.net socks as stdio — stdin
 * writable, stdout/stderr readable — and its onExit mirrors Node's
 * 'exit': it fires on process exit regardless of the streams, which
 * keep flowing until closed (net's "must close" contract). opts takes
 * {cwd=path}; run/spawn return the proc handle at once (pid/kill/
 * stdin/stdout/stderr); spawn failures throw synchronously, like
 * listen(); a killed child reports signal = <number> with a
 * meaningless status, mirroring libuv. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <signal.h>

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

/* Wake sentinel for immediates. A check handle lives in the check-phase
 * queue and is not in the poll set: once any I/O watcher exists, the poll
 * phase blocks in epoll until an event or the next timer, and a bare
 * check cannot interrupt it — the immediate would only run when that
 * wait ends. One unref'd async (created on first use) exists solely to
 * kick the blocked poll: its eventfd sits in the poll set, uv_async_send
 * is coalescing, and an unref'd handle neither keeps run() alive nor
 * joins the keep-alive count. */
static uv_async_t g_immediate_kick;
static int g_kick_ready;

static void on_kick(uv_async_t *a)
{
    (void)a; /* the check phase reads the queue; the kick only wakes it */
}

static void immediate_kick(void)
{
    if (!g_kick_ready) {
        uv_async_init(&g_loop, &g_immediate_kick, on_kick);
        uv_unref((uv_handle_t *)&g_immediate_kick);
        g_kick_ready = 1;
    }
    uv_async_send(&g_immediate_kick);
}

/* handle:ref / handle:unref, shared by every face: an unref'd handle
 * runs as usual — timers fire, callbacks deliver — but no longer keeps
 * run() alive, and the loop drains once nothing ref'd remains. Purely
 * uv-layer bookkeeping: keep-alive counting and the close paths are
 * untouched, so ref/unref/close interleave in any order, and closed
 * handles no-op (libuv checks). Every handle struct here starts with
 * its uv handle — directly, or as the first union member — so one
 * (uv_handle_t *) cast serves all. The macro is expanded right above
 * each face's method table. */
#define LUNA_HANDLE_CTL(face, type, mt)                                  \
    static int l_##face##_unref(lua_State *L)                            \
    {                                                                    \
        type *p = luaL_checkudata(L, 1, mt);                             \
        uv_unref((uv_handle_t *)p);                                      \
        return 0;                                                        \
    }                                                                    \
    static int l_##face##_ref(lua_State *L)                              \
    {                                                                    \
        type *p = luaL_checkudata(L, 1, mt);                             \
        uv_ref((uv_handle_t *)p);                                        \
        return 0;                                                        \
    }

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
static void keepalive_open(void);
static void keepalive_close(void);

static void on_closed(uv_handle_t *handle)
{
    struct loopbox *box = box_of(handle);
    keepalive_close();
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
    keepalive_open();
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
    /* anchor the deadline at "now": loop time only refreshes inside
     * uv_run, so a timer scheduled before the first run — or between
     * runs — would otherwise count its ms from a stale moment and can
     * even fire immediately */
    uv_update_time(&g_loop);
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
    uv_update_time(&g_loop); /* same staleness as setTimeout */
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
    immediate_kick(); /* a blocked poll must not outwait the immediate */
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

static int l_signal(lua_State *L); /* defined with the sigwatch block */

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
    { "signal", l_signal },
    { NULL, NULL },
};

/* -- async file IO (loop.fs) ----------------------------------------------
 *
 * Node-shaped: readFile(path, cb(err, data)), writeFile(path, data,
 * cb(err)), appendFile, stat(path, cb(err, st)), plus the directory
 * face — readdir/mkdir/rmdir/unlink/rename, cb(err) (or cb(nil, names)
 * for readdir). The actual IO runs on libuv's threadpool; completion
 * callbacks are delivered on the loop thread, so Lua is only ever
 * touched here. Each operation pins its callback in the registry until
 * it finishes — and counts as a live user handle, so the loop stays
 * keep-alive while any IO is in flight. */

#include <fcntl.h>
#include <sys/stat.h>

enum { FS_READ, FS_WRITE, FS_STAT, FS_READDIR, FS_ONCE, FS_PATH };

struct fsop {
    uv_fs_t req;       /* first member: fsop == (struct fsop *)req */
    lua_State *L;
    int cbref;         /* registry -> the user callback */
    int kind;
    int errcode;       /* failure seen mid-chain, surfaced at the end */
    int fd;            /* -1 while closed */
    int openflags;     /* writeFile/appendFile open mode */
    char *data;        /* writeFile payload / readFile buffer */
    size_t size;
    size_t have;       /* readFile: bytes actually read */
};

/* keep-alive bookkeeping shared with timers/immediates */
static void keepalive_open(void)
{
    if (!g_prep_inited) {
        uv_prepare_init(&g_loop, &g_prep);
        g_prep_inited = 1;
    }
    if (++g_user_handles > 0) {
        uv_prepare_start(&g_prep, on_prepare);
    }
}

static void keepalive_close(void)
{
    g_user_handles--;
    if (g_user_handles == 0 && g_prep_inited) {
        uv_prepare_stop(&g_prep);
    }
}

/* queue an operation: pin the callback so it outlives this call */
static struct fsop *fsop_new(lua_State *L, int cb_idx, int kind)
{
    struct fsop *op = malloc(sizeof(*op));
    memset(op, 0, sizeof(*op));
    op->L = L;
    op->kind = kind;
    op->fd = -1;
    lua_pushvalue(L, cb_idx);
    op->cbref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(L, op);
    lua_pushvalue(L, cb_idx);
    lua_rawset(L, LUA_REGISTRYINDEX); /* registry[op] = cb (the pin) */
    keepalive_open();
    return op;
}

static void fsop_free(struct fsop *op)
{
    if (op->data) {
        free(op->data);
    }
    uv_fs_req_cleanup(&op->req);
    lua_pushlightuserdata(op->L, op);
    lua_pushnil(op->L);
    lua_rawset(op->L, LUA_REGISTRYINDEX); /* drop the pin */
    free(op);
}

/* Deliver cb(err, value) on the loop thread; a raising callback is
 * reported and dropped, like any other loop callback. */
static void fs_finish(struct fsop *op)
{
    lua_State *L = op->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, op->cbref);
    if (op->errcode != 0) {
        lua_pushstring(L, uv_strerror(op->errcode));
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        if (op->kind == FS_READ) {
            lua_pushlstring(L, op->data ? op->data : "", op->have);
        } else if (op->kind == FS_STAT) {
            lua_createtable(L, 0, 4);
            lua_pushinteger(L, (lua_Integer)op->req.statbuf.st_size);
            lua_setfield(L, -2, "size");
            lua_pushinteger(L, (lua_Integer)op->req.statbuf.st_mtime);
            lua_setfield(L, -2, "mtime");
            lua_pushinteger(L, (lua_Integer)op->req.statbuf.st_mode);
            lua_setfield(L, -2, "mode");
            /* "file"/"dir"/"link"/"other": link only surfaces from
             * lstat — stat follows symlinks by definition */
            const char *ty;
            mode_t m = op->req.statbuf.st_mode;
            if (S_ISREG(m)) {
                ty = "file";
            } else if (S_ISDIR(m)) {
                ty = "dir";
            } else if (S_ISLNK(m)) {
                ty = "link";
            } else {
                ty = "other";
            }
            lua_pushstring(L, ty);
            lua_setfield(L, -2, "type");
        } else if (op->kind == FS_PATH) {
            /* realpath: result lives in req.ptr until req_cleanup */
            lua_pushstring(L, (const char *)op->req.ptr);
        } else if (op->kind == FS_READDIR) {
            /* drain the scandir list: uv_fs_scandir_next walks it and
             * UV_EOF ends it; the strings are copied into Lua here,
             * uv_fs_req_cleanup (in fsop_free) frees the list */
            lua_createtable(L, 0, 8);
            uv_dirent_t ent;
            int i = 1;
            while (uv_fs_scandir_next(&op->req, &ent) != UV_EOF) {
                lua_pushstring(L, ent.name);
                lua_rawseti(L, -2, i++);
            }
        } else {
            lua_pushnil(L); /* writeFile: pad to two args */
        }
    }
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: fs callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
    keepalive_close();
    fsop_free(op);
}

static void fs_done(uv_fs_t *req);

/* A mid-chain failure closes the fd first (reusing the req), then the
 * close callback lands in fs_done which reports the saved errcode. */
static void fs_fail(struct fsop *op)
{
    op->errcode = (int)op->req.result;
    if (op->fd >= 0) {
        int fd = op->fd;
        op->fd = -1;
        uv_fs_req_cleanup(&op->req);
        uv_fs_close(&g_loop, &op->req, fd, fs_done);
        return;
    }
    fs_finish(op);
}

/* the chain's last stage: fd already closed (or never opened) */
static void fs_done(uv_fs_t *req)
{
    fs_finish((struct fsop *)req);
}

/* single-shot ops (mkdir/rmdir/unlink/rename): the request IS the op */
static void fs_once_done(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        op->errcode = (int)req->result;
    }
    fs_finish(op);
}

/* readdir: scandir already listed everything; the names are drained
 * inside fs_finish (FS_READDIR branch) */
static void fs_scandir_done(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        op->errcode = (int)req->result;
    }
    fs_finish(op);
}

/* truncate chains open(O_WRONLY) -> ftruncate -> close: libuv only
 * has the fd version (uv_fs_ftruncate); the wanted length rides in
 * op->size */
static void fs_trunc_trunc(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    int fd = op->fd;
    op->fd = -1;
    uv_fs_req_cleanup(req);
    uv_fs_close(&g_loop, req, fd, fs_done);
}

static void fs_trunc_open(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    op->fd = (int)req->result;
    uv_fs_req_cleanup(req);
    uv_fs_ftruncate(&g_loop, req, op->fd, (off_t)op->size, fs_trunc_trunc);
}

static void fs_read_read(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    op->have = (size_t)req->result;
    int fd = op->fd;
    op->fd = -1;
    uv_fs_req_cleanup(req);
    uv_fs_close(&g_loop, req, fd, fs_done);
}

static void fs_read_stat(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    off_t size = req->statbuf.st_size;
    op->size = (size_t)size;
    op->data = malloc(op->size ? op->size : 1);
    if (!op->data) {
        op->errcode = UV_ENOMEM;
        int fd = op->fd;
        op->fd = -1;
        uv_fs_req_cleanup(req);
        uv_fs_close(&g_loop, req, fd, fs_done);
        return;
    }
    uv_buf_t buf = uv_buf_init(op->data, (unsigned)op->size);
    uv_fs_req_cleanup(req);
    uv_fs_read(&g_loop, req, op->fd, &buf, 1, 0, fs_read_read);
}

static void fs_read_open(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    op->fd = (int)req->result;
    uv_fs_req_cleanup(req);
    uv_fs_fstat(&g_loop, req, op->fd, fs_read_stat);
}

static void fs_write_write(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    op->have += (size_t)req->result;
    if (op->have < op->size) { /* partial write: continue */
        uv_buf_t buf = uv_buf_init(op->data + op->have,
                                   (unsigned)(op->size - op->have));
        uv_fs_req_cleanup(req);
        uv_fs_write(&g_loop, req, op->fd, &buf, 1, -1, fs_write_write);
        return;
    }
    int fd = op->fd;
    op->fd = -1;
    uv_fs_req_cleanup(req);
    uv_fs_close(&g_loop, req, fd, fs_done);
}

static void fs_write_open(uv_fs_t *req)
{
    struct fsop *op = (struct fsop *)req;
    if (req->result < 0) {
        fs_fail(op);
        return;
    }
    op->fd = (int)req->result;
    /* shared by writeFile (O_TRUNC) and appendFile (O_APPEND): the
     * open flag set lives in op->openflags, set by each entry */
    uv_buf_t buf = uv_buf_init(op->data, (unsigned)op->size);
    uv_fs_req_cleanup(req);
    uv_fs_write(&g_loop, req, op->fd, &buf, 1, -1, fs_write_write);
}

/* stat shares fs_once_done: its result is the whole op, and a failed
 * stat must surface as cb(err) — not a zeroed statbuf table */

/* loop.fs.readFile(path, cb) */
static int l_fs_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_READ);
    uv_fs_open(&g_loop, &op->req, path, O_RDONLY, 0, fs_read_open);
    return 0;
}

/* loop.fs.writeFile(path, data, cb) */
static int l_fs_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 3, FS_WRITE);
    op->size = len;
    op->data = malloc(len ? len : 1);
    if (!op->data) {
        lua_pushlightuserdata(L, op);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
        luaL_unref(L, LUA_REGISTRYINDEX, op->cbref);
        keepalive_close();
        free(op);
        return luaL_error(L, "loop.fs: out of memory");
    }
    memcpy(op->data, data, len);
    op->openflags = O_WRONLY | O_CREAT | O_TRUNC;
    uv_fs_open(&g_loop, &op->req, path, op->openflags, 0644, fs_write_open);
    return 0;
}

/* loop.fs.appendFile(path, data, cb) */
static int l_fs_append_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 3, FS_WRITE);
    op->size = len;
    op->data = malloc(len ? len : 1);
    if (!op->data) {
        lua_pushlightuserdata(L, op);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
        luaL_unref(L, LUA_REGISTRYINDEX, op->cbref);
        keepalive_close();
        free(op);
        return luaL_error(L, "loop.fs: out of memory");
    }
    memcpy(op->data, data, len);
    op->openflags = O_WRONLY | O_CREAT | O_APPEND;
    uv_fs_open(&g_loop, &op->req, path, op->openflags, 0644, fs_write_open);
    return 0;
}

/* loop.fs.readdir(path, cb) -> cb(nil, {"a.txt", ...}) */
static int l_fs_readdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_READDIR);
    uv_fs_scandir(&g_loop, &op->req, path, 0, fs_scandir_done);
    return 0;
}

/* loop.fs.mkdir(path, cb) — mode 0777, umask applies as usual */
static int l_fs_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_ONCE);
    uv_fs_mkdir(&g_loop, &op->req, path, 0777, fs_once_done);
    return 0;
}

/* loop.fs.rmdir(path, cb) */
static int l_fs_rmdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_ONCE);
    uv_fs_rmdir(&g_loop, &op->req, path, fs_once_done);
    return 0;
}

/* loop.fs.unlink(path, cb) */
static int l_fs_unlink(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_ONCE);
    uv_fs_unlink(&g_loop, &op->req, path, fs_once_done);
    return 0;
}

/* loop.fs.rename(old, new, cb) */
static int l_fs_rename(lua_State *L)
{
    const char *old = luaL_checkstring(L, 1);
    const char *new = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 3, FS_ONCE);
    /* libuv's PATH2 macro copies both strings into the request */
    uv_fs_rename(&g_loop, &op->req, old, new, fs_once_done);
    return 0;
}

/* loop.fs.stat(path, cb) -> cb(err, {size=, mtime=, mode=, type=}) */
static int l_fs_stat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_STAT);
    uv_fs_stat(&g_loop, &op->req, path, fs_once_done);
    return 0;
}

/* loop.fs.lstat(path, cb) — does not follow symlinks; a symlink's
 * st.type is "link" */
static int l_fs_lstat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_STAT);
    uv_fs_lstat(&g_loop, &op->req, path, fs_once_done);
    return 0;
}

/* loop.fs.copyFile(src, dst, cb) — dst already existing is silently
 * overwritten (Node's default); no clobber-protection flag, either
 * unlink first or detect before copying */
static int l_fs_copy_file(lua_State *L)
{
    const char *src = luaL_checkstring(L, 1);
    const char *dst = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 3, FS_ONCE);
    uv_fs_copyfile(&g_loop, &op->req, src, dst, 0, fs_once_done);
    return 0;
}

/* loop.fs.access(path, cb) — existence probe: cb(nil) if it can be
 * stat'ed, cb(err) otherwise; no mode argument — readability and
 * writability surface through read/write's own err, as usual */
static int l_fs_access(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_ONCE);
    uv_fs_access(&g_loop, &op->req, path, 0 /* F_OK */, fs_once_done);
    return 0;
}

/* loop.fs.realpath(path, cb) -> cb(nil, resolved) */
static int l_fs_realpath(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, 2, FS_PATH);
    uv_fs_realpath(&g_loop, &op->req, path, fs_once_done);
    return 0;
}

/* loop.fs.truncate(path, len?, cb) — default length 0; POSIX lets
 * ftruncate grow a file too (zero-filled), same as Node's fs.truncate.
 * The length is optional Node-style: dropping it puts the callback
 * one slot earlier. */
static int l_fs_truncate(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_Integer len = 0;
    int cb_idx;
    if (lua_isfunction(L, 2)) {
        cb_idx = 2;
    } else {
        len = luaL_optinteger(L, 2, 0);
        cb_idx = 3;
    }
    luaL_checktype(L, cb_idx, LUA_TFUNCTION);
    struct fsop *op = fsop_new(L, cb_idx, FS_ONCE);
    op->size = (size_t)len;
    uv_fs_open(&g_loop, &op->req, path, O_WRONLY, 0, fs_trunc_open);
    return 0;
}

static int pin_cb(lua_State *L, int idx);   /* defined in the net section */
static int l_fs_watch(lua_State *L);   /* defined with the fswatch block */

static const luaL_Reg fs_funcs[] = {
    { "readFile", l_fs_read_file },
    { "writeFile", l_fs_write_file },
    { "appendFile", l_fs_append_file },
    { "stat", l_fs_stat },
    { "readdir", l_fs_readdir },
    { "mkdir", l_fs_mkdir },
    { "rmdir", l_fs_rmdir },
    { "unlink", l_fs_unlink },
    { "rename", l_fs_rename },
    { "lstat", l_fs_lstat },
    { "copyFile", l_fs_copy_file },
    { "access", l_fs_access },
    { "realpath", l_fs_realpath },
    { "truncate", l_fs_truncate },
    { "watch", l_fs_watch },
    { NULL, NULL },
};

/* -- fs.watch: file watching (uv_fs_event) -------------------------------
 *
 * fs.watch(path, onEvent) -> watcher; onEvent(err, filename, event)
 * fires per filesystem event (event is "rename" or "change", Node's
 * names) and is retained — errors surface through err and the
 * watcher keeps watching until watcher:close(). The filename can be
 * nil (platform-dependent); the start error (e.g. ENOENT) throws
 * synchronously, like bind/listen. An open watcher keeps the loop
 * alive — same contract as everywhere else here. */

struct fswatch {
    uv_fs_event_t h;
    int closed;
    lua_State *L;
    int selfref, evref;
};

static void fswatch_close(struct fswatch *w);

static void on_fswatch_closed(uv_handle_t *h)
{
    struct fswatch *w = (struct fswatch *)h;
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->selfref);
    w->selfref = LUA_NOREF;
    keepalive_close();
}

static void fswatch_close(struct fswatch *w)
{
    if (w->closed) {
        return;
    }
    w->closed = 1;
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->evref);
    w->evref = LUA_NOREF;
    uv_close((uv_handle_t *)&w->h, on_fswatch_closed);
}

/* Synchronous close for the start-failure path: the close-finished
 * callback only runs when the loop turns, and a caller who just caught
 * this throw may never run it again — the userdata would dangle in the
 * closing queue past its own death. Close without a callback and finish
 * the unwinding here, exactly what on_fswatch_closed would have done. */
static void fswatch_close_now(struct fswatch *w)
{
    w->closed = 1;
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->evref);
    w->evref = LUA_NOREF;
    uv_close((uv_handle_t *)&w->h, NULL);
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->selfref);
    w->selfref = LUA_NOREF;
    keepalive_close();
}

/* deliver to the retained event callback: (err, filename, event) —
 * resident, like udp.bind's onMsg */
static void fswatch_deliver(struct fswatch *w, int errcode,
                            const char *filename, int events)
{
    lua_State *L = w->L;
    if (w->closed || w->evref == LUA_NOREF) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->evref);
    if (errcode != 0) {
        lua_pushstring(L, uv_strerror(errcode));
        lua_pushnil(L);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        if (filename) {
            lua_pushstring(L, filename);
        } else {
            lua_pushnil(L);        /* filename is best-effort */
        }
        if (events & UV_RENAME) {
            lua_pushliteral(L, "rename");
        } else {
            lua_pushliteral(L, "change");
        }
    }
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: fs callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

static void on_fs_event(uv_fs_event_t *h, const char *filename, int events,
                        int status)
{
    struct fswatch *w = (struct fswatch *)h;
    if (status < 0) {
        fswatch_deliver(w, status, NULL, 0);
        return;
    }
    fswatch_deliver(w, 0, filename, events);
}

/* fs.watch(path, onEvent) -> watcher */
static int l_fs_watch(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct fswatch *w = lua_newuserdata(L, sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->L = L;
    w->evref = LUA_NOREF;
    luaL_getmetatable(L, "loop.fswatch");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    w->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    uv_fs_event_init(&g_loop, &w->h);
    w->evref = pin_cb(L, 2);
    keepalive_open();
    int rc = uv_fs_event_start(&w->h, on_fs_event, path, 0);
    if (rc != 0) {
        fswatch_close_now(w);
        return luaL_error(L, "loop.fs: watch failed: %s", uv_strerror(rc));
    }
    return 1;
}

/* watcher:close() — idempotent */
static int l_fswatch_close(lua_State *L)
{
    struct fswatch *w = luaL_checkudata(L, 1, "loop.fswatch");
    fswatch_close(w);
    return 0;
}

static int fswatch_tostring(lua_State *L)
{
    struct fswatch *w = luaL_checkudata(L, 1, "loop.fswatch");
    lua_pushfstring(L, "loop.fswatch(%s): %p",
                    w->closed ? "closed" : "open", (void *)w);
    return 1;
}

LUNA_HANDLE_CTL(fswatch, struct fswatch, "loop.fswatch")

static const luaL_Reg fswatch_funcs[] = {
    { "close", l_fswatch_close },
    { "unref", l_fswatch_unref },
    { "ref", l_fswatch_ref },
    { NULL, NULL },
};

/* -- signals (loop.signal) ---------------------------------------------
 *
 * process.on('SIGTERM', fn), libuv-shaped: loop.signal(sig, cb) keeps a
 * resident callback for one signal; every watcher watching the same
 * signal is delivered (libuv fans out). SIGINT and SIGUSR1 are refused:
 * ^C stays the loop's interrupt key and SIGUSR1 is the attach doorbell
 * — both have owners already, and libuv installs its own handler via
 * sigaction, so re-registering them here would fight those owners. */

struct sigwatch {
    uv_signal_t h;
    int closed;
    lua_State *L;
    int selfref, cbref;
};

static void on_sigwatch_closed(uv_handle_t *h)
{
    struct sigwatch *w = (struct sigwatch *)h;
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->selfref);
    w->selfref = LUA_NOREF;
    keepalive_close();
}

static void sigwatch_close(struct sigwatch *w)
{
    if (w->closed) {
        return;
    }
    w->closed = 1;
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->cbref);
    w->cbref = LUA_NOREF;
    uv_close((uv_handle_t *)&w->h, on_sigwatch_closed);
}

/* Synchronous close for the start-failure path — same reason as
 * fswatch_close_now: the finish callback only runs when the loop turns,
 * and a caller who just caught this throw may never run it again. */
static void sigwatch_close_now(struct sigwatch *w)
{
    w->closed = 1;
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->cbref);
    w->cbref = LUA_NOREF;
    uv_close((uv_handle_t *)&w->h, NULL);
    luaL_unref(w->L, LUA_REGISTRYINDEX, w->selfref);
    w->selfref = LUA_NOREF;
    keepalive_close();
}

static void on_signal(uv_signal_t *h, int signum)
{
    struct sigwatch *w = (struct sigwatch *)h;
    lua_State *L = w->L;
    if (w->closed || w->cbref == LUA_NOREF) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, w->cbref);
    lua_pushinteger(L, signum);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: signal callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

/* loop.signal(signum, cb) -> watcher */
static int l_signal(lua_State *L)
{
    lua_Integer sig = luaL_checkinteger(L, 1);
    luaL_argcheck(L, sig > 0 && sig < 65, 1, "signal number out of range");
    luaL_argcheck(L, sig != SIGINT && sig != SIGUSR1, 1,
                  "signal is reserved (SIGINT: ^C interrupt, SIGUSR1: attach)");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct sigwatch *w = lua_newuserdata(L, sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->L = L;
    w->cbref = LUA_NOREF;
    luaL_getmetatable(L, "loop.sigwatch");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    w->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    uv_signal_init(&g_loop, &w->h);
    w->cbref = pin_cb(L, 2);
    keepalive_open();
    int rc = uv_signal_start(&w->h, on_signal, (int)sig);
    if (rc != 0) {
        sigwatch_close_now(w);
        return luaL_error(L, "loop.signal: start failed: %s", uv_strerror(rc));
    }
    return 1;
}

static int l_sigwatch_close(lua_State *L)
{
    struct sigwatch *w = luaL_checkudata(L, 1, "loop.sigwatch");
    sigwatch_close(w);
    return 0;
}

static int sigwatch_tostring(lua_State *L)
{
    struct sigwatch *w = luaL_checkudata(L, 1, "loop.sigwatch");
    lua_pushfstring(L, "loop.sigwatch(%s): %p",
                    w->closed ? "closed" : "open", (void *)w);
    return 1;
}

LUNA_HANDLE_CTL(sigwatch, struct sigwatch, "loop.sigwatch")

static const luaL_Reg sigwatch_funcs[] = {
    { "close", l_sigwatch_close },
    { "unref", l_sigwatch_unref },
    { "ref", l_sigwatch_ref },
    { NULL, NULL },
};

/* -- async sockets (loop.net) ---------------------------------------------
 *
 * Client-side TCP and unix-domain streams, Node-shaped: connect(host,
 * port, cb(err, sock)) resolves through uv_getaddrinfo (never a
 * blocking lookup), connectPipe(path, cb) dials a unix socket. A sock
 * is a userdata over uv_tcp_t / uv_pipe_t:
 *
 *   sock:write(data, cb(err))    flushes, cb optional
 *   sock:read(cb)                cb(nil, chunk) per chunk, cb(nil, nil)
 *                                at EOF, cb(err) on error; repeats
 *   sock:shutdown(cb(err))       half-close (FIN), reads keep working
 *                                ('end' is a Lua keyword — the method is
 *                                spelled shutdown; 'end' stays callable
 *                                as sock['end'] for symmetry)
 *   sock:close()                 idempotent, drops every callback
 *
 * The registry pins the userdata until uv_close completes — an open
 * socket keeps the loop alive, exactly like a Node handle. Callbacks
 * run under pcall; a raising one is reported, not fatal. */

enum { SOCK_TCP, SOCK_PIPE };

struct sock {
    union {
        uv_tcp_t tcp;    /* first member: same address as the union */
        uv_pipe_t pipe;
    } h;
    int kind;
    int closed;          /* close() begun: uv_close in flight or done */
    int connected;
    int got_eof;         /* a later read() answers (nil, nil) at once */
    lua_State *L;
    int selfref;         /* registry -> userdata, dropped on close */
    int connectref;      /* pending connect callback */
    int readref;         /* active read callback (streaming) */
    int endref;          /* pending end() callback */
    uv_connect_t conn;
    uv_shutdown_t shut;
};

static void sock_close(struct sock *s);

/* deliver cb(err, value) and drop the callback reference; returns 1 if
 * the callback ran. A raising callback is reported, never fatal. */
static int sock_deliver(struct sock *s, int *cbref, int errcode,
                        int push_sock, const char *data, size_t len)
{
    lua_State *L = s->L;
    int ref = *cbref;
    *cbref = LUA_NOREF;
    if (ref == LUA_NOREF || ref == LUA_REFNIL) {
        return 0;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (errcode != 0) {
        lua_pushstring(L, uv_strerror(errcode));
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        if (push_sock) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s->selfref);
        } else if (data) {
            lua_pushlstring(L, data, len);
        } else {
            lua_pushnil(L); /* write/end: pad to two args */
        }
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: net callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
    return 1;
}

static void on_sock_closed(uv_handle_t *handle)
{
    struct sock *s = (struct sock *)handle; /* union is the first member */
    keepalive_close();
    luaL_unref(s->L, LUA_REGISTRYINDEX, s->selfref);
}

static void sock_close(struct sock *s)
{
    if (s->closed) {
        return;
    }
    s->closed = 1;
    luaL_unref(s->L, LUA_REGISTRYINDEX, s->connectref);
    s->connectref = LUA_NOREF;
    luaL_unref(s->L, LUA_REGISTRYINDEX, s->readref);
    s->readref = LUA_NOREF;
    luaL_unref(s->L, LUA_REGISTRYINDEX, s->endref);
    s->endref = LUA_NOREF;
    uv_close((uv_handle_t *)&s->h.tcp, on_sock_closed);
}

/* -- connect --------------------------------------------------------- */

static void on_connected(uv_connect_t *req, int status)
{
    struct sock *s = (struct sock *)req->handle;
    if (s->closed) {
        return; /* user closed mid-connect: refs already dropped */
    }
    if (status != 0) {
        sock_deliver(s, &s->connectref, status, 0, NULL, 0);
        sock_close(s);
        return;
    }
    s->connected = 1;
    sock_deliver(s, &s->connectref, 0, 1, NULL, 0);
}

/* malloc'd carrier for the async resolve: ai.data points back at it */
struct resolver {
    uv_getaddrinfo_t ai;
    struct sock *s;
};

static void on_resolved(uv_getaddrinfo_t *ai, int status, struct addrinfo *res)
{
    struct resolver *r = (struct resolver *)ai->data;
    struct sock *s = r->s;
    free(r); /* the carrier was only a shuttle into this callback */
    if (s->closed) {
        if (res) {
            uv_freeaddrinfo(res);
        }
        return;
    }
    if (status != 0) {
        sock_deliver(s, &s->connectref, status, 0, NULL, 0);
        sock_close(s);
        return;
    }
    uv_tcp_connect(&s->conn, &s->h.tcp, res->ai_addr, on_connected);
    uv_freeaddrinfo(res);
}

/* allocate, pin, keep-alive — everything but the handle init: spawn's
 * stdio pipes must exist (initialized) before uv_spawn points its
 * containers at them, so l_process_spawn inits them itself */
static struct sock *sock_new_uninit(lua_State *L, int kind)
{
    struct sock *s = lua_newuserdata(L, sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->L = L;
    s->connectref = LUA_NOREF;
    s->readref = LUA_NOREF;
    s->endref = LUA_NOREF;
    luaL_getmetatable(L, "loop.sock");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    s->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    keepalive_open();
    return s;
}

/* both dials share this: allocate, pin, keep-alive */
static struct sock *sock_new(lua_State *L, int kind)
{
    struct sock *s = sock_new_uninit(L, kind);
    if (kind == SOCK_TCP) {
        uv_tcp_init(&g_loop, &s->h.tcp);
    } else {
        uv_pipe_init(&g_loop, &s->h.pipe, 0);
    }
    return s;
}

static int pin_cb(lua_State *L, int idx)
{
    lua_pushvalue(L, idx);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

/* loop.net.connect(host, port, cb(err, sock)) */
static int l_net_connect(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port >= 1 && port <= 65535, 2, "port out of range");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct sock *s = sock_new(L, SOCK_TCP);
    s->connectref = pin_cb(L, 3);

    struct resolver *r = malloc(sizeof(*r));
    if (!r) {
        return luaL_error(L, "loop.net: out of memory");
    }
    r->s = s;
    r->ai.data = r;
    char service[8];
    snprintf(service, sizeof(service), "%d", (int)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = uv_getaddrinfo(&g_loop, &r->ai, on_resolved, host, service,
                            &hints);
    if (rc != 0) {
        free(r);
        sock_deliver(s, &s->connectref, rc, 0, NULL, 0);
        sock_close(s);
    }
    return 1; /* the sock userdata */
}

/* loop.net.connectPipe(path, cb(err, sock)) — unix domain stream */
static int l_net_connect_pipe(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct sock *s = sock_new(L, SOCK_PIPE);
    s->connectref = pin_cb(L, 2);
    uv_pipe_connect(&s->conn, &s->h.pipe, path, on_connected);
    return 1; /* the sock userdata */
}

/* -- sock methods ----------------------------------------------------- */

/* uv_write needs the payload alive until the callback: one malloc'd
 * request carries the copy and the optional callback. */
struct sockwrite {
    uv_write_t req;
    lua_State *L;
    int cbref;
};

static void on_written(uv_write_t *req, int status)
{
    struct sockwrite *w = (struct sockwrite *)req;
    struct sock *s = (struct sock *)req->handle;
    lua_State *L = w->L;
    int cbref = w->cbref;
    free(w);
    if (cbref == LUA_NOREF || cbref == LUA_REFNIL) {
        return;
    }
    if (s->closed) {
        luaL_unref(L, LUA_REGISTRYINDEX, cbref);
        return;
    }
    sock_deliver(s, &cbref, status < 0 ? status : 0, 0, NULL, 0);
}

/* sock:write(data, cb(err)?) */
static int l_sock_write(lua_State *L)
{
    struct sock *s = luaL_checkudata(L, 1, "loop.sock");
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int with_cb = !lua_isnoneornil(L, 3);
    if (with_cb) {
        luaL_checktype(L, 3, LUA_TFUNCTION);
    }
    luaL_argcheck(L, s->connected && !s->closed, 1, "socket not connected");
    struct sockwrite *w = malloc(sizeof(*w) + len);
    if (!w) {
        return luaL_error(L, "loop.net: out of memory");
    }
    w->L = L;
    w->cbref = with_cb ? pin_cb(L, 3) : LUA_NOREF;
    char *copy = (char *)(w + 1);
    memcpy(copy, data, len);
    uv_buf_t buf = uv_buf_init(copy, (unsigned)len);
    int rc = uv_write(&w->req, (uv_stream_t *)&s->h.tcp, &buf, 1, on_written);
    if (rc != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, w->cbref);
        free(w);
        return luaL_error(L, "loop.net: write failed: %s", uv_strerror(rc));
    }
    return 0;
}

/* streaming read delivery: a chunk keeps the callback registered (the
 * stream keeps flowing); the terminal deliveries — EOF (nil, nil) and
 * errors — consume it, like every one-shot callback here. */
static void sock_read_deliver(struct sock *s, int errcode,
                              const char *data, size_t len)
{
    lua_State *L = s->L;
    int ref = s->readref;
    int terminal = (errcode != 0) || (data == NULL);
    if (ref == LUA_NOREF || ref == LUA_REFNIL) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushnil(L);
    if (errcode != 0) {
        lua_pushstring(L, uv_strerror(errcode));
    } else if (data) {
        lua_pushlstring(L, data, len);
    } else {
        lua_pushnil(L);                    /* (nil, nil): EOF */
    }
    if (terminal) {
        s->readref = LUA_NOREF;
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: net callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

static void on_read(uv_stream_t *st, ssize_t n, const uv_buf_t *buf)
{
    struct sock *s = (struct sock *)st;
    if (n < 0) {
        free(buf->base);
        uv_read_stop(st);
        if (s->closed) {
            return; /* closing: refs already dropped */
        }
        s->got_eof = 1;
        if (n == UV_EOF) {
            sock_read_deliver(s, 0, NULL, 0); /* (nil, nil) */
        } else {
            sock_read_deliver(s, (int)n, NULL, 0);
        }
        return;
    }
    if (n == 0) {
        free(buf->base); /* nothing read this round, keep going */
        return;
    }
    sock_read_deliver(s, 0, buf->base, (size_t)n);
    free(buf->base);
}

static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
    (void)h;
    buf->base = malloc(suggested ? suggested : 1);
    buf->len = (unsigned)(suggested ? suggested : 1);
}

/* sock:read(cb) — cb(nil, chunk) per chunk; cb(nil, nil) at EOF;
 * cb(err) on error. A second call swaps the callback. */
static int l_sock_read(lua_State *L)
{
    struct sock *s = luaL_checkudata(L, 1, "loop.sock");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    luaL_argcheck(L, !s->closed, 1, "socket is closed");
    luaL_unref(L, LUA_REGISTRYINDEX, s->readref);
    s->readref = pin_cb(L, 2);
    if (s->got_eof) {
        sock_deliver(s, &s->readref, 0, 0, NULL, 0); /* already at EOF */
        return 0;
    }
    if (!s->connected) {
        return luaL_error(L, "loop.net: socket not connected");
    }
    uv_read_start((uv_stream_t *)&s->h.tcp, on_alloc, on_read);
    return 0;
}

static void on_shutdown(uv_shutdown_t *req, int status)
{
    struct sock *s = (struct sock *)req->data;
    if (s->closed) {
        return;
    }
    sock_deliver(s, &s->endref, status < 0 ? status : 0, 0, NULL, 0);
}

/* sock:shutdown(cb(err)?) — half-close: the peer sees EOF, reads keep
 * working. Named shutdown because 'end' is a Lua keyword: sock:end()
 * does not parse, and sock['end'](sock, cb) loses the colon sugar. */
static int l_sock_end(lua_State *L)
{
    struct sock *s = luaL_checkudata(L, 1, "loop.sock");
    int with_cb = !lua_isnoneornil(L, 2);
    if (with_cb) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
    }
    luaL_argcheck(L, s->connected && !s->closed, 1, "socket not connected");
    s->endref = with_cb ? pin_cb(L, 2) : LUA_NOREF;
    s->shut.data = s;
    int rc = uv_shutdown(&s->shut, (uv_stream_t *)&s->h.tcp, on_shutdown);
    if (rc != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, s->endref);
        s->endref = LUA_NOREF;
        return luaL_error(L, "loop.net: end failed: %s", uv_strerror(rc));
    }
    return 0;
}

/* sock:close() — idempotent; open sockets keep the loop alive, so a
 * finished client must close (Node parity). */
static int l_sock_close(lua_State *L)
{
    struct sock *s = luaL_checkudata(L, 1, "loop.sock");
    sock_close(s);
    return 0;
}

static int sock_tostring(lua_State *L)
{
    struct sock *s = luaL_checkudata(L, 1, "loop.sock");
    const char *kind = s->kind == SOCK_PIPE ? "pipe" : "tcp";
    const char *state = s->closed ? "closed"
                      : s->connected ? "connected" : "connecting";
    lua_pushfstring(L, "loop.sock(%s, %s): %p", kind, state, (void *)s);
    return 1;
}

/* push {address = ..., port = ..., family = "inet"|"inet6"} for an
 * AF_INET/6 sockaddr — the shape sock:peer/sockname and
 * server:address all return */
static int push_sockaddr(lua_State *L, const struct sockaddr_storage *ss)
{
    char host[128];
    lua_createtable(L, 0, 3);
    if (ss->ss_family == AF_INET || ss->ss_family == AF_INET6) {
        if (uv_ip_name((const struct sockaddr *)ss, host,
                       sizeof(host)) != 0) {
            host[0] = '\0';
        }
        lua_pushstring(L, host);
        lua_setfield(L, -2, "address");
        if (ss->ss_family == AF_INET6) {
            lua_pushinteger(L,
                ntohs(((const struct sockaddr_in6 *)ss)->sin6_port));
        } else {
            lua_pushinteger(L,
                ntohs(((const struct sockaddr_in *)ss)->sin_port));
        }
        lua_setfield(L, -2, "port");
        lua_pushstring(L, ss->ss_family == AF_INET ? "inet" : "inet6");
        lua_setfield(L, -2, "family");
    } else {
        lua_pushstring(L, "unknown");
        lua_setfield(L, -2, "family");
    }
    return 1;
}

/* sock:peer() / sock:sockname() -> {address, port?, family}; the pipe
 * variants report the path with family = "unix". Call while the
 * socket is live — a closed handle has no name to report, and a TCP
 * socket still connecting reports the OS's ENOTCONN. */
static int sock_addr(lua_State *L, int peer)
{
    struct sock *s = luaL_checkudata(L, 1, "loop.sock");
    luaL_argcheck(L, !s->closed, 1, "sock is closed");
    if (s->kind == SOCK_PIPE) {
        char buf[1024];
        size_t len = sizeof buf;
        int rc = peer ? uv_pipe_getpeername(&s->h.pipe, buf, &len)
                      : uv_pipe_getsockname(&s->h.pipe, buf, &len);
        if (rc != 0) {
            return luaL_error(L, "loop.net: get%sname: %s",
                              peer ? "peer" : "sock", uv_strerror(rc));
        }
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, buf, len);
        lua_setfield(L, -2, "address");
        lua_pushstring(L, "unix");
        lua_setfield(L, -2, "family");
        return 1;
    }
    struct sockaddr_storage ss;
    int len = sizeof ss;
    int rc = peer ? uv_tcp_getpeername(&s->h.tcp,
                                       (struct sockaddr *)&ss, &len)
                  : uv_tcp_getsockname(&s->h.tcp,
                                       (struct sockaddr *)&ss, &len);
    if (rc != 0) {
        return luaL_error(L, "loop.net: get%sname: %s",
                          peer ? "peer" : "sock", uv_strerror(rc));
    }
    return push_sockaddr(L, &ss);
}

static int l_sock_peer(lua_State *L)
{
    return sock_addr(L, 1);
}

static int l_sock_sockname(lua_State *L)
{
    return sock_addr(L, 0);
}

/* -- listen (server side) --------------------------------------------- */

/* a listening socket. The connection callback is retained (the
 * setInterval precedent) and invoked once per peer as
 * onConn(err, sock) — err-first like every other callback here. */
struct lserver {
    union {
        uv_tcp_t tcp;   /* first member: same address as the union */
        uv_pipe_t pipe;
    } h;
    int kind, closed;
    lua_State *L;
    int selfref, connref, closeref;
};

static void server_close(struct lserver *sv);

static void on_server_closed(uv_handle_t *handle)
{
    struct lserver *sv = (struct lserver *)handle;
    keepalive_close();
    /* the close callback must land while selfref still pins the
     * userdata: delivering it runs arbitrary Lua, which can allocate
     * and collect */
    if (sv->closeref != LUA_NOREF) {
        int ref = sv->closeref;
        sv->closeref = LUA_NOREF;
        lua_State *L = sv->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            fprintf(stderr, "loop: net callback error: %s\n",
                    msg ? msg : lua_typename(L, lua_type(L, -1)));
            lua_pop(L, 1);
        }
    }
    luaL_unref(sv->L, LUA_REGISTRYINDEX, sv->selfref);
}

static void server_close(struct lserver *sv)
{
    if (sv->closed) {
        return;
    }
    sv->closed = 1;
    luaL_unref(sv->L, LUA_REGISTRYINDEX, sv->connref);
    sv->connref = LUA_NOREF;
    luaL_unref(sv->L, LUA_REGISTRYINDEX, sv->closeref);
    sv->closeref = LUA_NOREF;
    uv_close((uv_handle_t *)&sv->h.tcp, on_server_closed);
}

/* deliver to the retained connection callback; keeps the reference */
static void server_deliver_conn(struct lserver *sv, int errcode,
                                struct sock *s)
{
    lua_State *L = sv->L;
    if (sv->connref == LUA_NOREF) {
        if (s) {
            sock_close(s); /* nobody is listening for peers */
        }
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, sv->connref);
    if (errcode != 0) {
        lua_pushstring(L, uv_strerror(errcode));
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->selfref);
    }
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: net callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

static void on_connection(uv_stream_t *st, int status)
{
    struct lserver *sv = (struct lserver *)st;
    if (sv->closed) {
        return;
    }
    if (status == UV_EAGAIN) {
        return; /* transient; the next pending connection re-triggers */
    }
    if (status != 0) {
        server_deliver_conn(sv, status, NULL);
        return;
    }
    struct sock *s = sock_new(sv->L, sv->kind);
    int rc = uv_accept(st, (uv_stream_t *)&s->h.tcp);
    if (rc != 0) {
        sock_close(s);
        server_deliver_conn(sv, rc, NULL);
        return;
    }
    s->connected = 1;
    server_deliver_conn(sv, 0, s);
}

/* both listens share this: allocate, pin, keep-alive */
static struct lserver *server_new(lua_State *L, int kind)
{
    struct lserver *sv = lua_newuserdata(L, sizeof(*sv));
    memset(sv, 0, sizeof(*sv));
    sv->kind = kind;
    sv->L = L;
    sv->connref = LUA_NOREF;
    sv->closeref = LUA_NOREF;
    luaL_getmetatable(L, "loop.server");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    sv->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (kind == SOCK_TCP) {
        uv_tcp_init(&g_loop, &sv->h.tcp);
    } else {
        uv_pipe_init(&g_loop, &sv->h.pipe, 0);
    }
    keepalive_open();
    return sv;
}

/* a bind target is nearly always written numerically: v4 first, then
 * v6 ("0.0.0.0" binds all interfaces). Node resolves hostnames here,
 * but that invites surprising binds — keep it explicit. */
static int bind_addr_of(const char *host, int port,
                        struct sockaddr_storage *ss)
{
    if (uv_ip4_addr(host, port, (struct sockaddr_in *)ss) == 0) {
        return 0;
    }
    if (uv_ip6_addr(host, port, (struct sockaddr_in6 *)ss) == 0) {
        return 0;
    }
    return -1;
}

/* loop.net.listen(host, port, onConn) -> server; port 0 = ephemeral
 * (read it back with server:port()). Bind/listen errors throw. */
static int l_net_listen(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port out of range");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct sockaddr_storage ss;
    if (bind_addr_of(host, (int)port, &ss) != 0) {
        return luaL_error(L, "loop.net: cannot bind '%s' (numeric address "
                             "required)", host);
    }
    struct lserver *sv = server_new(L, SOCK_TCP);
    sv->connref = pin_cb(L, 3);
    int rc = uv_tcp_bind(&sv->h.tcp, (struct sockaddr *)&ss, 0);
    if (rc == 0) {
        rc = uv_listen((uv_stream_t *)&sv->h.tcp, SOMAXCONN, on_connection);
    }
    if (rc != 0) {
        server_close(sv);
        return luaL_error(L, "loop.net: listen failed: %s", uv_strerror(rc));
    }
    return 1;
}

/* loop.net.listenPipe(path, onConn) -> server. The path must not
 * exist (unlink first); closing does NOT remove it. */
static int l_net_listen_pipe(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct lserver *sv = server_new(L, SOCK_PIPE);
    sv->connref = pin_cb(L, 2);
    int rc = uv_pipe_bind(&sv->h.pipe, path);
    if (rc == 0) {
        rc = uv_listen((uv_stream_t *)&sv->h.pipe, SOMAXCONN, on_connection);
    }
    if (rc != 0) {
        server_close(sv);
        return luaL_error(L, "loop.net: listen failed: %s", uv_strerror(rc));
    }
    return 1;
}

/* server:close(cb?) — idempotent; the callback lands once the handle
 * is really closed */
static int l_server_close(lua_State *L)
{
    struct lserver *sv = luaL_checkudata(L, 1, "loop.server");
    if (!lua_isnoneornil(L, 2) && sv->closeref == LUA_NOREF) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        sv->closeref = pin_cb(L, 2);
    }
    server_close(sv);
    return 0;
}

/* server:port() — the bound TCP port (ephemeral binds report theirs) */
static int l_server_port(lua_State *L)
{
    struct lserver *sv = luaL_checkudata(L, 1, "loop.server");
    luaL_argcheck(L, sv->kind == SOCK_TCP, 1, "unix server has no port");
    struct sockaddr_storage ss;
    int len = sizeof ss;
    int rc = uv_tcp_getsockname(&sv->h.tcp, (struct sockaddr *)&ss, &len);
    if (rc != 0) {
        return luaL_error(L, "loop.net: getsockname: %s", uv_strerror(rc));
    }
    if (ss.ss_family == AF_INET6) {
        lua_pushinteger(L,
                        ntohs(((struct sockaddr_in6 *)&ss)->sin6_port));
    } else {
        lua_pushinteger(L, ntohs(((struct sockaddr_in *)&ss)->sin_port));
    }
    return 1;
}

static int server_tostring(lua_State *L)
{
    struct lserver *sv = luaL_checkudata(L, 1, "loop.server");
    lua_pushfstring(L, "loop.server(%s, %s): %p",
                    sv->kind == SOCK_PIPE ? "pipe" : "tcp",
                    sv->closed ? "closed" : "listening", (void *)sv);
    return 1;
}

/* server:address() -> {address, port?, family}; the pipe variant
 * reports the bound path with family = "unix" */
static int l_server_address(lua_State *L)
{
    struct lserver *sv = luaL_checkudata(L, 1, "loop.server");
    if (sv->kind == SOCK_PIPE) {
        char buf[1024];
        size_t len = sizeof buf;
        int rc = uv_pipe_getsockname(&sv->h.pipe, buf, &len);
        if (rc != 0) {
            return luaL_error(L, "loop.net: getsockname: %s",
                              uv_strerror(rc));
        }
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, buf, len);
        lua_setfield(L, -2, "address");
        lua_pushstring(L, "unix");
        lua_setfield(L, -2, "family");
        return 1;
    }
    struct sockaddr_storage ss;
    int len = sizeof ss;
    int rc = uv_tcp_getsockname(&sv->h.tcp, (struct sockaddr *)&ss, &len);
    if (rc != 0) {
        return luaL_error(L, "loop.net: getsockname: %s", uv_strerror(rc));
    }
    return push_sockaddr(L, &ss);
}

LUNA_HANDLE_CTL(sock, struct sock, "loop.sock")

static const luaL_Reg sock_funcs[] = {
    { "write", l_sock_write },
    { "read", l_sock_read },
    { "shutdown", l_sock_end },
    { "end", l_sock_end },  /* bracket-callable alias: 'end' is a keyword */
    { "close", l_sock_close },
    { "peer", l_sock_peer },
    { "sockname", l_sock_sockname },
    { "unref", l_sock_unref },
    { "ref", l_sock_ref },
    { NULL, NULL },
};

LUNA_HANDLE_CTL(server, struct lserver, "loop.server")

static const luaL_Reg server_funcs[] = {
    { "close", l_server_close },
    { "port", l_server_port },
    { "address", l_server_address },
    { "unref", l_server_unref },
    { "ref", l_server_ref },
    { NULL, NULL },
};

static int l_net_connect_tls(lua_State *L);   /* TLS section, below */
static int l_net_listen_tls(lua_State *L);    /* TLS section, below */

static const luaL_Reg net_funcs[] = {
    { "connect", l_net_connect },
    { "connectPipe", l_net_connect_pipe },
    { "connectTls", l_net_connect_tls },
    { "listen", l_net_listen },
    { "listenPipe", l_net_listen_pipe },
    { "listenTls", l_net_listen_tls },
    { NULL, NULL },
};

/* -- async datagrams (loop.udp) -------------------------------------------
 *
 * Node's dgram, cut down to luna's face: udp.bind(host, port, onMsg)
 * delivers every datagram as onMsg(err, data, rinfo) with rinfo =
 * {addr=, port=} — the callback is retained like listen's onConn, and
 * recv errors surface through the same err-first slot without tearing
 * the socket down. udp.socket() returns an unbound sender whose
 * send(data, host, port, cb(err)) is one-shot per datagram; libuv
 * binds it to an ephemeral port on the first send. A bound (or sent-
 * from) socket keeps the loop alive — same contract as loop.net:
 * when the work is done, the work must be closed. */

struct udpsock {
    uv_udp_t h;
    int closed;
    lua_State *L;
    int selfref, msgref;
};

/* uv_write needs the payload alive until the callback: one malloc'd
 * request carries the copy and the optional callback. */
struct udpsend {
    uv_udp_send_t req;
    lua_State *L;
    int cbref;
};

static void udpsock_close(struct udpsock *u);

static void on_udp_closed(uv_handle_t *h)
{
    struct udpsock *u = (struct udpsock *)h;
    luaL_unref(u->L, LUA_REGISTRYINDEX, u->selfref);
    u->selfref = LUA_NOREF;
    keepalive_close();
}

static void udpsock_close(struct udpsock *u)
{
    if (u->closed) {
        return;
    }
    u->closed = 1;
    luaL_unref(u->L, LUA_REGISTRYINDEX, u->msgref);
    u->msgref = LUA_NOREF;
    uv_close((uv_handle_t *)&u->h, on_udp_closed);
}

/* deliver to the retained message callback: (err, data, rinfo) */
static void udp_deliver_msg(struct udpsock *u, int errcode,
                            const char *data, size_t len,
                            const struct sockaddr *addr)
{
    lua_State *L = u->L;
    if (u->closed || u->msgref == LUA_NOREF) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, u->msgref);
    if (errcode != 0) {
        lua_pushstring(L, uv_strerror(errcode));
        lua_pushnil(L);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_pushlstring(L, data, len);
        lua_createtable(L, 0, 2);
        char ip[64];
        if (addr->sa_family == AF_INET6) {
            uv_ip6_name((const struct sockaddr_in6 *)addr, ip, sizeof ip);
            lua_pushinteger(
                L, ntohs(((const struct sockaddr_in6 *)addr)->sin6_port));
        } else {
            uv_ip4_name((const struct sockaddr_in *)addr, ip, sizeof ip);
            lua_pushinteger(
                L, ntohs(((const struct sockaddr_in *)addr)->sin_port));
        }
        lua_setfield(L, -2, "port");
        lua_pushstring(L, ip);
        lua_setfield(L, -2, "addr");
    }
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: udp callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

static void on_udp_recv(uv_udp_t *h, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags)
{
    struct udpsock *u = (struct udpsock *)h;
    (void)flags;
    if (nread < 0) {                      /* recv error, socket stays */
        free(buf->base);
        udp_deliver_msg(u, (int)nread, NULL, 0, NULL);
        return;
    }
    if (nread == 0) {
        free(buf->base);
        /* addr == NULL: nothing pending (EAGAIN); a zero-length
         * datagram carries a peer address — deliver the empty string */
        if (addr == NULL) {
            return;
        }
        udp_deliver_msg(u, 0, "", 0, addr);
        return;
    }
    udp_deliver_msg(u, 0, buf->base, (size_t)nread, addr);
    free(buf->base);
}

static void on_udp_sent(uv_udp_send_t *req, int status)
{
    struct udpsend *w = (struct udpsend *)req;
    struct udpsock *u = (struct udpsock *)req->handle;
    lua_State *L = w->L;
    int cbref = w->cbref;
    free(w);
    if (cbref == LUA_NOREF || cbref == LUA_REFNIL) {
        return;
    }
    if (u->closed) {
        luaL_unref(L, LUA_REGISTRYINDEX, cbref);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbref);
    if (status < 0) {
        lua_pushstring(L, uv_strerror(status));
    } else {
        lua_pushnil(L);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, cbref);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: udp callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

/* both entries share this: allocate, pin, keep-alive */
static struct udpsock *udp_new(lua_State *L)
{
    struct udpsock *u = lua_newuserdata(L, sizeof(*u));
    memset(u, 0, sizeof(*u));
    u->L = L;
    u->msgref = LUA_NOREF;
    luaL_getmetatable(L, "loop.udpsock");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    u->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    uv_udp_init(&g_loop, &u->h);
    keepalive_open();
    return u;
}

/* loop.udp.bind(host, port, onMsg) -> udpsock; port 0 = ephemeral
 * (read it back with sock:port()). Bind errors throw, like listen. */
static int l_udp_bind(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port out of range");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    struct sockaddr_storage ss;
    if (bind_addr_of(host, (int)port, &ss) != 0) {
        return luaL_error(L, "loop.udp: cannot bind '%s' (numeric address "
                             "required)", host);
    }
    struct udpsock *u = udp_new(L);
    u->msgref = pin_cb(L, 3);
    int rc = uv_udp_bind(&u->h, (struct sockaddr *)&ss, 0);
    if (rc == 0) {
        rc = uv_udp_recv_start(&u->h, on_alloc, on_udp_recv);
    }
    if (rc != 0) {
        udpsock_close(u);
        return luaL_error(L, "loop.udp: bind failed: %s", uv_strerror(rc));
    }
    return 1;
}

/* loop.udp.socket() -> unbound udpsock for sending */
static int l_udp_socket(lua_State *L)
{
    udp_new(L);
    return 1;
}

/* sock:send(data, host, port, cb(err)?) — one shot per datagram */
static int l_udp_send(lua_State *L)
{
    struct udpsock *u = luaL_checkudata(L, 1, "loop.udpsock");
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    const char *host = luaL_checkstring(L, 3);
    lua_Integer port = luaL_checkinteger(L, 4);
    luaL_argcheck(L, port >= 1 && port <= 65535, 4, "port out of range");
    int with_cb = !lua_isnoneornil(L, 5);
    if (with_cb) {
        luaL_checktype(L, 5, LUA_TFUNCTION);
    }
    luaL_argcheck(L, !u->closed, 1, "socket is closed");
    struct sockaddr_storage ss;
    if (bind_addr_of(host, (int)port, &ss) != 0) {
        return luaL_error(L, "loop.udp: cannot send to '%s' (numeric "
                             "address required)", host);
    }
    struct udpsend *w = malloc(sizeof(*w) + len);
    if (!w) {
        return luaL_error(L, "loop.udp: out of memory");
    }
    w->L = L;
    w->cbref = with_cb ? pin_cb(L, 5) : LUA_NOREF;
    char *copy = (char *)(w + 1);
    memcpy(copy, data, len);
    uv_buf_t buf = uv_buf_init(copy, (unsigned)len);
    int rc = uv_udp_send(&w->req, &u->h, &buf, 1,
                         (struct sockaddr *)&ss, on_udp_sent);
    if (rc != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, w->cbref);
        free(w);
        return luaL_error(L, "loop.udp: send failed: %s", uv_strerror(rc));
    }
    return 0;
}

/* sock:port() — the bound port (ephemeral binds report theirs) */
static int l_udp_port(lua_State *L)
{
    struct udpsock *u = luaL_checkudata(L, 1, "loop.udpsock");
    luaL_argcheck(L, !u->closed, 1, "socket is closed");
    struct sockaddr_storage ss;
    int len = sizeof ss;
    int rc = uv_udp_getsockname(&u->h, (struct sockaddr *)&ss, &len);
    if (rc != 0) {
        return luaL_error(L, "loop.udp: getsockname: %s", uv_strerror(rc));
    }
    if (ss.ss_family == AF_INET6) {
        lua_pushinteger(L,
                        ntohs(((struct sockaddr_in6 *)&ss)->sin6_port));
    } else {
        lua_pushinteger(L, ntohs(((struct sockaddr_in *)&ss)->sin_port));
    }
    return 1;
}

/* sock:close() — idempotent; open sockets keep the loop alive */
static int l_udp_close(lua_State *L)
{
    struct udpsock *u = luaL_checkudata(L, 1, "loop.udpsock");
    udpsock_close(u);
    return 0;
}

static int udpsock_tostring(lua_State *L)
{
    struct udpsock *u = luaL_checkudata(L, 1, "loop.udpsock");
    lua_pushfstring(L, "loop.udpsock(%s): %p",
                    u->closed ? "closed" : "open", (void *)u);
    return 1;
}

LUNA_HANDLE_CTL(udpsock, struct udpsock, "loop.udpsock")

static const luaL_Reg udpsock_funcs[] = {
    { "send", l_udp_send },
    { "port", l_udp_port },
    { "close", l_udp_close },
    { "unref", l_udpsock_unref },
    { "ref", l_udpsock_ref },
    { NULL, NULL },
};

static const luaL_Reg udp_funcs[] = {
    { "bind", l_udp_bind },
    { "socket", l_udp_socket },
    { NULL, NULL },
};

/* -- async DNS (loop.dns) --------------------------------------------------
 *
 * The threadpool resolver net.connect uses internally, exposed on its
 * own: lookup(host, cb) delivers the first address that came back,
 * reverse(addr, cb) the first hostname. Same callback-first contract,
 * errors as strings; the callback is registry-pinned until delivery
 * and the lookup counts as a live user handle, like loop.fs. */

#include <netdb.h>

struct dnsop {
    union {
        uv_getaddrinfo_t a;
        uv_getnameinfo_t n;
    } u;               /* first member: dnsop == (struct dnsop *)&op->u */
    lua_State *L;
    int cbref;
};

/* one shared delivery tail: pull the pinned callback, call it with
 * (err, value), isolate a raising callback like every other one here */
static void dns_deliver(struct dnsop *op, const char *err, const char *value)
{
    lua_State *L = op->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, op->cbref);
    if (err) {
        lua_pushstring(L, err);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        if (value) {
            lua_pushstring(L, value);
        } else {
            lua_pushnil(L);
        }
    }
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: dns callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, op->cbref);
    keepalive_close();
    free(op);
}

static void dns_lookup_done(uv_getaddrinfo_t *req, int status,
                            struct addrinfo *res)
{
    struct dnsop *op = (struct dnsop *)req; /* u.a sits at offset 0 */
    if (status != 0) {
        dns_deliver(op, uv_strerror(status), NULL);
        return;
    }
    char ip[64] = "";
    /* walk the returned list to the first address with a printable
     * form; uv_freeaddrinfo must see the head of the list */
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if ((ai->ai_family == AF_INET || ai->ai_family == AF_INET6) &&
            uv_ip_name(ai->ai_addr, ip, sizeof(ip)) == 0) {
            break;
        }
    }
    uv_freeaddrinfo(res);
    dns_deliver(op, NULL, ip);
}

static void dns_reverse_done(uv_getnameinfo_t *req, int status,
                             const char *hostname, const char *service)
{
    (void)service; /* not delivered: Node's reverse gives the name only */
    struct dnsop *op = (struct dnsop *)req; /* u.n sits at offset 0 */
    dns_deliver(op, status != 0 ? uv_strerror(status) : NULL, hostname);
}

/* loop.dns.lookup(host, cb(err, addr)) */
static int l_dns_lookup(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct dnsop *op = malloc(sizeof(*op));
    if (!op) {
        return luaL_error(L, "loop.dns: out of memory");
    }
    op->L = L;
    lua_pushvalue(L, 2);
    op->cbref = luaL_ref(L, LUA_REGISTRYINDEX);
    keepalive_open();
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* v4 and v6 both fine, first wins */
    int rc = uv_getaddrinfo(&g_loop, &op->u.a, dns_lookup_done, host, NULL,
                            &hints);
    if (rc != 0) { /* synchronous failure: no callback will come */
        luaL_unref(L, LUA_REGISTRYINDEX, op->cbref);
        keepalive_close();
        free(op);
        return luaL_error(L, "loop.dns: %s", uv_strerror(rc));
    }
    return 0;
}

/* loop.dns.reverse(addr, cb(err, hostname)) — numeric only, both
 * families; a name that isn't an address throws like bind's bad host */
static int l_dns_reverse(lua_State *L)
{
    const char *addr = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    if (uv_ip4_addr(addr, 0, (struct sockaddr_in *)&ss) != 0 &&
        uv_ip6_addr(addr, 0, (struct sockaddr_in6 *)&ss) != 0) {
        return luaL_error(L, "loop.dns: not a numeric address: %s", addr);
    }
    struct dnsop *op = malloc(sizeof(*op));
    if (!op) {
        return luaL_error(L, "loop.dns: out of memory");
    }
    op->L = L;
    lua_pushvalue(L, 2);
    op->cbref = luaL_ref(L, LUA_REGISTRYINDEX);
    keepalive_open();
    int rc = uv_getnameinfo(&g_loop, &op->u.n, dns_reverse_done,
                            (struct sockaddr *)&ss, 0);
    if (rc != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, op->cbref);
        keepalive_close();
        free(op);
        return luaL_error(L, "loop.dns: %s", uv_strerror(rc));
    }
    return 0;
}

static const luaL_Reg dns_funcs[] = {
    { "lookup", l_dns_lookup },
    { "reverse", l_dns_reverse },
    { NULL, NULL },
};

/* -- system info (loop.os) -------------------------------------------------
 *
 * Synchronous: these read the system and return — nothing enters the
 * loop. They live under loop because libuv is where they come from
 * (uv_os_homedir and friends), the same face Node calls "os". */

#include <sys/utsname.h>

/* loop.os.home() — $HOME or the passwd entry, via uv_os_homedir */
static int l_os_home(lua_State *L)
{
    char buf[4096];
    size_t size = sizeof(buf);
    if (uv_os_homedir(buf, &size) != 0) {
        return luaL_error(L, "loop.os: cannot determine home directory");
    }
    lua_pushlstring(L, buf, size);
    return 1;
}

/* loop.os.tmpdir() — $TMPDIR or /tmp, via uv_os_tmpdir */
static int l_os_tmpdir(lua_State *L)
{
    char buf[4096];
    size_t size = sizeof(buf);
    if (uv_os_tmpdir(buf, &size) != 0) {
        return luaL_error(L, "loop.os: cannot determine temp directory");
    }
    lua_pushlstring(L, buf, size);
    return 1;
}

/* loop.os.hostname() */
static int l_os_hostname(lua_State *L)
{
    char buf[264]; /* libuv's UV_MAXHOSTNAMESIZE without the ifdef */
    size_t size = sizeof(buf);
    if (uv_os_gethostname(buf, &size) != 0) {
        return luaL_error(L, "loop.os: cannot determine hostname");
    }
    lua_pushlstring(L, buf, size);
    return 1;
}

/* loop.os.type() — the kernel name ("Linux"), like Node's os.type */
static int l_os_type(lua_State *L)
{
    struct utsname un;
    if (uname(&un) != 0) {
        return luaL_error(L, "loop.os: uname failed");
    }
    lua_pushstring(L, un.sysname);
    return 1;
}

/* loop.os.uptime() — seconds the system has been up */
static int l_os_uptime(lua_State *L)
{
    double up = 0;
    if (uv_uptime(&up) != 0) {
        return luaL_error(L, "loop.os: cannot read uptime");
    }
    lua_pushnumber(L, up);
    return 1;
}

/* loop.os.loadavg() — the three load numbers; on kernels without
 * load average they come back as zeros (same as Node) */
static int l_os_loadavg(lua_State *L)
{
    double avg[3];
    uv_loadavg(avg);
    lua_createtable(L, 3, 0);
    for (int i = 0; i < 3; i++) {
        lua_pushnumber(L, avg[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* loop.os.freemem() / loop.os.totalmem() — bytes */
static int l_os_freemem(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)uv_get_free_memory());
    return 1;
}

static int l_os_totalmem(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)uv_get_total_memory());
    return 1;
}

/* loop.os.cpus() — one table per logical CPU: model, speed (MHz),
 * times {user, nice, sys, idle, irq} in milliseconds */
static int l_os_cpus(lua_State *L)
{
    uv_cpu_info_t *infos;
    int count;
    if (uv_cpu_info(&infos, &count) != 0) {
        return luaL_error(L, "loop.os: cannot read cpu info");
    }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, infos[i].model);
        lua_setfield(L, -2, "model");
        lua_pushinteger(L, infos[i].speed);
        lua_setfield(L, -2, "speed");
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, (lua_Integer)infos[i].cpu_times.user);
        lua_setfield(L, -2, "user");
        lua_pushinteger(L, (lua_Integer)infos[i].cpu_times.nice);
        lua_setfield(L, -2, "nice");
        lua_pushinteger(L, (lua_Integer)infos[i].cpu_times.sys);
        lua_setfield(L, -2, "sys");
        lua_pushinteger(L, (lua_Integer)infos[i].cpu_times.idle);
        lua_setfield(L, -2, "idle");
        lua_pushinteger(L, (lua_Integer)infos[i].cpu_times.irq);
        lua_setfield(L, -2, "irq");
        lua_setfield(L, -2, "times");
        lua_rawseti(L, -2, i + 1);
    }
    uv_free_cpu_info(infos, count);
    return 1;
}

/* loop.os.networkInterfaces() — {name = {{address=, family=,
 * mac=, internal=}, ...}}: per-name arrays, one entry per address,
 * Node's shape. The family rides in the union's first member: both
 * sockaddr flavors keep their family tag at offset 0. */
static int l_os_network_interfaces(lua_State *L)
{
    uv_interface_address_t *ifs;
    int count;
    if (uv_interface_addresses(&ifs, &count) != 0) {
        return luaL_error(L, "loop.os: cannot read interface addresses");
    }
    lua_createtable(L, 0, 8); /* result, keyed by interface name */
    for (int i = 0; i < count; i++) {
        /* find this interface's array, creating it on first sight */
        lua_pushstring(L, ifs[i].name);
        lua_rawget(L, -2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_createtable(L, 2, 0); /* the per-name array */
            /* key and value in one move: result[name] = array,
             * leaving the array on top for the entry below */
            lua_pushstring(L, ifs[i].name);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }
        int arr = lua_gettop(L); /* the per-name array */
        lua_createtable(L, 0, 4); /* one address entry */
        char ip[64];
        if (ifs[i].address.address4.sin_family == AF_INET6) {
            uv_ip6_name(&ifs[i].address.address6, ip, sizeof(ip));
            lua_pushstring(L, ip);
            lua_setfield(L, -2, "address");
            lua_pushliteral(L, "IPv6");
            lua_setfield(L, -2, "family");
        } else {
            uv_ip4_name(&ifs[i].address.address4, ip, sizeof(ip));
            lua_pushstring(L, ip);
            lua_setfield(L, -2, "address");
            lua_pushliteral(L, "IPv4");
            lua_setfield(L, -2, "family");
        }
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ifs[i].phys_addr[0], ifs[i].phys_addr[1],
                 ifs[i].phys_addr[2], ifs[i].phys_addr[3],
                 ifs[i].phys_addr[4], ifs[i].phys_addr[5]);
        lua_pushstring(L, mac);
        lua_setfield(L, -2, "mac");
        lua_pushboolean(L, ifs[i].is_internal);
        lua_setfield(L, -2, "internal");
        size_t n = lua_rawlen(L, arr);
        lua_rawseti(L, arr, (lua_Integer)n + 1);
        lua_pop(L, 1); /* the array; result stays for the next name */
    }
    uv_free_interface_addresses(ifs, count);
    return 1;
}

static const luaL_Reg os_funcs[] = {
    { "home", l_os_home },
    { "tmpdir", l_os_tmpdir },
    { "hostname", l_os_hostname },
    { "type", l_os_type },
    { "uptime", l_os_uptime },
    { "loadavg", l_os_loadavg },
    { "freemem", l_os_freemem },
    { "totalmem", l_os_totalmem },
    { "cpus", l_os_cpus },
    { "networkInterfaces", l_os_network_interfaces },
    { NULL, NULL },
};

/* -- TLS sockets (net.connectTls) ------------------------------------------
 *
 * An OpenSSL state machine driven by uv_poll on a hand-rolled fd: the
 * fd cannot live in a uv_tcp_t (that would fight the SSL layer for
 * the event subscription), so connect, read and write are all our own
 * non-blocking loops around SSL_connect/SSL_read/SSL_write, resumed
 * from poll events whenever OpenSSL says WANT_READ/WANT_WRITE.
 *
 * The face mirrors loop.net's sock: write(data, cb), read(cb) with
 * cb(nil, chunk) streaming and cb(nil, nil) on EOF, close(cb?), the
 * same keep-alive and callback-first contract. Certificates verify
 * against the system store with SNI + hostname checking on; opts
 * {insecure = true} turns both off, opts {ca = path} pins a store. */

#ifdef LUNA_LOOP_HAVE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

struct tsock {
    uv_poll_t h;         /* first member: tsock == (struct tsock *)handle */
    int closed;
    int poll_inited;     /* h may only uv_close after uv_poll_init */
    int tcp_connected;   /* the connect() probe has seen WRITABLE */
    int handshaked;
    int got_eof;
    int is_server;       /* accepted by listenTls: SSL_accept, quiet fail */
    lua_State *L;
    int selfref;         /* registry -> userdata, dropped on close */
    int connectref;      /* pending connect/handshake callback */
    int readref;         /* resident read callback (NOREF when none) */
    int writeref;        /* pending one-shot write callback */
    int closeref;        /* pending close() callback */
    SSL_CTX *ctx;
    SSL *ssl;
    int fd;
    char host[256];      /* SNI + hostname check, copied out of Lua */
    char *pend;          /* pending write payload, owned until flushed */
    size_t pend_len, pend_off;
    uv_getaddrinfo_t ai; /* only during name resolution */
};

static void tsock_close(struct tsock *t);
static void on_tls_event(uv_poll_t *h, int status, int events);
static void on_tls_resolved(uv_getaddrinfo_t *req, int status,
                            struct addrinfo *res);

/* shared construction for client and accepted socks: userdata plus
 * metatable plus registry pin, an SSL object over fd, the handshake
 * state pointed the right way. SSL_new up-references the context; only
 * client socks keep (and free) a ctx of their own, accepted ones
 * borrow the listener's. Returns NULL (already cleaned up) when the
 * SSL object cannot be built; the caller raises. */
static struct tsock *tsock_new(lua_State *L, int fd, SSL_CTX *ctx,
                               int is_server)
{
    struct tsock *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->L = L;
    t->fd = fd;
    t->is_server = is_server;
    t->tcp_connected = is_server; /* accepted sockets are connected */
    t->readref = LUA_NOREF;
    t->writeref = LUA_NOREF;
    t->closeref = LUA_NOREF;
    t->connectref = LUA_NOREF;
    t->selfref = LUA_NOREF;

    struct tsock **p = (struct tsock **)lua_newuserdata(L, sizeof(*p));
    *p = t;
    luaL_getmetatable(L, "loop.tsock");
    lua_setmetatable(L, -2);
    t->selfref = luaL_ref(L, LUA_REGISTRYINDEX); /* pin until close */
    keepalive_open();

    t->ctx = NULL;
    t->ssl = SSL_new(ctx);
    if (!t->ssl) {
        tsock_close(t);
        return NULL;
    }
    SSL_set_fd(t->ssl, fd);
    if (is_server) {
        SSL_set_accept_state(t->ssl);
    }
    return t;
}

/* deliver cb(err, value...) and drop the callback reference; a raising
 * callback is reported, never fatal — net.sock's delivery shape */
static int tsock_deliver(struct tsock *t, int *ref, const char *err,
                         const char *data, size_t len)
{
    lua_State *L = t->L;
    int r = *ref;
    *ref = LUA_NOREF;
    if (r == LUA_NOREF || r == LUA_REFNIL) {
        return 0;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, r);
    luaL_unref(L, LUA_REGISTRYINDEX, r);
    if (err) {
        lua_pushstring(L, err);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        if (data) {
            lua_pushlstring(L, data, len);
        } else {
            lua_pushnil(L);
        }
    }
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: tls callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
    return 1;
}

/* the last SSL error on the thread's queue, as a printable string */
static void tls_errstr(char *buf, size_t cap)
{
    unsigned long code = ERR_peek_last_error();
    if (code != 0) {
        ERR_error_string_n(code, buf, cap);
    } else {
        snprintf(buf, cap, "tls: unknown error");
    }
}

/* subscribe what the state machine needs next: WRITE while a payload
 * waits (or the handshake runs), READ whenever the stream is live */
static void tls_update_events(struct tsock *t)
{
    /* a callback may have closed the sock mid-pump; libuv asserts on
     * uv_poll_start against a closing handle */
    if (t->closed || uv_is_closing((const uv_handle_t *)&t->h)) {
        return;
    }
    int ev = t->pend || !t->handshaked ? UV_READABLE | UV_WRITABLE
                                       : UV_READABLE;
    uv_poll_start(&t->h, ev, on_tls_event);
}

/* the handshake callback is delivered as cb(nil, sock): push the
 * pinned userdata itself back out of the registry */
static void tls_connect_ok(struct tsock *t)
{
    lua_State *L = t->L;
    int cr = t->connectref;
    t->connectref = LUA_NOREF;
    if (cr == LUA_NOREF || cr == LUA_REFNIL) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, cr);
    luaL_unref(L, LUA_REGISTRYINDEX, cr);
    lua_pushnil(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, t->selfref);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: tls callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

static void tls_fail(struct tsock *t, const char *err)
{
    char msg[256];
    /* SSL errors need the queue drained; plain strings pass through */
    if (t->ssl && ERR_peek_last_error() != 0) {
        tls_errstr(msg, sizeof(msg));
        err = msg;
    }
    if (t->is_server) {
        /* a peer whose handshake fails never reaches onConn: drop it
         * quietly (the error is the client's problem) */
        luaL_unref(t->L, LUA_REGISTRYINDEX, t->connectref);
        t->connectref = LUA_NOREF;
        tsock_close(t);
        return;
    }
    /* every outstanding callback hears about it, then we close */
    tsock_deliver(t, &t->writeref, err, NULL, 0);
    tsock_deliver(t, &t->readref, err, NULL, 0);
    tsock_deliver(t, &t->connectref, err, NULL, 0);
    free(t->pend);
    t->pend = NULL;
    tsock_close(t);
}

/* the whole state machine: handshake, then flush pending writes, then
 * drain reads — each stage may stop at WANT_READ/WANT_WRITE and wait
 * for the next poll event */
static void tls_pump(struct tsock *t)
{
    char buf[16384];

    if (!t->handshaked) {
        int r = t->is_server ? SSL_accept(t->ssl) : SSL_connect(t->ssl);
        if (r == 1) {
            t->handshaked = 1;
            tls_update_events(t);
            tls_connect_ok(t);
            return;
        }
        int e = SSL_get_error(t->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            return; /* events already cover both during handshake */
        }
        tls_fail(t, NULL);
        return;
    }

    if (t->pend) {
        int r = SSL_write(t->ssl, t->pend + t->pend_off,
                          (int)(t->pend_len - t->pend_off));
        if (r > 0) {
            t->pend_off += (size_t)r;
            if (t->pend_off == t->pend_len) {
                free(t->pend);
                t->pend = NULL;
                tsock_deliver(t, &t->writeref, NULL, NULL, 0);
            }
        } else {
            int e = SSL_get_error(t->ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                return;
            }
            tls_fail(t, NULL);
            return;
        }
    }

    if (t->readref == LUA_NOREF || t->got_eof) {
        tls_update_events(t);
        return;
    }
    for (;;) {
        int r = SSL_read(t->ssl, buf, sizeof(buf));
        if (r > 0) {
            tsock_deliver(t, &t->readref, NULL, buf, (size_t)r);
            if (t->readref == LUA_NOREF) { /* cb swapped itself out */
                break;
            }
            continue;
        }
        if (r == 0) { /* close_notify: the peer is done */
            t->got_eof = 1;
            tsock_deliver(t, &t->readref, NULL, NULL, 0); /* (nil, nil) */
            tsock_close(t); /* the connection is over; nothing to poll */
            return;
        }
        int e = SSL_get_error(t->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            break;
        }
        tls_fail(t, NULL);
        return;
    }
    tls_update_events(t);
}

static void on_tls_event(uv_poll_t *h, int status, int events)
{
    struct tsock *t = (struct tsock *)h;
    if (t->closed) {
        return;
    }
    if (status != 0) {
        tls_fail(t, uv_strerror(status));
        return;
    }
    if (!t->tcp_connected && (events & UV_WRITABLE)) {
        /* the kernel-level connect finished (or failed): the pending
         * error surfaces through SO_ERROR */
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        if (soerr != 0) {
            tls_fail(t, uv_strerror(soerr));
            return;
        }
        t->tcp_connected = 1;
        SSL_set_fd(t->ssl, t->fd);
    }
    tls_pump(t);
}

static void on_tls_closed(uv_handle_t *h)
{
    struct tsock *t = (struct tsock *)h;
    tsock_deliver(t, &t->closeref, NULL, NULL, 0);
    luaL_unref(t->L, LUA_REGISTRYINDEX, t->selfref);
    t->selfref = LUA_NOREF;
    keepalive_close();
}

static void tsock_close(struct tsock *t)
{
    if (t->closed) {
        return;
    }
    t->closed = 1;
    if (t->ssl) {
        SSL_shutdown(t->ssl); /* best effort, no wait */
        SSL_free(t->ssl);
        t->ssl = NULL;
    }
    if (t->ctx) {
        SSL_CTX_free(t->ctx);
        t->ctx = NULL;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    if (t->poll_inited) {
        uv_close((uv_handle_t *)&t->h, on_tls_closed);
    } else {
        /* resolution failed before any poll existed: finish inline */
        on_tls_closed((uv_handle_t *)&t->h);
    }
}

/* net.connectTls(host, port, opts?, cb) -> tsock */
static int l_net_connect_tls(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port >= 1 && port <= 65535, 2, "port out of range");
    /* Node-style optional table: dropping it puts cb one slot earlier */
    int cb_idx, opt_idx = 0;
    if (lua_isfunction(L, 3)) {
        cb_idx = 3;
    } else {
        luaL_checktype(L, 3, LUA_TTABLE);
        opt_idx = 3;
        cb_idx = 4;
    }
    luaL_checktype(L, cb_idx, LUA_TFUNCTION);

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        return luaL_error(L, "loop.net: out of memory");
    }
    int insecure = 0;
    if (opt_idx) {
        lua_getfield(L, opt_idx, "insecure");
        insecure = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, opt_idx, "ca");
        const char *ca = lua_tostring(L, -1);
        if (ca && SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1) {
            lua_pop(L, 1);
            SSL_CTX_free(ctx);
            return luaL_error(L, "loop.net: cannot load CA file '%s'", ca);
        }
        lua_pop(L, 1);
    }
    if (insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_default_verify_paths(ctx);
    }

    /* the sock (userdata, pin, SSL) — setup failures raise: a bad CA
     * path or a starved allocator is the caller's mistake, while
     * resolution/connect/handshake failures reach cb(err) at runtime */
    struct tsock *t = tsock_new(L, -1, ctx, 0);
    if (!t) {
        SSL_CTX_free(ctx);
        return luaL_error(L, "loop.net: out of memory");
    }
    t->ctx = ctx; /* the sock frees the context at close */
    snprintf(t->host, sizeof(t->host), "%s", host);
    lua_pushvalue(L, cb_idx);
    t->connectref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (!insecure) {
        SSL_set1_host(t->ssl, t->host); /* hostname checked at verify */
    }
    SSL_set_tlsext_host_name(t->ssl, t->host); /* SNI */

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char service[8];
    snprintf(service, sizeof(service), "%d", (int)port);
    int rc = uv_getaddrinfo(&g_loop, &t->ai, on_tls_resolved, host, service,
                            &hints);
    if (rc != 0) {
        tls_fail(t, uv_strerror(rc));
        return 1;
    }
    return 1; /* the tsock userdata */
}

static void on_tls_resolved(uv_getaddrinfo_t *req, int status,
                            struct addrinfo *res)
{
    struct tsock *t = (struct tsock *)((char *)req -
                                       offsetof(struct tsock, ai));
    if (t->closed) {
        if (res) {
            uv_freeaddrinfo(res);
        }
        return;
    }
    if (status != 0 || !res) {
        tls_fail(t, status != 0 ? uv_strerror(status)
                                : "no address found");
        return;
    }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        uv_freeaddrinfo(res);
        tls_fail(t, uv_strerror(-errno));
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    uv_freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        tls_fail(t, uv_strerror(-errno));
        return;
    }
    t->fd = fd;
    uv_poll_init(&g_loop, &t->h, fd);
    t->poll_inited = 1;
    if (rc == 0) { /* connected on the spot */
        t->tcp_connected = 1;
        SSL_set_fd(t->ssl, fd);
        uv_poll_start(&t->h, UV_READABLE | UV_WRITABLE, on_tls_event);
        tls_pump(t);
    } else {
        uv_poll_start(&t->h, UV_WRITABLE, on_tls_event);
    }
}

/* tsock:write(data, cb(err)?) — payload held until fully flushed */
static int l_tsock_write(lua_State *L)
{
    struct tsock **p = luaL_checkudata(L, 1, "loop.tsock");
    struct tsock *t = *p;
    luaL_argcheck(L, !t->closed, 1, "tls sock is closed");
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int with_cb = !lua_isnoneornil(L, 3);
    if (with_cb) {
        luaL_checktype(L, 3, LUA_TFUNCTION);
    }
    if (t->pend) {
        luaL_argerror(L, 2, "previous write still in flight");
    }
    if (with_cb) {
        t->writeref = pin_cb(L, 3);
    }
    if (!t->handshaked) {
        /* hold the payload until the handshake completes */
        t->pend = malloc(len ? len : 1);
        if (!t->pend) {
            return luaL_error(L, "loop.net: out of memory");
        }
        memcpy(t->pend, data, len);
        t->pend_len = len;
        t->pend_off = 0;
        return 0;
    }
    int r = SSL_write(t->ssl, data, (int)len);
    if (r == (int)len) {
        tsock_deliver(t, &t->writeref, NULL, NULL, 0);
        return 0;
    }
    if (r < 0) {
        int e = SSL_get_error(t->ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            tls_fail(t, NULL);
            return 0;
        }
    }
    /* partial or blocked: copy what is left into the pending slot */
    int off = r > 0 ? r : 0;
    t->pend = malloc(len ? len : 1);
    if (!t->pend) {
        return luaL_error(L, "loop.net: out of memory");
    }
    memcpy(t->pend, data + off, len - (size_t)off);
    t->pend_len = len;
    t->pend_off = (size_t)off;
    tls_update_events(t);
    return 0;
}

/* tsock:read(cb) — resident callback, net.sock semantics: every chunk
 * cb(nil, chunk), EOF cb(nil, nil), errors cb(err); call again to swap */
static int l_tsock_read(lua_State *L)
{
    struct tsock **p = luaL_checkudata(L, 1, "loop.tsock");
    struct tsock *t = *p;
    luaL_argcheck(L, !t->closed, 1, "tls sock is closed");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (t->readref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->readref);
    }
    t->readref = pin_cb(L, 2);
    if (t->handshaked && !t->got_eof) {
        tls_pump(t); /* bytes may already be waiting in the SSL buffer */
    }
    return 0;
}

/* tsock:close(cb?) — idempotent; pending write cb hears an error */
static int l_tsock_close(lua_State *L)
{
    struct tsock **p = luaL_checkudata(L, 1, "loop.tsock");
    struct tsock *t = *p;
    int with_cb = !lua_isnoneornil(L, 2);
    if (with_cb) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (t->closeref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, t->closeref);
        }
        t->closeref = pin_cb(L, 2);
    }
    if (t->closed) {
        /* already down (or going down): answer the cb at once */
        tsock_deliver(t, &t->closeref, NULL, NULL, 0);
        return 0;
    }
    if (t->pend) {
        free(t->pend);
        t->pend = NULL;
        tsock_deliver(t, &t->writeref, "tls: closed with a write pending",
                      NULL, 0);
    }
    tsock_deliver(t, &t->connectref, "tls: closed before handshake",
                  NULL, 0);
    tsock_close(t);
    return 0;
}

static int l_tsock_tostring(lua_State *L)
{
    struct tsock **p = luaL_checkudata(L, 1, "loop.tsock");
    struct tsock *t = *p;
    if (t->closed) {
        lua_pushliteral(L, "tsock (closed)");
    } else {
        lua_pushfstring(L, "tsock (%s)", t->handshaked ? "open" : "handshake");
    }
    return 1;
}

LUNA_HANDLE_CTL(tsock, struct tsock, "loop.tsock")

static const luaL_Reg tsock_funcs[] = {
    { "write", l_tsock_write },
    { "read", l_tsock_read },
    { "close", l_tsock_close },
    { "unref", l_tsock_unref },
    { "ref", l_tsock_ref },
    { NULL, NULL },
};

/* -- net.listenTls: the server side of the TLS face ------------------------
 *
 * The same hand-rolled style as connectTls, mirrored: a plain listening
 * socket (bind/listen synchronously, like net.listen) polled for
 * incoming connections; every accept builds a server-side tsock whose
 * SSL_accept runs off the same pump. Only fully handshaken connections
 * reach onConn — one that fails verification or tears mid-handshake is
 * dropped quietly, the error being the client's to see. */

struct tserver {
    uv_poll_t h;         /* first member: polls the listening fd */
    int closed;
    int poll_inited;
    lua_State *L;
    int selfref, connref, closeref;
    SSL_CTX *ctx;
    int listen_fd;
    int bound_port;      /* read back by :port() */
};

static void tserver_close(struct tserver *sv);

static void on_tserver_closed(uv_handle_t *handle)
{
    struct tserver *sv = (struct tserver *)handle;
    keepalive_close();
    if (sv->closeref != LUA_NOREF) {
        int ref = sv->closeref;
        sv->closeref = LUA_NOREF;
        lua_State *L = sv->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            fprintf(stderr, "loop: tls callback error: %s\n",
                    msg ? msg : lua_typename(L, lua_type(L, -1)));
            lua_pop(L, 1);
        }
    }
    luaL_unref(sv->L, LUA_REGISTRYINDEX, sv->selfref);
}

static void tserver_close(struct tserver *sv)
{
    if (sv->closed) {
        return;
    }
    sv->closed = 1;
    luaL_unref(sv->L, LUA_REGISTRYINDEX, sv->connref);
    sv->connref = LUA_NOREF;
    luaL_unref(sv->L, LUA_REGISTRYINDEX, sv->closeref);
    sv->closeref = LUA_NOREF;
    /* live connections keep their SSL up-reference to the context, so
     * freeing here is safe while they finish out their conversations */
    SSL_CTX_free(sv->ctx);
    sv->ctx = NULL;
    if (sv->listen_fd >= 0) {
        close(sv->listen_fd);
        sv->listen_fd = -1;
    }
    if (sv->poll_inited) {
        uv_close((uv_handle_t *)&sv->h, on_tserver_closed);
    } else {
        on_tserver_closed((uv_handle_t *)&sv->h);
    }
}

/* deliver a handshaken connection to the retained onConn callback */
static void tserver_deliver_conn(struct tserver *sv, struct tsock *t)
{
    lua_State *L = sv->L;
    if (sv->connref == LUA_NOREF) {
        if (t) {
            tsock_close(t); /* nobody is listening for peers */
        }
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, sv->connref);
    lua_pushnil(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, t->selfref);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: tls callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
}

/* poll callback on the listening socket: accept one, start its TLS
 * handshake; the peer surfaces through onConn when the pump finishes */
static void on_tserver_event(uv_poll_t *h, int status, int events)
{
    struct tserver *sv = (struct tserver *)h;
    if (sv->closed || status != 0 || !(events & UV_READABLE)) {
        if (status != 0 && !sv->closed) {
            tserver_close(sv); /* the listener itself failed */
        }
        return;
    }
    int c = accept(sv->listen_fd, NULL, NULL);
    if (c < 0) {
        return; /* EAGAIN or a transient accept error: poll re-arms */
    }
    int flags = fcntl(c, F_GETFL, 0);
    fcntl(c, F_SETFL, flags | O_NONBLOCK);
    struct tsock *t = tsock_new(sv->L, c, sv->ctx, 1);
    if (!t) {
        close(c);
        return;
    }
    /* pending handshake delivery: tsock_deliver pushes (nil, sock),
     * exactly the onConn shape — the conn ref rides in connectref */
    lua_rawgeti(sv->L, LUA_REGISTRYINDEX, sv->connref);
    t->connectref = luaL_ref(sv->L, LUA_REGISTRYINDEX);
    t->poll_inited = 1;
    uv_poll_init(&g_loop, &t->h, c);
    uv_poll_start(&t->h, UV_READABLE | UV_WRITABLE, on_tls_event);
    tls_pump(t); /* the ClientHello may already be readable */
}

/* net.listenTls(host, port, opts, onConn) -> server; opts carries
 * cert and key paths (required). Bind/listen/cert errors throw. */
static int l_net_listen_tls(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port >= 0 && port <= 65535, 2, "port out of range");
    luaL_checktype(L, 3, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    lua_getfield(L, 3, "cert");
    const char *cert = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 3, "key");
    const char *key = lua_tostring(L, -1);
    lua_pop(L, 1);
    luaL_argcheck(L, cert != NULL, 3, "opts.cert (path) required");
    luaL_argcheck(L, key != NULL, 3, "opts.key (path) required");

    struct sockaddr_storage ss;
    if (bind_addr_of(host, (int)port, &ss) != 0) {
        return luaL_error(L, "loop.net: cannot bind '%s' (numeric address "
                             "required)", host);
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        return luaL_error(L, "loop.net: out of memory");
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        char msg[256];
        tls_errstr(msg, sizeof(msg));
        SSL_CTX_free(ctx);
        return luaL_error(L, "loop.net: cannot load certificate: %s", msg);
    }

    struct tserver *sv = lua_newuserdata(L, sizeof(*sv));
    memset(sv, 0, sizeof(*sv));
    sv->L = L;
    sv->connref = LUA_NOREF;
    sv->closeref = LUA_NOREF;
    sv->ctx = ctx;
    sv->listen_fd = -1;
    luaL_getmetatable(L, "loop.tserver");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    sv->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    keepalive_open();

    sv->listen_fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (sv->listen_fd < 0) {
        int e = errno;
        tserver_close(sv);
        return luaL_error(L, "loop.net: socket failed: %s", strerror(e));
    }
    int one = 1;
    setsockopt(sv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(sv->listen_fd, (struct sockaddr *)&ss, sizeof ss) != 0 ||
        listen(sv->listen_fd, SOMAXCONN) != 0) {
        int e = errno;
        tserver_close(sv);
        return luaL_error(L, "loop.net: listen failed: %s", strerror(e));
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof bound;
    if (getsockname(sv->listen_fd, (struct sockaddr *)&bound, &blen) == 0) {
        sv->bound_port = ntohs(bound.sin_port);
    }
    sv->connref = pin_cb(L, 4);

    sv->poll_inited = 1;
    uv_poll_init(&g_loop, &sv->h, sv->listen_fd);
    int rc = uv_poll_start(&sv->h, UV_READABLE, on_tserver_event);
    if (rc != 0) {
        tserver_close(sv);
        return luaL_error(L, "loop.net: listen failed: %s", uv_strerror(rc));
    }
    return 1;
}

static int l_tserver_close(lua_State *L)
{
    struct tserver *sv = luaL_checkudata(L, 1, "loop.tserver");
    int with_cb = !lua_isnoneornil(L, 2);
    if (with_cb) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (sv->closeref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, sv->closeref);
        }
        sv->closeref = pin_cb(L, 2);
    }
    tserver_close(sv);
    return 0;
}

static int l_tserver_port(lua_State *L)
{
    struct tserver *sv = luaL_checkudata(L, 1, "loop.tserver");
    if (sv->closed) {
        return luaL_error(L, "loop.net: tls server is closed");
    }
    lua_pushinteger(L, sv->bound_port);
    return 1;
}

static int l_tserver_tostring(lua_State *L)
{
    struct tserver *sv = luaL_checkudata(L, 1, "loop.tserver");
    if (sv->closed) {
        lua_pushliteral(L, "tserver (closed)");
    } else {
        lua_pushfstring(L, "tserver (%d)", sv->bound_port);
    }
    return 1;
}

LUNA_HANDLE_CTL(tserver, struct tserver, "loop.tserver")

static const luaL_Reg tserver_funcs[] = {
    { "close", l_tserver_close },
    { "port", l_tserver_port },
    { "unref", l_tserver_unref },
    { "ref", l_tserver_ref },
    { NULL, NULL },
};

#else  /* no OpenSSL: connectTls/listenTls exist and explain themselves */

static int l_net_connect_tls(lua_State *L)
{
    return luaL_error(L,
        "loop.net: connectTls needs a build with OpenSSL (the crypto "
        "module's dependency); this binary was built without it");
}

static int l_net_listen_tls(lua_State *L)
{
    return luaL_error(L,
        "loop.net: listenTls needs a build with OpenSSL (the crypto "
        "module's dependency); this binary was built without it");
}

#endif /* LUNA_LOOP_HAVE_OPENSSL */

/* -- async child processes (loop.process) ---------------------------------
 *
 * run(cmd, args, [opts], cb): no shell, no PATH games beyond execvp's
 * own — args is a plain argv tail. stdout/stderr come back as two
 * capture pipes drained into growing buffers; stdin is ignored. The
 * completion fires only when BOTH the exit callback and both pipe EOFs
 * have landed (a dying child always closes its fds, so this drains
 * deterministically). Like the sockets, the handle is registry-pinned
 * until delivery and keeps the loop alive. */

struct proc;

struct piper {
    uv_pipe_t p;         /* first member: piper == (struct piper *)handle */
    struct proc *owner;
    int is_err;
    char *buf;
    size_t len, cap;
};

enum { PROC_RUN, PROC_SPAWN };

struct proc {
    uv_process_t h;      /* first member: proc == (struct proc *)handle */
    lua_State *L;
    int mode;            /* PROC_RUN (capture) or PROC_SPAWN (stdio socks) */
    int selfref;         /* registry -> userdata, dropped at delivery */
    int cbref;           /* completion callback */
    int exit_seen, proc_closed, pipes_closed, delivered;
    int64_t status;      /* exit code (meaningful only when signum == 0) */
    int signum;
    /* PROC_RUN: capture pipes, buffers moved here at pipe EOF */
    struct piper *out, *err;
    char *out_data, *err_data;
    size_t out_len, err_len;
    /* PROC_SPAWN: the three stdio socks; the refs pin the userdatas
     * until delivery (then ownership of live ones goes to the caller,
     * held only by each sock's own selfref, like any net socket) */
    struct sock *in_s, *out_s, *err_s;
    int inref, outref, errref;
};

static void proc_try_close(struct proc *pr);

/* allocate, pin, keep-alive — shared by run/spawn */
static struct proc *proc_new(lua_State *L, int mode)
{
    struct proc *pr = lua_newuserdata(L, sizeof(*pr));
    memset(pr, 0, sizeof(*pr));
    pr->L = L;
    pr->mode = mode;
    pr->selfref = LUA_NOREF;
    pr->cbref = LUA_NOREF;
    pr->inref = pr->outref = pr->errref = LUA_NOREF;
    luaL_getmetatable(L, "loop.process");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    pr->selfref = luaL_ref(L, LUA_REGISTRYINDEX);
    return pr;
}

/* spawn-failure path: nobody is left to own the pipers */
static void on_piper_gone_dead(uv_handle_t *handle)
{
    struct piper *pp = (struct piper *)handle;
    free(pp->buf);
    free(pp);
}

/* normal path: hand the capture buffer to the owner, free the piper */
static void on_piper_gone(uv_handle_t *handle)
{
    struct piper *pp = (struct piper *)handle;
    struct proc *pr = pp->owner;
    if (pp->is_err) {
        pr->err_data = pp->buf;
        pr->err_len = pp->len;
    } else {
        pr->out_data = pp->buf;
        pr->out_len = pp->len;
    }
    free(pp);
    pr->pipes_closed++;
    proc_try_close(pr);
}

static void on_proc_alloc(uv_handle_t *handle, size_t suggested,
                          uv_buf_t *buf)
{
    (void)handle;
    (void)suggested;
    buf->base = malloc(65536);
    buf->len = buf->base ? 65536 : 0;
}

static void on_proc_read(uv_stream_t *stream, ssize_t nread,
                         const uv_buf_t *buf)
{
    struct piper *pp = (struct piper *)stream;
    if (nread > 0) {
        if (pp->len + (size_t)nread > pp->cap) {
            size_t cap = pp->cap ? pp->cap * 2 : 8192;
            while (cap < pp->len + (size_t)nread) {
                cap *= 2;
            }
            char *nb = realloc(pp->buf, cap);
            if (nb) {
                pp->buf = nb;
                pp->cap = cap;
            }
        }
        if (pp->buf && pp->len + (size_t)nread <= pp->cap) {
            memcpy(pp->buf + pp->len, buf->base, (size_t)nread);
            pp->len += (size_t)nread;
        }
        /* on OOM the chunk is dropped: capture stays best-effort */
    }
    free(buf->base);
    if (nread < 0) {     /* EOF or a hard read error: stream is done */
        uv_read_stop(stream);
        uv_close((uv_handle_t *)pp, on_piper_gone);
    }
}

static void on_proc_handle_closed(uv_handle_t *handle)
{
    struct proc *pr = (struct proc *)handle;
    pr->proc_closed = 1;
    proc_try_close(pr);
}

static void on_proc_exit(uv_process_t *h, int64_t status, int signum)
{
    struct proc *pr = (struct proc *)h;
    pr->exit_seen = 1;
    pr->status = status;
    pr->signum = signum;
    uv_close((uv_handle_t *)h, on_proc_handle_closed);
}

/* deliver cb(nil, res) once the child exited and the process handle is
 * closed — plus, for run(), both capture pipes drained. spawn() does
 * NOT wait for the stdio pipes: a pipe nobody reads never EOFs, so
 * onExit mirrors Node's 'exit' and the streams keep flowing afterwards.
 * The callback still runs with every pin held — it can raise, like
 * any. */
static void proc_try_close(struct proc *pr)
{
    if (pr->delivered || !pr->exit_seen || !pr->proc_closed) {
        return;
    }
    if (pr->mode == PROC_RUN && pr->pipes_closed < 2) {
        return;
    }
    pr->delivered = 1;
    lua_State *L = pr->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, pr->cbref);
    luaL_unref(L, LUA_REGISTRYINDEX, pr->cbref);
    pr->cbref = LUA_NOREF;
    lua_pushnil(L);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)pr->status);
    lua_setfield(L, -2, "status");
    if (pr->signum != 0) {
        lua_pushinteger(L, pr->signum);
        lua_setfield(L, -2, "signal");
    }
    if (pr->mode == PROC_RUN) {
        lua_pushlstring(L, pr->out_data ? pr->out_data : "", pr->out_len);
        lua_setfield(L, -2, "stdout");
        lua_pushlstring(L, pr->err_data ? pr->err_data : "", pr->err_len);
        lua_setfield(L, -2, "stderr");
    }
    if (pr->mode == PROC_SPAWN) {
        /* drop the pins BEFORE delivery: onExit already mirrors Node's
         * 'exit', and inside it p:stdout() must read nil — a live sock
         * is held only by its own selfref (dropped when the user
         * closes it), a dead one can finally be collected */
        luaL_unref(L, LUA_REGISTRYINDEX, pr->inref);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->outref);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->errref);
        pr->inref = pr->outref = pr->errref = LUA_NOREF;
        pr->in_s = pr->out_s = pr->err_s = NULL;
    }
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "loop: process callback error: %s\n",
                msg ? msg : lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
    }
    if (pr->mode == PROC_RUN) {
        free(pr->out_data);
        free(pr->err_data);
        pr->out_data = pr->err_data = NULL;
    }
    keepalive_close();
    luaL_unref(L, LUA_REGISTRYINDEX, pr->selfref);
    pr->selfref = LUA_NOREF;
}

static int proc_tostring(lua_State *L)
{
    struct proc *pr = luaL_checkudata(L, 1, "loop.process");
    lua_pushfstring(L, "loop.process(pid %d, %s): %p",
                    (int)pr->h.pid,
                    pr->delivered ? "done"
                        : pr->exit_seen ? "exited" : "running",
                    (void *)pr);
    return 1;
}

static int l_proc_pid(lua_State *L)
{
    struct proc *pr = luaL_checkudata(L, 1, "loop.process");
    lua_pushinteger(L, (lua_Integer)pr->h.pid);
    return 1;
}

/* proc:kill(sig?) — default SIGTERM; the exit callback still fires */
static int l_proc_kill(lua_State *L)
{
    struct proc *pr = luaL_checkudata(L, 1, "loop.process");
    luaL_argcheck(L, !pr->delivered && !pr->exit_seen, 1,
                  "process already exited");
    lua_Integer sig = luaL_optinteger(L, 2, SIGTERM);
    int rc = uv_process_kill(&pr->h, (int)sig);
    if (rc != 0) {
        return luaL_error(L, "loop.process: kill: %s", uv_strerror(rc));
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* the shared preamble of run/spawn — (cmd, args?, opts {cwd}?, cb).
 * Sets *nargs and *cbidx; an opts table is replaced by its anchored
 * cwd string at slot 3 (NULL when unset). Returns cmd. */
static const char *proc_parse_args(lua_State *L, int *nargs, int *cbidx,
                                   const char **cwd)
{
    const char *cmd = luaL_checkstring(L, 1);
    *nargs = 0;
    *cwd = NULL;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        *nargs = (int)lua_rawlen(L, 2);
        for (int i = 1; i <= *nargs; i++) {
            lua_rawgeti(L, 2, i);
            luaL_argcheck(L, !lua_isnil(L, -1) && lua_tostring(L, -1), 2,
                          "args must be strings");
            lua_pop(L, 1);
        }
    }
    *cbidx = 3;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "cwd");
        if (!lua_isnil(L, -1)) {
            *cwd = lua_tostring(L, -1);
        }
        lua_replace(L, 3); /* anchor the cwd string on the stack */
        *cbidx = 4;
    }
    luaL_checktype(L, *cbidx, LUA_TFUNCTION);
    return cmd;
}

/* copy argv out of Lua: the stack slots die when we return */
static void proc_free_argv(char **args)
{
    if (!args) {
        return;
    }
    for (int i = 0; args[i]; i++) {
        free(args[i]);
    }
    free(args);
}

/* NULL on failure (a partial array is freed); no uv handle exists
 * while this runs, so failing callers just drop their pins */
static char **proc_build_argv(lua_State *L, const char *cmd, int nargs)
{
    char **args = malloc(((size_t)nargs + 2) * sizeof(char *));
    if (!args) {
        return NULL;
    }
    args[0] = malloc(strlen(cmd) + 1);
    if (!args[0]) {
        free(args);
        return NULL;
    }
    strcpy(args[0], cmd);
    for (int i = 0; i < nargs; i++) {
        lua_rawgeti(L, 2, i + 1);
        const char *a = lua_tostring(L, -1);
        args[i + 1] = malloc(strlen(a) + 1);
        if (!args[i + 1]) {
            lua_pop(L, 1);
            args[i + 1] = NULL;
            proc_free_argv(args);
            return NULL;
        }
        strcpy(args[i + 1], a);
        lua_pop(L, 1);
    }
    args[nargs + 1] = NULL;
    return args;
}

/* spawn-failure tail: every handle the attempt opened gets closed,
 * every pin dropped, then throw — the run still drains afterwards */
static int proc_spawn_error(lua_State *L, struct proc *pr, int rc)
{
    luaL_unref(L, LUA_REGISTRYINDEX, pr->cbref);
    luaL_unref(L, LUA_REGISTRYINDEX, pr->selfref);
    pr->cbref = LUA_NOREF;
    pr->selfref = LUA_NOREF;
    if (pr->mode == PROC_RUN) {
        uv_close((uv_handle_t *)&pr->out->p, on_piper_gone_dead);
        uv_close((uv_handle_t *)&pr->err->p, on_piper_gone_dead);
    } else {
        sock_close(pr->in_s);
        sock_close(pr->out_s);
        sock_close(pr->err_s);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->inref);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->outref);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->errref);
    }
    return luaL_error(L, "loop.process: spawn failed: %s", uv_strerror(rc));
}

static void proc_options_of(uv_process_options_t *opts, char **args,
                            const char *cwd, uv_stdio_container_t *io)
{
    memset(opts, 0, sizeof(*opts));
    opts->exit_cb = on_proc_exit;
    opts->file = args[0];
    opts->args = args;
    opts->cwd = cwd;
    opts->stdio_count = 3;
    opts->stdio = io;
}

/* loop.process.run(cmd, args?, [opts], cb(nil, res)) -> proc.
 * opts: { cwd = path }. Spawn failures throw, like listen(). */
static int l_process_run(lua_State *L)
{
    int nargs, cbidx;
    const char *cwd;
    const char *cmd = proc_parse_args(L, &nargs, &cbidx, &cwd);
    struct proc *pr = proc_new(L, PROC_RUN);
    pr->cbref = pin_cb(L, cbidx);
    char **args = proc_build_argv(L, cmd, nargs);
    pr->out = calloc(1, sizeof(*pr->out));
    pr->err = calloc(1, sizeof(*pr->err));
    if (!args || !pr->out || !pr->err) {
        proc_free_argv(args);
        free(pr->out);
        free(pr->err);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->cbref);
        luaL_unref(L, LUA_REGISTRYINDEX, pr->selfref);
        return luaL_error(L, "loop.process: out of memory");
    }
    pr->out->owner = pr;
    pr->err->owner = pr;
    pr->err->is_err = 1;
    uv_pipe_init(&g_loop, &pr->out->p, 0);
    uv_pipe_init(&g_loop, &pr->err->p, 0);

    uv_stdio_container_t io[3];
    io[0].flags = UV_IGNORE;
    io[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    io[1].data.stream = (uv_stream_t *)pr->out;
    io[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    io[2].data.stream = (uv_stream_t *)pr->err;
    uv_process_options_t opts;
    proc_options_of(&opts, args, cwd, io);

    int rc = uv_spawn(&g_loop, &pr->h, &opts);
    proc_free_argv(args);
    if (rc != 0) {
        return proc_spawn_error(L, pr, rc);
    }
    keepalive_open();
    uv_read_start((uv_stream_t *)pr->out, on_proc_alloc, on_proc_read);
    uv_read_start((uv_stream_t *)pr->err, on_proc_alloc, on_proc_read);
    return 1;
}

/* loop.process.spawn(cmd, args?, [opts], onExit(nil, res)) -> proc
 * with live stdio: proc:stdin()/stdout()/stderr() hand out ordinary
 * socks (stdin writable, stdout/stderr readable). onExit mirrors
 * Node's 'exit' — it fires on process exit regardless of the streams,
 * which keep flowing until the user closes them. */
static int l_process_spawn(lua_State *L)
{
    int nargs, cbidx;
    const char *cwd;
    const char *cmd = proc_parse_args(L, &nargs, &cbidx, &cwd);
    struct proc *pr = proc_new(L, PROC_SPAWN);
    pr->cbref = pin_cb(L, cbidx);

    /* the stdio socks must exist, initialized, before uv_spawn points
     * the containers at them; each pins itself and opens its own
     * keep-alive — the spawn-failure tail balances all of it */
    pr->in_s = sock_new_uninit(L, SOCK_PIPE);
    pr->inref = luaL_ref(L, LUA_REGISTRYINDEX);
    pr->out_s = sock_new_uninit(L, SOCK_PIPE);
    pr->outref = luaL_ref(L, LUA_REGISTRYINDEX);
    pr->err_s = sock_new_uninit(L, SOCK_PIPE);
    pr->errref = luaL_ref(L, LUA_REGISTRYINDEX);
    uv_pipe_init(&g_loop, &pr->in_s->h.pipe, 0);
    uv_pipe_init(&g_loop, &pr->out_s->h.pipe, 0);
    uv_pipe_init(&g_loop, &pr->err_s->h.pipe, 0);

    char **args = proc_build_argv(L, cmd, nargs);
    if (!args) {
        return proc_spawn_error(L, pr, UV_ENOMEM);
    }

    uv_stdio_container_t io[3];
    io[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE; /* child reads it */
    io[0].data.stream = (uv_stream_t *)&pr->in_s->h.pipe;
    io[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    io[1].data.stream = (uv_stream_t *)&pr->out_s->h.pipe;
    io[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    io[2].data.stream = (uv_stream_t *)&pr->err_s->h.pipe;
    uv_process_options_t opts;
    proc_options_of(&opts, args, cwd, io);

    int rc = uv_spawn(&g_loop, &pr->h, &opts);
    proc_free_argv(args);
    if (rc != 0) {
        return proc_spawn_error(L, pr, rc);
    }
    keepalive_open();
    pr->in_s->connected = pr->out_s->connected = pr->err_s->connected = 1;
    return 1;
}

/* proc:stdin()/stdout()/stderr() — the stdio socks of a spawn(); nil
 * once delivered (ownership released at onExit). */
static int proc_stdio_get(lua_State *L, struct proc *pr, int ref)
{
    luaL_argcheck(L, pr->mode == PROC_SPAWN, 1,
                  "run() procs have no stdio streams");
    if (ref == LUA_NOREF) {
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    }
    return 1;
}

static int l_proc_stdin(lua_State *L)
{
    struct proc *pr = luaL_checkudata(L, 1, "loop.process");
    return proc_stdio_get(L, pr, pr->inref);
}

static int l_proc_stdout(lua_State *L)
{
    struct proc *pr = luaL_checkudata(L, 1, "loop.process");
    return proc_stdio_get(L, pr, pr->outref);
}

static int l_proc_stderr(lua_State *L)
{
    struct proc *pr = luaL_checkudata(L, 1, "loop.process");
    return proc_stdio_get(L, pr, pr->errref);
}

LUNA_HANDLE_CTL(proc, struct proc, "loop.process")

static const luaL_Reg proc_funcs[] = {
    { "pid", l_proc_pid },
    { "kill", l_proc_kill },
    { "stdin", l_proc_stdin },
    { "stdout", l_proc_stdout },
    { "stderr", l_proc_stderr },
    { "unref", l_proc_unref },
    { "ref", l_proc_ref },
    { NULL, NULL },
};

static const luaL_Reg process_funcs[] = {
    { "run", l_process_run },
    { "spawn", l_process_spawn },
    { NULL, NULL },
};

/* handle:ref / handle:unref — box's pair and method table live here,
 * after loopbox; every other face expands the macro right above its own
 * method table */

LUNA_HANDLE_CTL(box, struct loopbox, "loop.handle")

static const luaL_Reg box_funcs[] = {
    { "unref", l_box_unref },
    { "ref", l_box_ref },
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
        luaL_newlib(L, box_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, handle_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "loop.sock")) {
        luaL_newlib(L, sock_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, sock_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
#ifdef LUNA_LOOP_HAVE_OPENSSL
    if (luaL_newmetatable(L, "loop.tsock")) {
        luaL_newlib(L, tsock_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_tsock_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "loop.tserver")) {
        luaL_newlib(L, tserver_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_tserver_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
#endif
    if (luaL_newmetatable(L, "loop.server")) {
        luaL_newlib(L, server_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, server_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "loop.process")) {
        luaL_newlib(L, proc_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, proc_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "loop.udpsock")) {
        luaL_newlib(L, udpsock_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, udpsock_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "loop.fswatch")) {
        luaL_newlib(L, fswatch_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, fswatch_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "loop.sigwatch")) {
        luaL_newlib(L, sigwatch_funcs);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, sigwatch_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    luaL_newlib(L, loop_funcs);
    luaL_newlib(L, fs_funcs);
    lua_setfield(L, -2, "fs");
    luaL_newlib(L, net_funcs);
    lua_setfield(L, -2, "net");
    luaL_newlib(L, process_funcs);
    lua_setfield(L, -2, "process");
    luaL_newlib(L, udp_funcs);
    lua_setfield(L, -2, "udp");
    luaL_newlib(L, dns_funcs);
    lua_setfield(L, -2, "dns");
    luaL_newlib(L, os_funcs);
    lua_setfield(L, -2, "os");
    /* named signal numbers for loop.signal, Linux-standard; SIGKILL and
     * SIGSTOP are listed but the kernel never delivers them */
    static const struct { const char *name; int num; } sig_names[] = {
        { "HUP", SIGHUP },       { "INT", SIGINT },       { "QUIT", SIGQUIT },
        { "ILL", SIGILL },       { "ABRT", SIGABRT },     { "FPE", SIGFPE },
        { "KILL", SIGKILL },     { "SEGV", SIGSEGV },     { "PIPE", SIGPIPE },
        { "ALRM", SIGALRM },     { "TERM", SIGTERM },     { "USR1", SIGUSR1 },
        { "USR2", SIGUSR2 },     { "CHLD", SIGCHLD },     { "CONT", SIGCONT },
        { "STOP", SIGSTOP },     { "TSTP", SIGTSTP },     { "TTIN", SIGTTIN },
        { "TTOU", SIGTTOU },
    };
    lua_createtable(L, 0, (int)(sizeof(sig_names) / sizeof(sig_names[0])));
    for (size_t i = 0; i < sizeof(sig_names) / sizeof(sig_names[0]); i++) {
        lua_pushinteger(L, sig_names[i].num);
        lua_setfield(L, -2, sig_names[i].name);
    }
    lua_setfield(L, -2, "sig");
    return 1;
}
