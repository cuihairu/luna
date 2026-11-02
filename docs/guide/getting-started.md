# Getting started

## Get luna

Choose one of three paths:

**One-line install** (recommended: installs the latest daily build into `PATH`, verifies with `luna --version`, and upgrades when rerun):

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex
```

The artifacts come from a **rolling nightly Release** (fixed tag [`nightly`](https://github.com/cuihairu/luna/releases/tag/nightly), republished after each green run): public repository assets are anonymously downloadable **without credentials**. Each zip has a `.sha256` sidecar and the script verifies it after download; a mismatch is a hard failure (the artifact is corrupt, or the nightly was republished while downloading — re-run to retry). `--token`/`GITHUB_TOKEN`/`LUNA_INSTALL_MIRROR`/`LUNA_MIRROR` remain available for private forks and direct-link fallbacks.

**Manual download**: use the [nightly Release page](https://github.com/cuihairu/luna/releases/tag/nightly) to download **`luna-nightly-<os>-<arch>.zip`** (`luna-nightly-linux-x86_64` / `luna-nightly-linux-aarch64` / `luna-nightly-macos-aarch64` / `luna-nightly-windows-x86_64`) plus its `.sha256` sidecar. The archive contains the binary and the `luna_modules/` policy-layer sidecar (the binary resolves the Lua policy layer next to itself, so the extracted tree is self-contained). All four platform legs are production-ready (Windows on 2026-10-08, macOS on 2026-10-09): the daily test matrix (`daily.yml`) is green-gated per platform, while the delivery workflow keeps the Windows leg on `continue-on-error` so a red Windows build does not hold back the rolling nightly Release. The Daily Build workflow ([nightly.yml](https://github.com/cuihairu/luna/actions/workflows/nightly.yml)) produces these assets; per-run artifacts are local copies only, while Release is the delivery channel.

Verify after extraction:

```bash
./luna -e '6*7'      # should print Out[1]: 42
```

macOS packages are unsigned; run `xattr -cr luna` once to clear quarantine.

**Build from source**: the only dependencies are CMake ≥ 3.14 and a C compiler. Everything else (Lua 5.5, LPeg, replxx, scintillua, luasocket, luafilesystem, lua-zlib, luaossl, dkjson, argparse, libuv, tomlc17, libyaml+lyaml, expat+lua-expat, lualogging, and so on) lives under `deps/` and is fetched during configure.

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build          # all test groups should be green
```

On macOS also run `brew install pkg-config autoconf cmake`. See [Build and self-test](/other/build) for the test groups and coverage-instrumented tree.

Optional: install OpenSSL development headers (`libssl-dev`) and reconfigure to enable the crypto module (`sha256`, `hmac`, random bytes, and so on) beyond zlib. Without OpenSSL the build still succeeds and `require("crypto")` returns a guided error.

**Native Windows support**: all four platform legs are production-ready (2026-10-08), nightly ships assets for all four platforms, and `install.ps1` is a one-line install — see [FAQ](/other/faq).

## Thirty seconds

```bash
luna                          # 1. interactive console (like node)
luna hello.lua                # 2. run a script (like lua)
luna serve ./docs 8000        # 3. serve a directory instantly (like python -m http.server)
```

Step 3 needs no script: open `http://localhost:8000` for a directory listing or `index.html`. An API server is also one line:

```bash
luna -e 'require("http").serve(function(req, res)
  res:json({ hello = "luna", path = req.path }) end)'
```

```bash
$ curl http://localhost:8000/api?name=cui
{"hello":"luna","path":"/api"}          # key order follows the runtime
^C                                       # stop with ^C; exit code 130
```

TCP echo is one line too (in another terminal, `echo hi | nc 127.0.0.1 9000` prints `echo: hi`):

```bash
luna -e 'require("net").serve("*", 9000, function(c)
  c:send("echo: " .. (c:receive("*l") or "") .. "\n") end)'
```

See [Standard library · http](/stdlib/http) and [Standard library · net](/stdlib/net) for details.

## Startup modes

```bash
luna                     # interactive console (like node)
luna script.lua a b      # run a script and exit; a b become the script's `...` (like lua)
luna -i script.lua       # run a script, then enter the console
luna -e 'print(6 * 7)'   # evaluate and exit (like node -e); expressions echo as Out[n]
luna serve [dir] [port]  # static file server; stop with ^C (see "Thirty seconds")
```

Common switches:

| Switch | Effect |
| --- | --- |
| `--no-color` | Disable ANSI output coloring |
| `--no-plugins` | Skip plugin discovery and loading |
| `--no-serve` | Do not create the attach listener socket |
| `--help` | Usage and examples |

See [Configuration and environment variables](/other/config) for every switch and environment variable.

Color policy: enabled automatically on a TTY that is not `dumb`; `NO_COLOR` forces it off and `LUNA_COLOR=1` forces it on; pipes fall back to plain text, so script output is never polluted.

## First REPL session

```lua
In [1]: 6 * 7
Out[1]: 42

In [2]: os.time()                       ← Tab completion: os.<TAB>
In [3]: ?string.format                  ← ? help sugar
string.format(fmt, ...) — formats per Lua printf rules

In [4]: %whos                           ← list current globals
In [5]: %exit                           ← or ^D
```

`In[n]`/`Out[n]` mirror IPython: every non-empty result is registered in `Out[n]` and also stored in `_` and `__`; `%hist` shows all inputs.

## Use in a project

```
myapp/
├── luna_modules/
│   └── hello/            ← a package: manifest + entry point
│       ├── package.json  {"name":"hello","main":"init.lua"}
│       └── init.lua      return { greet = function() return "hi" end }
└── app/
    └── main.lua          require("hello").greet()
```

```bash
luna app/main.lua        # bare name hello is resolved while climbing to luna_modules/
```

See [Module system](/guide/modules) for details, and [Plugin development](/guide/plugins) for adding project plugins such as custom magic commands.
