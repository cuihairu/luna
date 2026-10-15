[English](architecture.en.md) | [中文](architecture.md)

# Architecture design

This document answers four questions: **how the code is layered, how third-party libraries are chosen, where the plugin boundary sits, and where the boundary of the event loop sits**.

## Layering: a three-layer model

luna's shape is told by three layers — **User Space / Luna Runtime / Native Runtime**. The product identity is a general-purpose Lua runtime; the isomorphism with Node.js's official dependency-layering diagram happens only at the level of architectural reference (the comparison follows in the next section).

```
┌─ User Space ─────────────────────────────────────────────────────────────────┐
│  your script · REPL session · CLI · app                                      │
│  luna script.lua · luna build.lua · luna -e · luna --attach <pid>            │
├─ Luna Runtime ───────────────────────────────────────────────────────────────┤
│  luna_modules/ packages (Node-style resolution: relative require,            │
│    climbing up level by level · manifest main)                               │
│  LuaRocks wrapper: luna install/search/list/update → .luna/rocks tree        │
│  stdlib (ships with the binary, require-and-go; four-way                     │
│    classification on the stdlib overview page)                               │
│    Core        json fs path util events stream net http crypto               │
│    Data-Format csv ini toml yaml xml zlib                                    │
│    Dev tooling logging · repl/introspect/magic/complete/highlight            │
│    Ecosystem   plugins · LuaRocks · the lua_modules integration face         │
│  plugins plugins/ (project → user → $LUNA_PLUGIN_PATH, isolated)             │
│    extension points: magic.register · complete.add_source ·                  │
│    highlight.set · module injection                                          │
│  REPL policy layer (pure Lua, compiled into the binary at build time)        │
│    repl · complete · highlight · magic · introspect · serve(attach)          │
├─ Native Runtime ─────────────────────────────────────────────────────────────┤
│  C kernel (thin): signals · ms clock · isatty · output sink ·                │
│    replxx bridge                                                             │
│    luna_main entry/CLI · luna_kernel state/binding registration ·            │
│    luna_line                                                                 │
│    luna_kernel counting hook · luna_loop libuv wiring · luna_toml            │
│    tomlc17 bridge                                                            │
│  a row of vendored libraries:                                                │
│    Lua 5.5 · libuv · OpenSSL · replxx · luasocket                            │
│    LPeg · lfs · lua-zlib · luaossl · tomlc17                                 │
│    libyaml + lyaml · expat + lua-expat · dkjson (pure Lua)                   │
│    argparse · scintillua · luarocks · lualogging                             │
│    (the Lua layer ships with the binary)                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Why the layers:**

- **User Space is a shell, not the content**: scripts use only standard interfaces (require, CLI, REPL, attach) and have no idea how the inside of Luna Runtime is arranged — a policy-layer refactor does not break user code;
- **Luna Runtime is the body**: modules / stdlib / plugins / package management and the REPL policy layer all live here, pure Lua plus wrapper layers, shipped in the binary, with source as documentation (`lua/` is directly readable), readable and replaceable by plugins;
- **The C kernel is kept as thin as possible**: the Native Runtime does only what C is good at and must be in C — signals (SIGINT), a millisecond clock, isatty, the output sink, the replxx bridge. The behavior logic of the REPL is all Lua, because logic changes often, and keeping it in the Lua layer means changing it requires no C knowledge;
- **Extension points are the boundary**: the four registration APIs (`magic.register`, `complete.add_source`, `highlight.set`, returning a `modules` table) are the plugins' only entrance. The internal structure of the policy layer is invisible to plugins, and a policy-layer refactor does not break plugins;
- **Official Lua is the kernel**: luna contains no self-made interpreter; every "language-layer" capability comes from official Lua 5.5 plus LPeg, and upgrading Lua touches only `deps/`.

## Relationship with Node.js: architectural reference, not identity

luna's alignment target is to give Lua scripts an engineering experience close to Node's — module resolution, stdlib shape, CLI entry, these **habits** are copied verbatim, because they are the forms the Lua ecosystem has agreed work well. But this is not identity: the identity is a Lua runtime, and the Node comparison only answers "why the experience is designed this way"; wherever Lua semantics are stronger (require caching, chunks and `...`, the official searcher), official behavior is kept. The per-module capability comparison is on [node-parity](/node-parity).

| Concept | Node.js | luna |
| --- | --- | --- |
| Entry | `node` / `node script.js` / `node -e` | `luna` / `luna script.lua` / `luna -e` |
| Dependency directory | `node_modules/` climbing up level by level | `luna_modules/` climbing up level by level |
| Relative require | relative to the current file | relative to the current chunk (`@` source anchor) |
| Package manifest | `package.json`'s `main` | the `main` of the same-named manifest, defaulting to `init.lua` |
| Package management | `npm install` + `package-lock.json` | `luna install` (a LuaRocks wrapper) + `luna.lock` (version + source sha256, offline reproduction via `--from-lock`) |
| Plugins | (n/a, rely on packages) | `plugins/` directory + `plugin.json`, shadowing in place |
| Global injection | `process`/`Buffer` | `kernel` (the REPL additionally has `In`/`Out` globals) |
| Stdlib | built-in | `json`/`fs`/`net`/`http`/`csv`/`ini`/`toml`/`yaml`/`xml`/`zlib`/`crypto`/`path`/`util`/`events`/`stream` ships with the binary (`loop` is the async face); the per-module one-to-one comparison with Node is on [node-parity](/node-parity) |

The differences are deliberate: Lua's require caching (`package.loaded`), chunks and `...`, and official searcher semantics all keep official behavior; luna only **inserts one link into the searcher sequence** and does not replace anything.

## Selection comparisons

The convention: candidates, the reason for selection (maintenance activity / API surface / binding quality), and who was eliminated and why.

### Line editing and history — replxx

| Candidate | Verdict |
| --- | --- |
| **replxx**(`undoing/replxx`) | **selected**: actively maintained, Unicode/wide CJK characters handled correctly, colored prompts, hints, history persistence and incremental search, a concise C API (the project provides the `luna_line.c` bridge) |
| linenoise (antirez) | the original plan, upgraded to replxx in batch 7: the single file is lovely, but wide/multibyte editing is weak and there are no colored prompts, with community fixes left unapplied for years |
| readline | both the GPL/LGPL licensing and the global state (one state per process for each new readline) make it unsuitable for an embedding host |
| lua-linenoise | wraps linenoise yet again, inheriting its weaknesses |

### Lexical highlighting — scintillua + LPeg

| Candidate | Verdict |
| --- | --- |
| **scintillua** (Mitchell / orbitalquark, Textadept ecosystem) | **selected**: a production-grade lexer set for dozens of languages, pure Lua + LPeg, embeddable standalone (`lexers/lua.lua` works as-is); swapping language = swapping a lexer file, naturally pluggable |
| hand-written regex highlighting | rejected: regex cannot correctly handle nested long strings `[[ ]]`, escapes inside strings, ambiguities such as `--` and `---@`; the cautionary tale is the highlight-bug lists of every editor |
| porting TextMate grammars by hand | rejected: an oniguruma dependency plus a JSON→Lua conversion layer buys complexity with no payoff |

### Command-line parsing — argparse (mpeterv)

| Candidate | Verdict |
| --- | --- |
| **argparse**(`mpeterv/argparse`) | **selected**: inspired by Python's argparse, declarative definition, automatic usage/help, subcommand support, pure Lua, thoroughly tested |
| docopt.lua | rejected: docopt's "the document is the parser" style is hard to control when help wording and parsing conflict, and implementation maturity varies |
| hand-written `arg[i]` branches | rejected: hand-writing the combination of `-i`/`--interactive`, optional positionals and `--no-*` switches is guaranteed to be wrong |

### JSON — dkjson

| Candidate | Verdict |
| --- | --- |
| **dkjson** (David Kolf, dkolf.de; LuaRocks account dhkolf) | **selected**: pure Lua, single file, zero dependencies, strict mode (rejects illegal input), correct big-integer handling, distributed stably on LuaRocks for years (no upstream git repo; published as src.rock, so provenance is recorded when vendoring) |
| lua-cjson | rejected: the C binding is 3–5× faster, but throughput is irrelevant in REPL scenarios, and the distribution/OpenResty forks proliferate with API drift between them |
| rxi/json.lua | rejected: a clean implementation but no strict mode, and its cyclic-reference behavior disagrees with dkjson's |

### zlib — lua-zlib (brimworks)

| Candidate | Verdict |
| --- | --- |
| **lua-zlib**(`brimworks/lua-zlib`) | **selected**: the longest-maintained, a streaming closure API (`zlib.deflate(level)(data, "finish")`) that supports incremental work; a thin layer adds one-shot `compress`/`decompress` |
| lua-lzlib | rejected: an API closer to zlib's prototypes but not following new Lua versions |
| hand-written FFI binding | rejected: depends on LuaJIT FFI, while luna uses official PUC Lua |

### crypto/TLS — luaossl (wahern)

| Candidate | Verdict |
| --- | --- |
| **luaossl**(`wahern/luaossl`) | **selected**: the most complete coverage (the full pkey/x509 family, ssl/store/rand/…, 26 submodules), tight to OpenSSL 1.1–3.x, registered per submodule (`_openssl.<sub>`), thoroughly documented |
| lua-openssl (zhaozg, common in the OpenResty ecosystem) | rejected: the API is a literal translation of OpenSSL's prototypes, and **note the attribution — it is not an official OpenResty product**, it merely ships in OpenResty distributions; the maintenance cadence depends on a single author |
| LuaSec | rejected: positioned as a socket-TLS wrapper, does not operate at the x509/pkey level |

### Package management — LuaRocks (thin wrapper)

| Candidate | Verdict |
| --- | --- |
| **wrapping LuaRocks v3.13.0** (source vendored into the binary, invoked in-process) | **selected**: the de facto standard for Lua packages listed in lua.org's official main repository, with thousands of rocks ready; luna writes only a thin wrapper over four subcommands plus the `.luna/rocks` tree plus `luna.lock` (rockspec sha required, `.src.rock` sha best-effort), with `--from-lock` reproducing offline with sha verification |
| writing a registry client of our own | rejected: repository/upload/signing/index/dependency-resolution are all long-term burdens, and not a single existing package could be used — violating "find a mature library, do not write your own" |
| porting the npm client | rejected: what you get is npm's shell (the format, the parserers), while the goods (the packages in the npm repository) are not Lua code; Lua packages still have to be published through LuaRocks, and two parallel package registries cost only |

### Binding layer — hand-written C luaopen bindings (no sol2)

How are C libraries hooked into Lua? luna's answer has always been the same: hand-written `luaopen_*` (the official C API), registered through `register_c_modules` into `package.loaded` (luna_main.c, glob off), with the pure Lua convenience layer in `lua/modules/*/init.lua`. All five binding families — lfs / luasocket / lua-zlib / luaossl / loop — follow this pattern: 50–300 lines per module, explicit type mapping, stack operations visible at a glance, error paths reviewable line by line — which is exactly where the format modules' "errors carry line/column" convention lands. The already-landed xml/yaml/toml (see [node-parity](/node-parity)) continue it.

| Candidate | Verdict |
| --- | --- |
| **hand-written C luaopen bindings** | **selected**: official Lua is the kernel (see the layering rationale above), and the binding layer keeps the official shape; no intermediate abstraction can obscure stack semantics; each binding's error convention can be checked line by line |
| sol2 (`ThePhD/sol2`) | rejected (evaluated 2026-09-30): the latest release v3.3.0 (2025-06) **does not support Lua 5.5** — 5.5 support is stuck on unmerged PR #1723 (since 2025-07), with zero response from the maintainer in the thread, downstream commentary reading "increasingly unmaintained", and projects such as osrm-backend each carrying their own patch set; integrating it means vendoring an unmerged patch set and internalizing upstream maintenance-stop risk into a single binary; and a header-only C++17 template library runs against the "C kernel as thin as possible" layering |
| LuaBridge / lua-wrapper and other template bindings | rejected: the same bet as sol2 (C++ templates chasing official VM evolution), with a smaller ecosystem and smaller pool of pitfall knowledge; the "thinness" of hand-written bindings is exactly why the existing five binding families have had zero incidents |

Also record the Lua version ledger: deps/lua pins **v5.5.1** — the newest 5.5 release tag upstream (master holds 6 more small fixes beyond the release tag, such as a GC parameter UB, none of which made it into a release; the version-lock discipline awaits 5.5.2 and does not chase master). "Integrate official Lua directly, without a binding framework" is therefore not a new decision but an already-confirmed status quo being reconfirmed.

### The rest

- **Lua 5.5**: official source, no LuaJIT (5.5 semantics + maintenance first);
- **LPeg**: a dependency of scintillua and the runtime of future lexer plugins;
- **luafilesystem** (keplerproject) and **luasocket** (Diego Nehab): the de facto standards of their respective domains, not re-selected;
- **libuv**: already in deps/ and wired as the `loop` module (1.53.0, statically linked) — see the next section.

## Kernel/plugin boundary and loading order

**Boundary**: the loader (`luna.plugins`) only discovers, sorts in place order, executes the entry points under `pcall`, injects `modules`, and produces three reports (`loaded`/`failed`/`overridden`); plugins wire themselves up only through the extension-point APIs. Neither side sees inside the other.

**Loading order** (loaded for every startup mode; `--no-plugins` skips):

1. `./plugins` (project) → 2. `~/.luna/plugins` (user) → 3. each entry of `$LUNA_PLUGIN_PATH` in turn;
2. within one directory, lexicographic order; manifest names **first come, first served**, the later arrival is recorded as overridden (project shadows user);
3. entry points execute under `pcall` — **failure isolation**: one plugin throwing or a corrupt manifest is recorded into `plugins.failed` only, sibling plugins and luna itself keep starting; completion sources and highlight functions are equally pcall-protected.

**Why "first come, first served by name + no fallback on failure"**: shadowing semantics must be predictable — a user seeing in `%plugins` that the project version already holds the name is easier to diagnose than "quietly using the user version"; a fallback would bury a failure inside a success.

## Event loop: the first batch (an explicit choice, opted in by scripts)

Half of what makes Node "Node" is the event loop. luna now has its first piece: **`require "loop"`** — deps/libuv (1.53.0) wired as a Node-named API (setTimeout/setInterval/setImmediate/clear*/run/stop), statically linked into luna's single binary. The synchronous stdlib is unaffected and the REPL unchanged; whoever requires it enters the async world. The reasons for the earlier postponement still hold, and they define this batch's boundary:

