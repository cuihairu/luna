// The static symbol snapshot and its lookups. The snapshot is
// GENERATED — scripts/dump_symbols.lua walks the real runtime's own
// introspection (luna.introspect + debug.getinfo over every module the
// runtime serves) and scripts/regen.sh stamps it into symbols.json.
// Never hand-write entries: a hand-maintained table fossilizes the
// moment the runtime moves.

export interface SymbolNode {
  kind: string; // "function" | "table" | ...
  sig?: string; // introspect.signature: "function(2 params)"
  at?: string; // "luna/loop.lua:123" or "builtin"
  doc?: string; // docstring when the runtime carries one
  members?: Record<string, SymbolNode>;
}

export interface SymbolTable {
  generated: string;
  modules: Record<string, SymbolNode>;
  globals: Record<string, SymbolNode>;
  docs: Record<string, string>;
}

export function parseSymbols(json: string): SymbolTable {
  const raw = JSON.parse(json) as Partial<SymbolTable>;
  if (!raw || typeof raw !== "object" || typeof raw.modules !== "object") {
    throw new Error("symbols: not a symbol table (missing modules)");
  }
  return {
    generated: String(raw.generated ?? ""),
    modules: raw.modules,
    globals: raw.globals ?? {},
    docs: raw.docs ?? {},
  };
}

export function moduleNames(table: SymbolTable): string[] {
  return Object.keys(table.modules).sort();
}

/**
 * Resolve a dotted chain ("loop", "fs.readFile", "kernel.wake") against
 * the snapshot: the head resolves as a module name, then as a global
 * (kernel), then each following segment walks members.
 */
export function resolveChain(
  table: SymbolTable,
  segments: string[],
): SymbolNode | undefined {
  if (segments.length === 0 || !/^\w[\w.]*$/.test(segments.join("."))) {
    return undefined;
  }
  const [head, ...rest] = segments;
  let node: SymbolNode | undefined = table.modules[head] ?? table.globals[head];
  for (const seg of rest) {
    if (!node || !node.members) {
      return undefined;
    }
    node = node.members[seg];
  }
  return node;
}

/** The members of a resolved node, sorted (for require/completions). */
export function memberNames(node: SymbolNode): string[] {
  return Object.keys(node.members ?? {}).sort();
}

/**
 * The require-alias map of a document: `local X = require "y"` binds
 * the alias X to module y, so later `X.` chains resolve against the
 * snapshot. Also collects non-local `X = require(...)` table fields.
 */
export function requireAliases(text: string): Record<string, string> {
  const aliases: Record<string, string> = {};
  const re = /(\w[\w.]*)\s*=\s*require\s*\(?\s*["']([\w.\-]+)["']/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(text)) !== null) {
    // dotted alias targets (X.a.b = require ...) resolve through the
    // first segment's table; the snapshot walks members, so keep the
    // full dotted alias — resolveChain handles the segments
    aliases[m[1]] = m[2];
  }
  return aliases;
}

/**
 * Expand the dotted chain around a column (offset within the line):
 * the run of [\w.:] characters covering the offset, split on "." and
 * ":". Returns null when the offset sits on nothing chain-like.
 */
export function chainAround(line: string, column: number): string[] | null {
  // a method call's single ':' splits like '.' (loop.sock:write — the
  // sock face hangs off the same members)
  const re = /[\w.:]+/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(line)) !== null) {
    if (column >= m.index && column <= m.index + m[0].length) {
      return m[0].split(/[.:]+/).filter((s) => s !== "");
    }
  }
  return null;
}

/**
 * The trailing identifier chain of a line prefix (the complete-module
 * convention): "loop.setT" -> {chain: ["loop"], typed: "setT"},
 * "lo" -> {chain: [], typed: "lo"}.
 */
export function trailingChain(
  prefix: string,
): { chain: string[]; typed: string } {
  const m = prefix.match(/([\w.]*)$/);
  const word = m ? m[1] : "";
  const lastDot = word.lastIndexOf(".");
  if (lastDot < 0) {
    return { chain: [], typed: word };
  }
  const chain = word.slice(0, lastDot).split(".").filter((s) => s !== "");
  const typed = word.slice(lastDot + 1);
  return { chain, typed };
}

/**
 * The require-string prefix of a line prefix, when the cursor sits
 * inside require's quotes: `require "lo` -> "lo", else null.
 */
export function requirePrefix(prefix: string): string | null {
  const m =
    prefix.match(/require\s*\(?\s*["']([\w.\-]*)$/) ??
    prefix.match(/require\s*["']([\w.\-]*)$/);
  return m ? m[1] : null;
}
