# Introduction

**luna** is a batteries-included general-purpose Lua runtime built for scripting, tooling, and lightweight services. It has two ambitions:

- **An IPython-style REPL**: multi-line input, live syntax highlighting, tab completion, introspection, magic commands, and an `In[n]`/`Out[n]` session history;
- **A Node.js-style engineering experience**: Node-like module resolution, standard-library shapes, directory plugins, and a package-management wrapper. Node alignment is an architectural reference, not an identity; see [Architecture design](/architecture).

The three-layer model: your code (User Space) runs on luna itself — the standard library, modules, plugins, and REPL policy layer (Luna Runtime) — over official Lua 5.5 and vendored libraries (Native Runtime).

Lua leaves everything to the host: no command-line argument parsing, no package management, and an interactive layer that is just `lua -`. luna fills in the host while keeping the kernel **official Lua 5.5**; luna contains no home-grown interpreter.

## What it looks like

```
$ luna
luna 0.1.0 — Lua 5.5 interactive console
Type ^D to exit.
In [1]: 6 * 7
Out[1]: 42
In [2]: function fib(n)
...   if n < 2 then return n end
...   return fib(n-1) + fib(n-2)
... end
In [3]: fib(10)
Out[2]: 55
In [4]: %time fib(20)
Wall time: 6.000 ms
```

Note that `Out` numbering is independent of `In`: `fib(10)` is evaluated at `In [3]`, but it is the second value produced in the session, so it is `Out[2]`; `%time` reports elapsed time and does not produce an `Out` value.

Input lines are highlighted live (using the scintillua lexer plus LPeg), incomplete blocks continue automatically, `^C` interrupts the current block and returns to the prompt, and `?expr` shows help.

## Three design rules

1. **Use mature libraries**: line editing, lexing, argument parsing, JSON, zlib, and TLS all come from well-maintained mature libraries. See the item-by-item comparison in [Architecture design](/architecture#selection-comparisons);
2. **Separate kernel and policy**: the C kernel provides only state and primitives. The REPL, completion, highlighting, and magic commands are all Lua source embedded in the binary, and plugins can replace any of them at runtime;
3. **The REPL is always the entry point**: bare `luna` enters the console, while `luna script.lua` and `luna -e` are its script forms — the same approach Node takes.

## Next steps

- [Getting started](/guide/getting-started): build and three ways to start luna;
- [CLI and REPL](/guide/cli-repl): magic commands and the session model;
- [Module system](/guide/modules): require resolution and manifests;
- [Plugin development](/guide/plugins): four extension points and failure isolation.
