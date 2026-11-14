// Symbol snapshot lookups — pure resolution against an inline mini
// table. The SHIPPED snapshot's shape has its own contract test in
// snapshot.test.mjs (after scripts/regen.sh generates it).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const require = createRequire(import.meta.url);
const core = require(join(root, 'out', 'core', 'symbols.js'));

const mini = core.parseSymbols(JSON.stringify({
  generated: 'test',
  modules: {
    loop: {
      kind: 'table',
      members: {
        setTimeout: { kind: 'function', sig: 'function(2 params)', at: 'luna/loop.lua:12' },
        fs: {
          kind: 'table',
          members: { readFile: { kind: 'function', sig: 'function(2 params)' } },
        },
      },
    },
  },
  globals: {
    kernel: {
      kind: 'table',
      members: { wake: { kind: 'function', sig: 'function(1 param)' } },
    },
  },
  docs: { 'string.rep': 'returns the string repeated' },
}));

test('parseSymbols rejects non-tables and keeps missing sections empty', () => {
  assert.throws(() => core.parseSymbols('{"modules": 1}'));
  const t = core.parseSymbols('{"modules": {}}');
  assert.deepEqual(t.globals, {});
  assert.deepEqual(t.docs, {});
});

test('moduleNames sorts', () => {
  assert.deepEqual(core.moduleNames(mini), ['loop']);
});

test('resolveChain walks modules, globals and nested faces', () => {
  assert.equal(core.resolveChain(mini, ['loop'])?.kind, 'table');
  assert.equal(core.resolveChain(mini, ['loop', 'setTimeout'])?.sig, 'function(2 params)');
  assert.equal(core.resolveChain(mini, ['loop', 'fs', 'readFile'])?.kind, 'function');
  assert.equal(core.resolveChain(mini, ['kernel', 'wake'])?.sig, 'function(1 param)');
});

test('resolveChain refuses junk and unknowns', () => {
  assert.equal(core.resolveChain(mini, []), undefined);
  assert.equal(core.resolveChain(mini, ['nope']), undefined);
  assert.equal(core.resolveChain(mini, ['loop', 'nope']), undefined);
  assert.equal(core.resolveChain(mini, ['loop', 'setTimeout', 'x']), undefined);
});

test('memberNames lists the snapshot members', () => {
  const node = core.resolveChain(mini, ['loop']);
  assert.deepEqual(core.memberNames(node), ['fs', 'setTimeout']);
});

test('requireAliases reads both require spellings', () => {
  const text = [
    'local loop = require "loop"',
    'local fs = require("loop.fs")',
    'kernel = require "kernel"',
  ].join('\n');
  assert.deepEqual(core.requireAliases(text), {
    loop: 'loop',
    fs: 'loop.fs',
    kernel: 'kernel',
  });
});

test('chainAround covers the column and splits method colons', () => {
  assert.deepEqual(core.chainAround('loop.fs.readFile(x)', 10), ['loop', 'fs', 'readFile']);
  assert.deepEqual(core.chainAround('loop.sock:write(x)', 11), ['loop', 'sock', 'write']);
  assert.deepEqual(core.chainAround('loop', 2), ['loop']);
  assert.equal(core.chainAround('  (  ', 2), null);
});

test('trailingChain splits the typed tail from the chain', () => {
  assert.deepEqual(core.trailingChain('loop.setT'), { chain: ['loop'], typed: 'setT' });
  assert.deepEqual(core.trailingChain('lo'), { chain: [], typed: 'lo' });
  assert.deepEqual(core.trailingChain('loop.'), { chain: ['loop'], typed: '' });
  assert.deepEqual(core.trailingChain('loop.fs.read'), { chain: ['loop', 'fs'], typed: 'read' });
  assert.deepEqual(core.trailingChain(''), { chain: [], typed: '' });
});

test('requirePrefix catches the cursor inside require quotes', () => {
  assert.equal(core.requirePrefix('x = require "lo'), 'lo');
  assert.equal(core.requirePrefix('x = require("lo'), 'lo');
  assert.equal(core.requirePrefix('x = require("loop.f'), 'loop.f');
  assert.equal(core.requirePrefix('local s = "lo'), null);
});
