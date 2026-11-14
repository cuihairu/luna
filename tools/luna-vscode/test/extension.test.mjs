// Extension smoke — cage-style: the `vscode` module is stubbed with a
// behavioral harness (registries, buffers, an in-memory document set)
// and the compiled out/extension.js is driven through it. Three task
// points + the attach REPL flow:
//
//   1. completion — snapshot faces (dotted + require-string + alias)
//   2. hover — snapshot doc for the chain under the cursor
//   3. run file — REAL luna binary, output lands in the channel
//   4. attach — fake serve target: sendLine round-trip and the live
//      completion track merged into the static one

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import Module from 'node:module';
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import * as net from 'node:net';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const require = createRequire(import.meta.url);

// -- the vscode stub -------------------------------------------------------

const calls = { commands: [], messages: [], quickPickResult: undefined, shown: [] };
const docsByPath = new Map();
let replCounter = 0;

function offsetOf(text, pos) {
  const lines = text.split('\n');
  let off = 0;
  for (let i = 0; i < pos.line && i < lines.length; i++) off += lines[i].length + 1;
  return off + pos.character;
}

function makeDoc(initial, meta = {}) {
  const doc = {
    uri: meta.uri ?? { scheme: 'untitled', path: `/untitled-${++replCounter}.lua` },
    fileName: meta.fileName ?? (meta.uri?.path ?? `/untitled-${++replCounter}.lua`),
    languageId: 'lua',
    _text: initial,
    getText(range) {
      if (!range) return this._text;
      return this._text.slice(offsetOf(this._text, range.start), offsetOf(this._text, range.end));
    },
    lineAt(i) {
      return { text: this._text.split('\n')[i] };
    },
    positionAt(off) {
      const lines = this._text.slice(0, off).split('\n');
      const p = { line: lines.length - 1, character: lines[lines.length - 1].length };
      p._off = off;
      return p;
    },
    save: async () => true,
  };
  docsByPath.set(doc.uri.path, doc);
  return doc;
}

class Position {
  constructor(line, character) { this.line = line; this.character = character; }
  with(patch) { return new Position(patch.line ?? this.line, patch.character ?? this.character); }
}
class Range { constructor(start, end) { this.start = start; this.end = end; } }
class CompletionItem {
  constructor(label, kind) { this.label = label; this.kind = kind; }
}
class Hover { constructor(contents) { this.contents = contents; } }
class MarkdownString { constructor() { this.value = ''; } appendText(t) { this.value += t; } }
class WorkspaceEdit {
  constructor() { this.ops = []; }
  insert(uri, position, text) { this.ops.push({ uri, position, text }); }
}

const vscode = {
  commands: {
    registry: new Map(),
    registerCommand(name, fn) {
      this.registry.set(name, fn);
      return { dispose() {} };
    },
    async executeCommand(name, ...args) {
      calls.commands.push([name, ...args]);
      if (name === 'setContext') return undefined;
      return this.registry.get(name)?.(...args);
    },
  },
  languages: {
    completion: null,
    hover: null,
    registerCompletionItemProvider(_sel, provider, ...chars) {
      this.completion = { provider, chars: chars.join('') };
      return { dispose() {} };
    },
    registerHoverProvider(_sel, provider) {
      this.hover = provider;
      return { dispose() {} };
    },
  },
  window: {
    get activeTextEditor() { return activeEditor; },
    createOutputChannel(name) {
      const ch = {
        name, _buf: '',
        append: (s) => { ch._buf += s; },
        appendLine: (s) => { ch._buf += s + '\n'; },
        show: () => {}, dispose: () => {},
      };
      return ch;
    },
    createStatusBarItem() {
      const bar = {
        text: '', command: undefined, tooltip: undefined,
        show: () => { calls.shown.push('status'); }, hide: () => {}, dispose: () => {},
      };
      return bar;
    },
    async showQuickPick(items, _opts) {
      if (calls.quickPickResult !== undefined) return calls.quickPickResult;
      return items[0];
    },
    showErrorMessage: (m) => calls.messages.push(['error', m]),
    showInformationMessage: (m) => calls.messages.push(['info', m]),
    async showTextDocument(doc, _opts) { calls.shown.push(doc); },
  },
  workspace: {
    config: { runtimePath: join(root, '..', '..', 'build', 'luna'), attachTimeoutMs: 30000 },
    getConfiguration() {
      return { get: (k, d) => (k in this.config ? this.config[k] : d) };
    },
    async openTextDocument(arg) {
      if (arg && typeof arg === 'object' && arg.content !== undefined) {
        return makeDoc(arg.content, { languageId: arg.language });
      }
      return makeDoc('', { uri: { scheme: 'file', path: arg } });
    },
    async applyEdit(edit) {
      for (const op of edit.ops) {
        const doc = docsByPath.get(op.uri.path);
        if (!doc) throw new Error(`stub: no doc at ${op.uri.path}`);
        const off = op.position._off ?? offsetOf(doc._text, op.position);
        doc._text = doc._text.slice(0, off) + op.text + doc._text.slice(off);
      }
      return true;
    },
  },
  StatusBarAlignment: { Left: 1, Right: 2 },
  CompletionItemKind: { Module: 9, Class: 4, Function: 3, Field: 5, Value: 21 },
  Position, Range, CompletionItem, Hover, MarkdownString, WorkspaceEdit,
};

