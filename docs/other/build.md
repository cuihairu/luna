# Building and self-testing

## Dependencies

Only **CMake ≥ 3.14** and a **C compiler** (gcc/clang) are needed. Apart from cmocka (fetched via FetchContent, test tree only), everything lives in `deps/`:

- Lua 5.5
- LPeg
- replxx (line editing)
- scintillua (highlighting)
- luasocket
- lua-zlib
- luafilesystem
- luaossl (needs system OpenSSL dev headers)
- dkjson
- lualogging (the logging module; `logging` core + appenders staged with the binary)
- argparse
- tomlc17 (TOML v1.1)
- lyaml + libyaml (YAML 1.1)
- lua-expat + expat (XML)
- libuv (event loop backend)
- LuaRocks (embedded source, used by `luna install` etc.)
- luacov (coverage, Profiling tree only)
- cmocka (test framework, fetched via FetchContent)

The first configure needs the network (pulls git submodules and cmocka); afterward offline builds work — **no manual installation of any LuaRocks or system Lua package** is needed. (`deps/` also holds `base64/` and `jemalloc/` vendored directories, not compiled in the current build, kept as spares.)

## Build steps

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build          # every test group should be green
```

**Windows source builds are supported the same way**: Windows builds and runs the full ctest on the push-triggered `cmake.yml`; the two daily pipelines build and smoke on `windows-latest` but do not run the full ctest — the verification face's `daily.yml` Windows leg stops at Build/Smoke (the `Test` step is `if: runner.os != 'Windows'`), and the delivery face's `nightly.yml` passes `-DENABLE_UNIT_TESTS=OFF` on all platforms (tests off by design). Dependency assembly uses the vcpkg static triplet (`-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake`, zlib and openssl), no brew; installation via `install.ps1`. The previously recorded "Windows not in the support face" (replxx bridge, unix domain socket path) has been digested with the compile-time blockers; the current state is in [FAQ](/other/faq).

### Optional: enable the crypto module

After installing the OpenSSL dev headers, reconfigure:

```bash
# Debian/Ubuntu
apt-get install libssl-dev
# Fedora/RHEL
dnf install openssl-devel
# macOS (Homebrew)
brew install openssl
```

After reconfiguring, `require("crypto")` is available (`sha256` family, `hmac`, `randombytes`). Without it the build succeeds as usual, and the runtime gives an instructive error.

### macOS specifics

```bash
brew install pkg-config autoconf cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The bundle's `PLATFORM-NOTES.txt` has the same notes. First run needs `xattr -cr luna` to clear quarantine.

## Test groups

`ctest --test-dir build` runs sixteen groups:

| Group | Coverage |
| --- | --- |
| `repl` | REPL sessions, multi-line continuation, history, coloring |
| `cli` | Startup modes, argument parsing, `-e`/script/serve/attach |
| `complete` | Tab completion, chained expressions, plugin completion sources |
| `introspect` | `?expr`/`expr?`, `%whos`, value descriptions |
| `highlight` | Lexer, color scheme, `luna.highlight` extension point |
| `magic` | Built-in magic commands, plugin-registered commands, `%plugins` |
| `modules` | require resolution, luna_modules upward search, package manifest, `package.loaded` |
| `plugins` | Plugin discovery/priority/failure isolation, four extension-point registrations |
| `luna` | Entry smoke: no-arg start, stdin EOF clean exit |
| `serve` | `http.serve` static/handler dual mode, `luna serve`, `^C` contract, close drain |
| `loop` | Timers, TCP/Unix/TLS, async fs, signals, subprocesses, HTTP client/server |
| `rocks` | Package management (install/update/search/list + lock-file reproduction) |
| `line` | Line editor basics, cursor, kill-word |
| `linedit` | replxx binding, incremental search, history persistence |
| `main` | Entry dispatch, signal installation, module embedding, startup flow |
| `covsum` | Coverage summary targets (`gcovr_summary`, `lua_coverage`) |

### Run a single group

```bash
ctest --test-dir build -R serve     # only the serve group
ctest --test-dir build -E loop      # all groups except loop
```

### Serial and parallel

`ctest` defaults to **serial**; day-to-day you can speed up with `ctest --test-dir build -j"$(nproc)"`. Coverage measurement (`build-cov`) must be serial:

```bash
ctest --test-dir build-cov -j1      # stats-file merging assumes no concurrent writes
```

## Coverage measurement

Both trees each being green is a pre-commit condition; coverage uses a separate Profiling-instrumented tree, running the same tests verbatim:

