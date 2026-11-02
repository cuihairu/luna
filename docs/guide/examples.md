# Examples and scaffolding

There are two sets of starting assets: the eight official examples in `examples/` in the repository, and the built-in `luna new` templates. Examples ship with the repo; templates ship with the binary — on a new machine you can start from either without installing anything else.

## Official examples

Each example is one directory with a unified `main.lua` entry point; every output below is a real capture. One-shot processes exit when done; long-running ones stop on `^C` (exit code 130).

| Example | Demonstrates | Run |
| --- | --- | --- |
| [hello-cli](https://github.com/cuihairu/luna/tree/main/examples/hello-cli) | argparse options/flags/`-h` usage page | `luna examples/hello-cli/main.lua -- -n luna a b` |
| [http-server](https://github.com/cuihairu/luna/tree/main/examples/http-server) | `http.serve` programmable service: routing, `res:json`, query, 500 contract | `luna examples/http-server/main.lua [port]`, then curl from another terminal |
| [tcp-server](https://github.com/cuihairu/luna/tree/main/examples/tcp-server) | `net.serve` line-protocol service: handler called once per connection, errors do not topple the server | `luna examples/tcp-server/main.lua [port]`, then nc from another terminal |
| [file-tool](https://github.com/cuihairu/luna/tree/main/examples/file-tool) | Synchronous `fs` read + table output, `wc` shape | `luna examples/file-tool/main.lua <file>…` |
| [web-scraper](https://github.com/cuihairu/luna/tree/main/examples/web-scraper) | `http.request` fetches a page + pattern matching to pull `<title>` and links (http:// only) | `luna examples/web-scraper/main.lua http://…` |
| [build-tool](https://github.com/cuihairu/luna/tree/main/examples/build-tool) | Staged data build: data/*.csv → dist/*.json, with timing and single-stage runs | From `examples/build-tool/`, run `luna main.lua` |
| [game-script](https://github.com/cuihairu/luna/tree/main/examples/game-script) | Game logic as pure data tables: room/item/verb tables + json save | Pipe verbs in; see below |
| [automation](https://github.com/cuihairu/luna/tree/main/examples/automation) | `fs.watch` directory monitoring + `loop.run()` residency | `luna examples/automation/main.lua <dir>` |

One session of hello-cli — the script's flags go after `--` (the `luna` entry point consumes its own flags first), and `extra` holds positional arguments:

```text
$ luna examples/hello-cli/main.lua -- -n luna a b
hello-cli: greeting issued
hello, luna, a b!
```

file-tool multi-file summary; the `total` line appears from two files onward:

```text
$ luna examples/file-tool/main.lua README.md todo.md
    105     541    7791  README.md
   1961    8939  140536  todo.md
   2066    9480  148327  total
```

build-tool's three-stage pipeline; `compile` finishes and produces `dist/*.json` (`luna main.lua compile` runs just that stage):

```text
clean    ok  0.000s
compile  ok  0.001s
  orders.json      100 bytes
  users.json       107 bytes
report   ok  0.000s
```

game-script keeps state entirely in tables (`rooms`/`state`/`verbs` plus a single engine loop); stdin feeds verbs, saves go to json, and `load` reads the same state back:

```text
$ printf 'look\nnorth\ntake key\nsouth\nup\nsave\nquit\n' | luna examples/game-script/main.lua
luna game-script — 'help' lists verbs, 'quit' leaves.
You are in hall: a pillared hall. Dust, and one flickering lamp.
> You are in hall: a pillared hall. Dust, and one flickering lamp.
> You are in study: a cramped study. A desk, an open drawer.
here: key (a small brass key)
> taken: key
> You are in hall: a pillared hall. Dust, and one flickering lamp.
carrying: key
> You are in tower: the tower top. Wind, and the whole valley below.
carrying: key
> saved to luna-save.json
> bye.
$ cat luna-save.json
{"inv":["key"],"room":"tower"}
```

automation watches one directory; create/modify/rename/delete arrive as individual events (Linux connects directly to inotify), and `^C` ends with code 130:

```text
$ luna examples/automation/main.lua /tmp/watchme &
watching /tmp/watchme — touch files there; ^C stops (exit 130)
18:25:48  rename   new.txt
18:25:48  change   new.txt
^C
examples/automation/main.lua:30: interrupted
```

web-scraper is http:// only (the luasocket client face has no TLS; TLS clients go through [loop.http's connectTls](/zh/guide/loop#loopnettlsconnecttls-listentls)); offline walkthroughs pair with `luna serve <dir>` — when scraping a JSON API, `title` is `(none)` and `links` counts zero; a failed connection exits with code 1.

### The ^C contract in the three resident examples

http-server / tcp-server / automation all share the same "start a service, then wait for ^C" shape, backed by one loop contract: when a silent loop is blocked inside epoll, libuv retries EINTR internally without ending the iteration, and the prepare hook never sees the interrupt flag ([the `^C` entry on the loop page](/zh/guide/loop)) — the loop must keep turning. Each example picks one mechanism: http-server uses the built-in 100 ms heartbeat of `http.serve`; tcp-server's `net.serve` puts a 250 ms deadline on accept; automation installs its own 100 ms heartbeat. The examples are templates — copy them as-is.

## luna new: built-in scaffolding

```text
luna new <template> <name>    generate starter code from a built-in template
luna new --list               list templates
```

| Template | Generates | Next step |
| --- | --- | --- |
| `plugin` | `<name>/plugin.json` + `<name>/init.lua` (registers the `%<name>` magic command) | Move the directory into `./plugins/` (or `~/.luna/plugins`); takes effect on `luna` start |
| `package` | `<name>/package.json` + `<name>/init.lua` | Move into `<project>/luna_modules/`; then `require("<name>")` from anywhere |
| `script` | `<name>.lua` (arguments become the chunk's `varargs`; `-h` shows usage) | `luna <name>.lua world` |

Three hard rules: **reject if the target already exists** (`refusing to overwrite`, never overwriting a byte); names accept only letters/digits/`.`/`_`/`-` and reject `..` (the name lands in paths and JSON, so escaping is blocked first); a template without a name prints usage and the template list (exit code 1).

One complete session — generate, move into `plugins/`, verify with `%plugins`, fire the magic command:

```text
$ luna new plugin hello
created plugin 'hello':
  hello/plugin.json
  hello/init.lua
next: move hello/ into ./plugins/ (or ~/.luna/plugins), then run `luna` and try %hello
$ mkdir -p plugins && cp -r hello plugins/
$ luna
In [1]: %plugins
loaded plugins:
  hello 0.1.0  hello — a luna plugin
In [2]: %hello
hello from the hello plugin
```

Once the package template lands in `luna_modules/`, resolution rules are in [Module system](/zh/guide/modules); the plugin template's four extension points and failure isolation are in [Plugin development](/zh/guide/plugins). Templates are starter code — once generated, ownership of the content is yours.
