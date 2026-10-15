[English](loop-backend-design.en.md) | [中文](loop-backend-design.md)

# Event loop backend design: libuv, io_uring, and where the boundaries of a self-built backend sit

This is a **discussion-only document** (documentation before implementation, no code attached), answering one question: is stopping at libuv enough for luna's async IO foundation? Should io_uring be integrated now? Should Windows's IOCP be touched by hand?

**Conclusion up front**: neither platform mechanism has to be written by hand. Windows's IOCP — libuv already rides on it underneath, so it comes for free; io_uring likewise does not have to be written by hand: libuv upstream is integrating it incrementally, and luna's only obligation is to keep its libuv integration solid (pinned version, full regression on upgrade); once it matures, upgrading gets it for free. luna does not plan a liburing binding, does not build a second backend, and does not pre-build an abstraction layer for a hypothetical second backend. Every judgement below comes with its reasons.

## 1. Current state: libuv as the single backend, all three platforms covered in one shot

luna already has an opt-in event loop: the `loop` module exposes the Node-named timer/immediate APIs plus `run`/`stop`, and `loop.fs` then puts `readFile`/`writeFile`/`stat` onto the libuv thread pool, with callbacks landing back on the loop thread. The pinned libuv is 1.53.0 (`deps/libuv`, statically linked, single binary unchanged; the 2026-09 upgrade exercise from 1.44.2 — zero changes in product code, the single test assertion relaxed to a range because 1.53 moved timers' relative ordering after check, full regression green).

The decisive fact is libuv's backend mapping: **IOCP on Windows, epoll on Linux, kqueue on macOS/BSD**, the same API dispatching automatically. This means:

- luna's async file and socket IO on Windows is already running on IOCP underneath — **"IOCP need not be written by hand" is not a trade-off conclusion, it is a finished fact**;
- a hand-written IOCP wrapper (registering overlapped IO, completion-port queuing, cancellation semantics, the zero-byte read trick of `WSARecv`) would re-step in every hole libuv has spent a decade and more filling, buying nothing but "one less abstraction" — and that abstraction is precisely what paying for libuv buys;
- **behavioral consistency** across three platforms is libuv's job (for instance `uv_fs_*` follows thread-pool semantics uniformly on every platform), while a hand-written wrapper would have to prove equivalence per platform.

So the verdict of this section is a single sentence: **libuv as the single backend already covers the event mechanisms of all three kernels, so any proposal of "adding another backend" must first answer what libuv is missing.**

## 2. io_uring: not integrated; wait for libuv to supply it

io_uring is Linux's native async IO interface since 5.1: userland and kernel share a pair of ring buffers, submission and completion are batched, and both theoretical throughput and syscall overhead beat epoll. It deserves a serious evaluation, but the conclusion is not "when do we integrate it" but "**we simply do not integrate it**" — libuv upstream has already boarded this path, and following it gets it for free. The reasons fall into three layers.

### 2.1 Integration status on libuv's mainline: incremental, conservative, ongoing

libuv's mainline introduced io_uring **on file IO** from 1.45.0 (replacing parts of the thread-pool path), and went back and forth on its default toggle after a sandbox-environment compatibility incident; in the pinned version 1.53.0 at the time of writing, file IO goes through the `uv__iou` channel (turnable off with `UV_USE_IO_URING`, unsupported kernels/operations fall back to the thread pool item by item), epoll batching has also been merged in, while real network completion semantics are still progressing upstream — incremental, one domain first, degrading per environment, sandbox fallback logic all built in. This chain of facts yields exactly one judgement:

- libuv's io_uring integration is **incremental and cautious** — one domain first, fallable, degrading per environment; precisely the engineering posture io_uring's reality (see 2.3) demands;
- the completion timing is decided by libuv's roadmap: luna **need not and cannot** maintain a parallel path of its own — two io_uring integrations would only turn "following upstream" into "maintaining a fork";
- hence luna's entire io_uring strategy collapses into one sentence: **keep the libuv integration solid, wait for the integration to mature, get it on upgrade.**

### 2.2 Benefit side: luna's load shape never reaches the bottleneck in the first place

io_uring's gains concentrate in the **massively many concurrent connections + high-frequency small IO** scenario — proxies, storage engines, message queues serving hundreds of thousands of requests per second. luna's positioning is a REPL and scripting runtime:

- `loop.fs` already moves file IO to the thread pool, so the main thread is never waiting on disk anyway;
- a typical script's concurrency is "a few dozen handles", where epoll's syscall overhead is microsecond-scale noise;
- the REPL interaction path (attach, completion) goes over unix socket + a counting hook, unrelated to the IO backend.

**With no profile data proving epoll is the bottleneck, integrating io_uring has no benefit-side justification — and once libuv's integration matures, even this premise need not be verified by ourselves: upgrade, run the full regression, and the result speaks for itself.**

### 2.3 Cost side: io_uring's entire complexity is left to libuv

- **Kernel barrier**: io_uring's capability drifts with kernel version (basic usability from 5.1; buffer registration, multishot, `IORING_OP_*` extensions scattered across 5.19/6.x) — feature detection and the degradation matrix are maintained by libuv, untouched by luna;
- **Sandbox prohibition**: many container environments disable io_uring via seccomp, and each failing call must fall back per API to epoll — that fallback logic also lives inside libuv, while luna's regression tests only verify "behavior unchanged after upgrade";
- **Implementation discipline**: if self-integrating io_uring were to happen, only liburing would be permitted and hand-written syscall wrappers forbidden — but the correct answer is that it never happens: io_uring's ABI evolves fast and its semantics are fine-grained; racing the kernel ABI is the libuv team's profession, not luna's.

## 3. Layered abstraction: the second backend is not planned, and the abstraction layer is withdrawn with it

The nine faces of `loop` facing Lua — **timer** (setTimeout/setInterval), **fs** (the full async file surface plus watch), **net** (TCP/unix plus both TLS ends), **process** (run/spawn/exec), **udp**, **dns**, **os** (synchronous host information), **signal** (^C already handled by the prepare hook), plus the pure Lua **http** face — continue to evolve as luna's own API, independent of backend choice: except for http, which sits on the net face, all sit on libuv.

The once-imagined "backend interface layer" (splitting the inside of `luna_loop.c` into a replaceable backend) had exactly one realistic motive: self-integrating io_uring. Since io_uring belongs to libuv, **this abstraction layer loses its justification and is not scheduled**. Paying abstraction cost for a nonexistent second backend is the same mistake as self-integrating io_uring, just with a smaller bill.

luna's "integration obligation" toward libuv therefore converges to three items, all already in existing practice:

1. **Pin the version**: the `deps/libuv` submodule pins a specific version (currently 1.53.0), so behavior changes are traceable;
2. **Regression on upgrade**: raising libuv goes through the ordinary dependency upgrade flow — the full ctest (including the loop/fs/net behavior nails) must be all green before landing;
3. **Use the public API only**: luna does not touch libuv internals, so upstream refactors do not hurt luna.

## 4. Three-backend comparison

| Dimension | epoll (via libuv) | IOCP (via libuv) | io_uring (via libuv) |
| --- | --- | --- | --- |
| Readiness/completion model | readiness notification, read once the fd is ready | completion notification, overlapped IO | completion notification, shared ring queue |
| luna's current state | in use on Linux, 1.53.0 | already in use on Windows (free) | file IO arrived with 1.53.0; the network face is still on upstream's road |
| Code to write | no | **no** | **no** — follow the libuv upgrade |
| Beneficiary scenario | ordinary concurrency, sufficient | same | massively many concurrency / high-frequency small IO (luna has no such shape yet) |
| Kernel/system barrier | all Linux versions | all Windows versions | degradation matrix maintained by libuv |
| Sandbox compatibility | unimpeded | unimpeded | fallback logic maintained by libuv |
| Maintained by | libuv upstream | libuv upstream | libuv upstream |
| Verdict | **keep** | **keep, zero action** | **not planned, free on upgrade** |

## 5. Conclusion and roadmap

1. **Phase one (now)**: make libuv as the single backend solid — the four faces of `loop` (timer/fs/net/signal) keep growing on libuv as the only foundation, semantics nailed by tests.
2. **io_uring: not planned.** No liburing binding, no second backend, no pre-built abstraction layer for it. How far libuv's integration has progressed, luna reaches by the same distance through **upgrading libuv + running the full regression** — the only strategy, and a zero-cost one.
3. **IOCP**: not on the list. It is already inside libuv; that is the conclusion.
