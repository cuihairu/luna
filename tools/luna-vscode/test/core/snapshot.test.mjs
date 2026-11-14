// The SHIPPED snapshot's contract: it exists, it parses, and it covers
// the modules the extension promises to complete on. The snapshot is
// generated (scripts/regen.sh) and committed, so this passes without a
// luna binary — CI runs the static half, dev machines run the live
// half (attach.test.mjs).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const require = createRequire(import.meta.url);
const { parseSymbols, moduleNames, resolveChain, memberNames } = require(
  join(root, 'out', 'core', 'symbols.js')
);

const table = parseSymbols(readFileSync(join(root, 'symbols.json'), 'utf8'));

test('the snapshot is generated and stamped', () => {
  assert.match(table.generated, /^\d{4}-\d{2}-\d{2}T/, 'an ISO stamp says when the runtime was dumped');
});

test('core modules the completions promise are in the snapshot', () => {
  const mods = moduleNames(table);
  for (const m of ['loop', 'fs', 'net', 'http', 'process', 'socket', 'json', 'task']) {
    assert.ok(mods.includes(m), `${m} must be dumped (got: ${mods.length} modules)`);
  }
});

test('the snapshot carries real faces, not stub entries', () => {
  const funcs = [];
  const walk = (node, path) => {
    if (node.kind === 'function') funcs.push(path);
    for (const [k, child] of Object.entries(node.members ?? {})) walk(child, `${path}.${k}`);
  };
  for (const [name, node] of Object.entries(table.modules)) walk(node, name);
  for (const [name, node] of Object.entries(table.globals)) walk(node, name);
  assert.ok(funcs.length >= 100, `hundreds of runtime functions, got ${funcs.length}`);

  // spot checks with signatures and definition sites; sig is either a
  // param count or function(...) for varargs — introspect's own shapes
  const setTimeoutFn = resolveChain(table, ['loop', 'setTimeout']);
  assert.equal(setTimeoutFn?.kind, 'function');
  assert.match(setTimeoutFn?.sig ?? '', /^function\((?:\d+ params?|\.\.\.)\)$/);
  // C-implemented faces have no Lua source — at is honestly absent;
  // Lua-sourced faces carry module/<file>.lua:<line>
  let withAt = 0;
  const walkAt = (node) => {
    if (node.kind === 'function' && node.at) {
      withAt++;
      assert.match(node.at, /^(modules|luna)\/[\w./-]+:\d+$/, `at shape: ${node.at}`);
    }
    for (const child of Object.values(node.members ?? {})) walkAt(child);
  };
  for (const node of Object.values(table.modules)) walkAt(node);
  for (const node of Object.values(table.globals)) walkAt(node);
  assert.ok(withAt >= 50, `Lua faces carry at (${withAt} do)`);

  const readFileFn = resolveChain(table, ['fs', 'readFileSync']);
  assert.equal(readFileFn?.kind, 'function');

  const wakeFn = resolveChain(table, ['kernel', 'wake']);
  assert.equal(wakeFn?.kind, 'function', 'kernel is the one luna global');
});

test('the flat docs table carries the runtime docstrings', () => {
  // introspect.docs covers base-stdlib paths (string.rep, io.write...);
  // base globals are NOT dumped as faces — that is community Lua
  // plugins' territory — so the flat table is the hover source for them
  assert.ok(Object.keys(table.docs).length > 0, 'docs are dumped');
  assert.equal(typeof table.docs['string.rep'], 'string');
  assert.ok(table.docs['string.rep'].length > 0);
});

test('module members are sorted and ident-named', () => {
  const loopMembers = memberNames(resolveChain(table, ['loop']));
  assert.ok(loopMembers.length > 3);
  assert.deepEqual(loopMembers, [...loopMembers].sort());
  for (const m of loopMembers) assert.match(m, /^\w[\w]*$/);
});
