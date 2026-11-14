// Attach channel tests. Two layers:
//
//   1. fake-server cases (always run): a plain net server speaking the
//      serve frame protocol — reply timeout, late-frame teardown, the
//      peer-closed rejection.
//   2. live integration (skipped, loudly, when the repo has no built
//      binary): a REAL `luna` target spinning in a busy loop — the
//      count-hook poll is the only responder, which is exactly the
//      "still answers while busy" contract the attach channel promises.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { spawn } from 'node:child_process';
import { mkdtempSync, existsSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as net from 'node:net';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const repoRoot = join(root, '..', '..');
const require = createRequire(import.meta.url);
const { LunaAttach, ps, sockPathFor, sockDir } = require(
  join(root, 'out', 'core', 'attach.js')
);

const lunaBin = process.env.LUNA_BIN ?? join(repoRoot, 'build', 'luna');
const haveLuna = existsSync(lunaBin);

// -- fake-server cases ----------------------------------------------------

function fakeServer(handler) {
  return new Promise((resolve) => {
    const dir = mkdtempSync(join(tmpdir(), 'luna-attach-test-'));
    const server = net.createServer(handler);
    server.listen(join(dir, 'luna-4194302.sock'), () => {
      resolve({ dir, server, close: () => server.close(() => rmSync(dir, { recursive: true, force: true })) });
    });
  });
}

test('no reply within the timeout rejects and tears the session down', async () => {
  const { dir, server, close } = await fakeServer(() => {
    /* swallows requests, never replies */
  });
  const a = new LunaAttach();
  const oldDir = process.env.LUNA_SOCK_DIR;
  process.env.LUNA_SOCK_DIR = dir;
  try {
    await a.connect(4194302, 2000);
    // the late reply must not land in a later exchange's slot: the
    // timeout destroys the socket, so the next exchange refuses
    await assert.rejects(
      a.exchange('return 1', 250),
      /no reply within 250ms/,
    );
    assert.equal(a.connected, false);
    await assert.rejects(a.exchange('return 2', 250), /not attached/);
  } finally {
    process.env.LUNA_SOCK_DIR = oldDir;
    a.close();
    close();
  }
});

test('a peer that closes mid-exchange surfaces a clear error', async () => {
  const { dir, close } = await fakeServer((sock) => {
    sock.on('data', () => sock.destroy());
  });
  const a = new LunaAttach();
  const oldDir = process.env.LUNA_SOCK_DIR;
  process.env.LUNA_SOCK_DIR = dir;
  try {
    await a.connect(4194302, 2000);
    await assert.rejects(a.exchange('return 1', 2000), /closed the connection/);
  } finally {
    process.env.LUNA_SOCK_DIR = oldDir;
    a.close();
    close();
  }
});

test('a full exchange round-trips through a real frame writer', async () => {
  const { dir, close } = await fakeServer((sock) => {
    sock.on('data', (buf) => {
      const line = buf.toString('utf8').trimEnd();
      sock.write('OK\n' + JSON.stringify(line.toUpperCase()) + '\n\x1e\n');
    });
  });
  const a = new LunaAttach();
  const oldDir = process.env.LUNA_SOCK_DIR;
  process.env.LUNA_SOCK_DIR = dir;
  try {
    await a.connect(4194302, 2000);
    const r = await a.exchange('return 1+1', 2000);
    assert.equal(r.status, 'OK');
    assert.equal(r.body, JSON.stringify('RETURN 1+1'));
    // latency is measured per exchange
    assert.equal(typeof a.latencyMs, 'number');
  } finally {
    process.env.LUNA_SOCK_DIR = oldDir;
    a.close();
    close();
  }
});

test('ps lists socket files with liveness', async () => {
  const { dir, close } = await fakeServer(() => {});
  const oldDir = process.env.LUNA_SOCK_DIR;
  process.env.LUNA_SOCK_DIR = dir;
  try {
    const rows = ps();
    const row = rows.find((r) => r.pid === 4194302);
    assert.ok(row, 'the fake socket file shows up');
    assert.equal(row.path, join(dir, 'luna-4194302.sock'));
    // nothing owns pid 4194302: the file exists, the process does not
    assert.equal(row.alive, false);
  } finally {
    process.env.LUNA_SOCK_DIR = oldDir;
    close();
  }
});

// -- live integration: a real busy luna target ----------------------------

test('a busy luna target still answers exchanges and live completion', {
  skip: haveLuna ? false : `no luna binary at ${lunaBin} (run cmake --build . first)`,
}, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'luna-attach-live-'));
  const script = join(dir, 'busy.lua');
  // a pure busy loop: no idle REPL, only the instruction-count hook's
  // serve.step poll can pick the exchange up — the hard case
  writeFileSync(script, 'local x = 0\nwhile true do\n  x = x + 1\nend\n');
  const child = spawn(lunaBin, [script], {
    env: { ...process.env, LUNA_SOCK_DIR: dir },
    stdio: 'ignore',
  });
  const a = new LunaAttach();
  const oldDir = process.env.LUNA_SOCK_DIR;
  try {
    // point OUR lookups at the target's sock dir BEFORE waiting: the
    // child binds at $LUNA_SOCK_DIR (its spawn env), not our /tmp
    process.env.LUNA_SOCK_DIR = dir;
    // wait for the target's socket file (serve.start binds at boot)
    const path = sockPathFor(child.pid);
    for (let i = 0; i < 100 && !existsSync(path); i++) {
      await new Promise((r) => setTimeout(r, 100));
    }
    assert.ok(existsSync(path), 'target opened its attach socket');

    const rows = ps();
    assert.equal(rows.find((r) => r.pid === child.pid)?.alive, true);

    await a.connect(child.pid, 5000);

    const ok = await a.exchange('return 1+1', 10000);
    assert.equal(ok.status, 'OK');
    assert.equal(ok.body, '2');

    const err = await a.exchange('1+', 10000);
    assert.equal(err.status, 'ERR');
    assert.ok(err.body.length > 0, 'the error text comes back');

    // live completion: the target's OWN completion engine answers
    const cands = await a.complete('string.le', 10000);
    assert.ok(
      cands.some((c) => c.startsWith('len')),
      `candidates include string.len*, got: ${JSON.stringify(cands)}`,
    );

    // exchanges stay sequential and keep answering; string results
    // arrive quoted (introspect.repr shape)
    const again = await a.exchange('return "still" .. "alive"', 10000);
    assert.equal(again.status, 'OK');
    assert.ok(again.body.includes('stillalive'), `repr carries the value: ${again.body}`);
    assert.ok(/^['"]/.test(again.body), 'string results arrive quoted');
  } finally {
    process.env.LUNA_SOCK_DIR = oldDir;
    a.close();
    child.kill('SIGKILL');
    rmSync(dir, { recursive: true, force: true });
  }
});