1. **The semantic conflict is undecided**: mixing the synchronous stdlib with callback-style async in one session, and the yield point (which call yields) remains a language-design problem — so the loop drives only user-registered callbacks and the stdlib is not touched one byte;
2. **The REPL does not need it**: the interactive main loop is naturally synchronous; the loop is an explicit choice of script mode, with no global setTimeout to pollute synchronous code;
3. **Two contracts ride along**: the loop's only own handle is a prepare hook (running while live handles exist, stopping when the last one closes, itself unref'd — it does not keep the loop alive; the keep-alive ledger counts only user handles), and each round it does two things — turn the `^C` that landed during the blocking `uv_run` into an `interrupted` error (the exit code 130 contract unchanged), and poll the attach socket. Processes inside the event loop remain observable through `--attach` as usual: the polling-point model holds as-is, and libuv's existence adds no second source of concurrency.

The keep-alive semantics align with libuv: every callback handle is held by the registry until its `uv_close` completion callback lands (`uv_close` is asynchronous, and the handle's memory must outlive it); when the last handle closes, the hook stops and an idle `run("default")` returns — consistent with Node's "exit when the event loop drains". On the same batch came `loop.fs`: readFile/writeFile/stat running on the libuv thread pool, callbacks landing back on the loop thread, errors in the Node "callback first argument" style; the directory surface was later completed (appendFile/readdir/mkdir/rmdir/unlink/rename, the read side still the same contract); another batch added `loop.net`: client and server sockets for TCP and unix domain (connect/connectPipe/listen/listenPipe plus sock and server handles), EOF delivered as `(nil, nil)`, the same callback contract, with `sock:peer/sockname` and `server:address` providing the address surface (inet/inet6/unix families, TLS and UDP faces in the same style, the TLS end hand-rolled fd direct from the kernel); `loop.udp` added the datagram face (bind receives packets resident, send fires a per-packet callback, bind conflicts throw synchronously); `fs.watch` added the watching face — a resident event callback for directory changes (wired via inotify); `loop.signal` wired Unix signals into the same contract (SIGTERM graceful exit is the home turf; SIGINT/SIGUSR1 each have their owners and refuse registration); `loop.dns` factored the thread-pool resolver used internally by net.connect into its own face (lookup/reverse, a getaddrinfo straight-through); `net.connectTls` and `net.listenTls` stacked both TLS ends onto the net face — an OpenSSL state machine driven by uv_poll over a hand-rolled fd (handshake, WANT_READ/WRITE yielding, a pending write buffer, close_notify as EOF), certificate verification on by default, opts able to swap the CA or skip it, the server side handing only handshake-completed connections to onConn. `loop.http` is the first pure Lua face — an HTTP/1.1 client running on net.connect/connectTls, one aggregate callback per request, the body framed by content-length, chunked or EOF, and the redirect chain and whole-request timeout closed into state machines in Lua as well, without touching the C kernel one line; the server face is equally in Lua — `http.listen` takes over net's server, the handler fires only once the whole request has landed, res.send's three overloads write a complete response and then close, a handler throwing yields a 500; large responses need not enter memory whole — onHead/onData turn the body into streaming observation, with the content-length and unframed paths truly incremental and the framing decision unchanged. `loop.os` is the only synchronous face — host name, network interfaces, CPU, memory and such system information read and returned directly, not entering the loop, classified under loop only because it comes from the same libuv (later completed with the five Node os keys arch/release/EOL/userInfo/availableParallelism, arch mapping uname's `x86_64`/`aarch64` to Node's `x64`/`arm64`). The newest batch adds `loop.process`: aggregate execution of child processes (spawn without a shell, stdout/stderr capture, `proc:kill`), the delivery condition being "exit + both pipes at EOF", spawn failures throwing synchronously like listen; two sugars were stacked on later — `exec` is a literal straight-through of `run("sh", {"-c", cmd})` (shell semantics) and `execSync` is the popen synchronous face (read stdout to the end, a non-zero exit code raises); `process.spawn` then offers the streaming opposite — the three stdio streams handed over directly as ordinary `loop.net` socks, the exit callback aligned with Node's `'exit'` (not waiting for the streams to drain), and half-close via `sock:shutdown`. The whole handle family's `unref`/`ref` puts the keep-alive ledger in the user's hands — handles run as usual, they just can be "present but not keeping anyone around", and background periodic tasks and bystanders no longer tie the script's lifetime. There is also an automatic drain at the end of a script — once the main block of `luna script.lua`/`luna -e` ends normally, handles still holding up the loop are automatically run to empty and then exit (Node's "event loop runs to empty" contract); a main block erroring is not drained, the REPL is not drained, and a `^C` landing in the drain segment still yields 130.

**The underlying backend selection** (IOCP free of charge, and why io_uring is not planned but gained for free on libuv upgrade) is discussed in its own document: [event loop backend design](/loop-backend-design).

Get modules, plugins, highlighting and introspection solid first — once the runtime's "bones" have grown properly, the event loop can grow in the right place.

## attach: a polling point, not an event loop

`luna --attach` lets one process observe another process's live state, and it has to still answer while the target is busy with arbitrary Lua code (even an infinite loop) or blocked in the line editor — this is essentially a concurrency problem, and its implementation deliberately **does not introduce** an event loop:

- **Two polling points**. The target process exposes a non-blocking `serve.step()` in `lua/luna/serve.lua` (accept → read a line → pcall-evaluate → frame a reply). Only two places call it: the idle gaps of the REPL's main loop, and one opportunistic call inside the C counting hook every 100,000 instructions during script execution (the hook is suspended during polling and restored afterwards, and a nested `pcall` guarantees that a polling failure does not hurt user code). No new thread enters Lua, no epoll, the semantics stay purely synchronous;
- **The signal is the doorbell**. An idle target blocked in replxx's line editing — replxx swallows EINTR internally and the same-thread notify is deliberately ignored on top — so `luna_line.c` has a dedicated helper thread: the SIGUSR1 handler only writes one byte to a pipe (async-signal-safe), and the thread, on waking, synthesizes a `KEY_ENTER` into replxx (it must be a key code with the control modifier; a bare `\r` only rings a bell), the blocked `input()` returns an empty line, and the main loop returns to the polling point;
- **The protocol is one line readable**: a request is one line of text, and a reply is the text frame `"OK\n"/"ERR\n"/"EXIT\n" + body + "\30\n"` (`EXIT` only detaches the client and does not kill the target), the newline after `\30` keeps the line reader from dangling; during command execution the kernel output is captured into the frame and mirrored to the target's terminal at the same time. `%` magic, the `?expr` help sugar, and the `\1complete` completion meta-line all reuse the target side's own modules (`luna.magic`, `luna.introspect`, `luna.complete`) — serve is only a bridge, not a second REPL implementation; client errors go to stderr, keeping stdout a clean result stream;
- **The boundary of observability**: attach arrives only at **polling points**, so it sees the state of the target "in the gaps between its breaths", not the state at an arbitrary instruction boundary — which is exactly what distinguishes it from a debugger and from a future event loop.

This is another instance of "mature pieces first": the counting hook (SIGINT already had one), replxx's synthetic key API, and luasocket's unix transport are all existing building blocks; the only new code is a few hundred lines of Lua bridging. The accompanying process policy: `SIGPIPE` is explicitly ignored (writes to a vanished peer surface as EPIPE errors, which `pcall` catches; the signal itself is what `pcall` cannot catch), and the attach socket is wrapped in umask plus bind and chmod double protection, readable and writable only by its creator.