let activeEditor = null;

const origLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return vscode;
  return origLoad.apply(this, arguments);
};

const { activate } = require(join(root, 'out', 'extension.js'));
const ctx = { subscriptions: [] };
const lunaBin = join(root, '..', '..', 'build', 'luna');
const haveLuna = spawnSync('test', ['-x', lunaBin]).status === 0;

before(async () => {
  await activate(ctx);
  assert.ok(vscode.languages.completion, 'completion provider registered');
  assert.ok(vscode.languages.hover, 'hover provider registered');
});
after(() => { Module._load = origLoad; });

const provider = () => vscode.languages.completion.provider;

// -- 1. completion ----------------------------------------------------------

test('completion offers snapshot members through a require alias', async () => {
  const doc = makeDoc("local loop = require 'loop'\nloop.setT");
  const pos = new Position(1, 8);
  activeEditor = null; // provider must not need an editor for static faces
  const items = await provider().provideCompletionItems(doc, pos);
  const hit = items.find((i) => i.label === 'setTimeout');
  assert.ok(hit, `setTimeout offered, got: ${items.map((i) => i.label).join(',')}`);
  assert.match(hit.detail, /function/, 'detail carries the runtime signature');
  assert.ok(!hit.detail.includes('params') || hit.detail.length > 0, 'signature shape');
});

test('completion inside require quotes lists the module names', async () => {
  const doc = makeDoc('local x = require "ht');
  const pos = new Position(0, 22);
  const items = await provider().provideCompletionItems(doc, pos);
  assert.ok(items.some((i) => i.label === 'http'), 'module names offered');
  assert.ok(!items.some((i) => i.label === 'setTimeout'), 'no members leak into require strings');
});

test('bare prefix offers module heads and luna globals', async () => {
  const doc = makeDoc('ke');
  const items = await provider().provideCompletionItems(doc, new Position(0, 2));
  assert.ok(items.some((i) => i.label === 'kernel'), 'the luna global is offered');
});

// -- 2. hover ----------------------------------------------------------------

test('hover documents the chain under the cursor', async () => {
  const doc = makeDoc('local f = fs.readFileSync(p)');
  const hover = vscode.languages.hover.provideHover(doc, new Position(0, 18));
  assert.ok(hover, 'a hover comes back for a known chain');
  const detail = hover.contents[0];
  assert.match(detail, /function/, 'detail carries kind/signature');
  const md = hover.contents[1];
  assert.ok(md && typeof md.value === 'string');
});

test('hover stays quiet on unknown chains', () => {
  const doc = makeDoc('nope.nada(x)');
  const hover = vscode.languages.hover.provideHover(doc, new Position(0, 6));
  assert.equal(hover, undefined);
});

// -- 3. run file (real binary) --------------------------------------------------

