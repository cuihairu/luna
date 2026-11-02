# Configuration and environment variables

luna has no configuration file: every behavior switch is controlled through **environment variables** and **command-line switches**, with defaults that work out of the box and every item individually overridable.

## Environment variables

| Variable | Effect | Default |
| --- | --- | --- |
| `NO_COLOR` / `LUNA_NO_COLOR` | Turns off all ANSI coloring (input line and echoed results move together, see [CLI and REPL](/guide/cli-repl)) | unset |
| `LUNA_COLOR` | `1` forces color on, `0` forces off; lower priority than the off variables above | unset |
| `TERM` | `dumb` automatically downgrades to plain text | — |
| `HOME` | Determines the history file `~/.luna_history` and the user plugin directory `~/.luna/plugins`; when empty, history is not written and the user directory is skipped | — |
| `LUNA_PLUGIN_PATH` | Colon-separated extra plugin directories, participating in discovery in order, with priority after `./plugins` and `~/.luna/plugins` | unset |
| `LUNA_SOCK_DIR` | Directory for the attach listen socket (`luna-<pid>.sock`, 0600) | `/tmp` |
| `LUNA_VENDOR_DIR` | Override for the embedded LuaRocks vendor directory (normally untouched, fixed at build time) | build-time value |
| `LUNA_COVERAGE` | Only effective in Profiling coverage builds: when set, embedded policy modules load under their real filenames so luacov can count | unset |
| `PWD` | `path`'s fallback when the process cwd is unavailable | passed through by the shell |

`LUNA_*` variables have no dependency relationship, each takes effect independently; `NO_COLOR` is named the same as [no-color.org](https://no-color.org) — **presence means off**, an empty value still counts as set; `LUNA_COLOR` needs a non-empty value to take effect, `0` off, any other value on. The variables consumed by the install scripts themselves (`LUNA_INSTALL_MIRROR` / `LUNA_MIRROR`, `GITHUB_TOKEN`) are one-time install-time inputs, not the binary's business; see [Getting started](/guide/getting-started).

## Command-line switches

| Switch | Effect |
| --- | --- |
| `-i --interactive` | After the script runs (or without one), drops into the interactive console; the script's globals remain (modeled on `python -i`) |
| `-e --eval 'code'` | Evaluates and exits; expression results echo as `Out[n]`, incomplete code errors (modeled on `node -e`) |
| `-v --version` | Prints the version and exits (the script installer's post-install check uses it); loads no plugins and opens no attach socket |
| `--no-color` | Disables coloring for this session (same effect as `NO_COLOR`, session-only scope) |
| `--no-plugins` | Skips the entire plugin discovery process; for "behavior changed after installing a plugin," reproduce with this first |
| `--no-serve` | Does not create the attach listen socket (the target side's accept capability is closed; the client itself never creates one) |
| `--attach <pid>` | Attaches to another running luna process, see the attach section of [CLI and REPL](/guide/cli-repl) |
| `--help` | Usage and examples |

Subcommands have their own argument surfaces, not in this table: `luna serve [dir] [port]` (dir/port order-independent) is in [stdlib · http](/stdlib/http); `luna ps` is a read-only process view; `luna install/search/list/update` pass through the embedded LuaRocks' own flags (`--tree`, `--from-lock`…) — see [Package management](/guide/rocks). The full startup-mode list is in [CLI and REPL](/guide/cli-repl).

## Files on disk

| Path | Contents |
| --- | --- |
| `~/.luna_history` | Input history (with timestamp separators), recalled with `↑` after restart; written only on TTY with `HOME` available |
| `$LUNA_SOCK_DIR/luna-<pid>.sock` | The attach listener, 0600, cleaned up automatically at process exit |
| `<project root>/.luna/rocks/` | The `luna install` dependency tree (LuaRocks `--tree`), searched upward level by level |
| `<project root>/luna.lock` | Dependency snapshot: per-rock versions + source archive sha256, which `--from-lock` uses to reproduce |
| `<project root>/.luna/cache/` | Cache of downloaded `.src.rock` source archives (for offline reproduction) |

The last three appear only when using package-management subcommands such as `luna install`, and are **written only inside the project**. Beyond that luna writes no global state on the user's machine: no global cache directory, no first-run wizard, no telemetry.