```bash
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Profiling \
      -DCMAKE_C_FLAGS_PROFILING="-O0 -g --coverage" \
      -DCMAKE_CXX_FLAGS_PROFILING="-O0 -g --coverage" \
      -DCMAKE_EXE_LINKER_FLAGS_PROFILING="--coverage" \
      -DCMAKE_SHARED_LINKER_FLAGS_PROFILING="--coverage"
cmake --build build-cov -j
ctest --test-dir build-cov           # serial
cmake --build build-cov --target gcovr_summary    # C side (src/)
cmake --build build-cov --target lua_coverage     # Lua side (lua/)
```

### C side

Counted by gcov/gcovr. A process killed by SIGTERM does not write `.gcda`, so test fixtures all end by normal exit.

### Lua side

Counted by the embedded luacov wiring (active only in the Profiling tree): tests and the binary load embedded policy modules under their real filenames when `LUNA_COVERAGE=1`, merging line-level hits into `build-cov/luacov.stats.out`, and the `lua_coverage` target renders the report and prints the summary table to the build output. Normal builds and interactive behavior are unaffected (the `'=(luna/x)'` chunk names in errors stay verbatim).

### Merge-hook division of labor

Each Lua state has only one debug hook slot; after coverage takes over the slot it dispatches by event — line events count for luacov, and both line and count events forward one step to `kernel.count_hook()` (`^C` interrupt check and attach poll). The forwarding deliberately does not select event kinds: luacov's per-line accounting itself consumes count budget, so in a tight loop count events may land only inside the hook frame and never reach user code, while a line event fires every iteration — driving from it gives a landing point that does not depend on the instruction budget.

## Local docs preview

```bash
cd docs
pnpm install           # first time
pnpm run docs:dev      # http://localhost:5173/luna/
pnpm run docs:build    # output in .vitepress/dist/
```

VitePress 2.0-alpha, base `/luna/`, Chinese local search.

## CI: two daily pipelines (daily gate + nightly delivery)

**The two-pipeline decision (re-evaluated 2026-10-14): the two pipelines have complementary roles and neither is retired.** `daily.yml` is the verification face (full tests, red pulls the alarm), `nightly.yml` is the delivery face (packaging, rolling Release) — retiring the gate leg loses the red alarm, retiring the delivery leg loses the only delivery channel. The previously open item "whether the old pipeline is retired" closes with this conclusion; the re-evaluation was triggered by the Windows leg going green (2026-10-08, `12c90bd`) leaving the delivery face's docs and echo text unsynchronized: Windows assets were actually being delivered (verified 2026-10-13, below), while the text still said "no artifacts yet."

### daily.yml — verification gate

- `schedule`: daily at UTC 21:13 (Beijing time next day 05:13, avoiding the top-of-hour runner peak); `workflow_dispatch` for manual triggers;
- Four-platform test matrix, each leg full ctest, `fail-fast: false`; all four legs green (Windows 2026-10-08, macOS 2026-10-09), no trial-run exemption legs — a red build or test pulls the night's alarm directly;
- No tags, no Release; the per-run artifact `luna-nightly-<os>-<arch>` is kept 14 days, only as evidence, not delivery.

### nightly.yml — delivery face

- `schedule`: daily at UTC 01:23 (Beijing time 09:23); `workflow_dispatch` for manual triggers;
- Four-platform parallel builds: linux-x86_64 / linux-aarch64 / macos-aarch64 stable legs + windows-x86_64 (`continue-on-error` — a single leg's build failure does not freeze all-platform delivery; the red alarm is the verification face's job; the Windows leg is delivering assets, verified 2026-10-13);
- The delivery face is a **rolling nightly Release**: the collect & publish job creates/moves the fixed tag `nightly` (pre-release), clears old and uploads the new platform zips + `.sha256` sidecars with `BUILD_INFO.txt` / `SHA256SUMS.txt` / `VERIFY.md`, and rerun is idempotent; Release notes are the commit and time plus verification steps;
- The run-page artifacts come in two tiers: `pkg-<platform>` kept 3 days (build-leg products), `daily-build` kept 14 days (a same-named copy after collection, deleting prior same-name before upload) — both are only run-page copies, not delivery.

Pickup is anonymous from the Release page: `releases/download/nightly/luna-nightly-<os>-<arch>.zip` (the installer's download coordinate); run-page artifacts are only evidence.

## After pushing main

CI automatically builds and publishes the docs site to GitHub Pages (deployment does not count as a release).