test('run file spawns the real luna and streams to the channel', { skip: haveLuna ? false : 'no luna binary' }, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'luna-ext-run-'));
  const file = join(dir, 'hello.lua');
  writeFileSync(file, 'print("hello from the file run")\n');
  const doc = makeDoc('print("hello from the file run")\n', {
    uri: { scheme: 'file', path: file },
    fileName: file,
  });
  activeEditor = { document: doc, selection: { active: new Position(0, 0) } };
  const channel = ctx.subscriptions.find((s) => s.name === 'luna');
  assert.ok(channel, 'the luna output channel is registered');

  await vscode.commands.executeCommand('luna.runFile');
  for (let i = 0; i < 100 && !channel._buf.includes('hello from the file run'); i++) {
    await new Promise((r) => setTimeout(r, 100));
  }
  assert.match(channel._buf, /hello from the file run/);
  assert.match(channel._buf, /\[exit 0\]/);
  activeEditor = null;
  rmSync(dir, { recursive: true, force: true });
});

// -- 4. attach: sendLine round-trip + the live completion track --------------

let sockDirLive;
let server;
let standIn;

function fakeLuna(handler) {
  sockDirLive = mkdtempSync(join(tmpdir(), 'luna-ext-attach-'));
  server = net.createServer(handler);
  // a throwaway live pid: ps() liveness must pass for the quick pick to
  // offer it — and the doorbell SIGUSR1 must NOT hit this test process
  // (in node, SIGUSR1 starts the inspector)
  standIn = spawn('sleep', ['60'], { stdio: 'ignore' });
  return new Promise((resolve) => {
    server.listen(join(sockDirLive, `luna-${standIn.pid}.sock`), resolve);
  });
}

test('attach drives sendLine and merges live completion candidates', async () => {
  await fakeLuna((sock) => {
    sock.on('data', (buf) => {
      const line = buf.toString('utf8').trimEnd();
      if (line.startsWith('\x01complete')) {
        sock.write('OK\nloop\nlive_thing\n\x1e\n');
      } else if (line === 'return 6*7') {
        sock.write('OK\n42\n\x1e\n');
      } else {
        sock.write('OK\nnil\n\x1e\n');
      }
    });
  });
  const oldDir = process.env.LUNA_SOCK_DIR;
  process.env.LUNA_SOCK_DIR = sockDirLive;
  try {
    calls.quickPickResult = undefined; // the fake target is the only pick
    await vscode.commands.executeCommand('luna.attach');
    const ctxCall = (val) =>
      calls.commands.some(([c, k, v]) => c === 'setContext' && k === 'luna.attached' && v === val);
    assert.ok(ctxCall(true), 'luna.attached context goes true');

    // ctrl+enter: the line under the cursor round-trips to the target,
    // the reply lands in the REPL doc
    const editorDoc = makeDoc('return 6*7');
    const caret = new Position(0, 11);
    activeEditor = {
      document: editorDoc,
      // a Selection is a Range: start/end for getText, active for the cursor
      selection: { active: caret, start: new Position(0, 0), end: caret },
    };
    await vscode.commands.executeCommand('luna.attach.sendLine');
    const repl = [...docsByPath.values()].find((d) => d._text.includes('attached to'));
    assert.ok(repl, 'a REPL doc was opened');
    assert.match(repl._text, /-- > return 6\*7/, 'the sent line is echoed');
    assert.match(repl._text, /42/, 'the reply body lands in the doc');

    // dual-track completion: static "loop" head + live "live_thing"
    const doc = makeDoc('live_thing = 1\nlo');
    const items = await provider().provideCompletionItems(doc, new Position(1, 2));
    const labels = items.map((i) => i.label);
    assert.ok(labels.includes('loop'), `static track answers, got: ${labels.join(',')}`);
    const live = items.find((i) => i.label === 'live_thing');
    assert.ok(live, 'the live track merged its candidate');
    assert.match(live.detail, /attached luna process/, 'the live candidate is labeled');

    await vscode.commands.executeCommand('luna.detach');
    assert.ok(ctxCall(false), 'detach clears the context');
    activeEditor = null;
  } finally {
    process.env.LUNA_SOCK_DIR = oldDir;
    calls.quickPickResult = undefined;
    standIn.kill('SIGKILL');
    server.close(() => rmSync(sockDirLive, { recursive: true, force: true }));
  }
});
