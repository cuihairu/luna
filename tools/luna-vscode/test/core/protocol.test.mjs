// Frame protocol unit tests — the pure reader, driven with whole
// chunks, split chunks and byte-by-byte feeds. Run: npm test (from
// tools/luna-vscode).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');

// the compiled core (npm test compiles first)
const require = createRequire(import.meta.url);
const { FrameReader, FRAME_TERMINATOR, COMPLETION_META } = require(
  join(root, 'out', 'core', 'protocol.js')
);

test('a whole frame in one chunk parses', () => {
  const r = new FrameReader();
  const frames = r.push('OK\n2\n\x1e\n');
  assert.equal(frames.length, 1);
  assert.equal(frames[0].status, 'OK');
  assert.equal(frames[0].body, '2');
});

test('an error frame keeps its status and multi-line body', () => {
  const r = new FrameReader();
  const frames = r.push('ERR\neval[1]: boom\nstack line\n\x1e\n');
  assert.equal(frames.length, 1);
  assert.equal(frames[0].status, 'ERR');
  assert.equal(frames[0].body, 'eval[1]: boom\nstack line');
});

test('an empty-body frame is body ""', () => {
  const r = new FrameReader();
  const frames = r.push('OK\n\x1e\n');
  assert.equal(frames.length, 1);
  assert.equal(frames[0].status, 'OK');
  assert.equal(frames[0].body, '');
});

test('byte-by-byte feeding reassembles frames', () => {
  const r = new FrameReader();
  const whole = 'EXIT\n%exit inside attach detaches the client\n\x1e\n';
  let collected = [];
  for (const ch of whole) {
    collected = collected.concat(r.push(ch));
  }
  assert.equal(collected.length, 1);
  assert.equal(collected[0].status, 'EXIT');
  assert.equal(collected[0].body, '%exit inside attach detaches the client');
});

test('two frames in one chunk both come out, in order', () => {
  const r = new FrameReader();
  const frames = r.push('OK\n1\n\x1e\nOK\n2\n\x1e\n');
  assert.equal(frames.length, 2);
  assert.equal(frames[0].body, '1');
  assert.equal(frames[1].body, '2');
});

test('a chunk boundary may split the terminator from its newline', () => {
  const r = new FrameReader();
  assert.equal(r.push('OK\nx').length, 0); // partial body line
  assert.equal(r.push('y\n\x1e').length, 0); // terminator without newline
  const frames = r.push('\n');
  assert.equal(frames.length, 1);
  assert.equal(frames[0].body, 'xy');
});

test('utf-8 body lines survive multi-chunk splits', () => {
  const r = new FrameReader();
  // "λ≠" = 4 bytes; split inside the multibyte sequence is the
  // socket decoder's job (setEncoding), here we only prove the reader
  // holds partial lines until their newline arrives
  assert.equal(r.push('OK\nλ≠').length, 0);
  const frames = r.push('\n\x1e\n');
  assert.equal(frames[0].body, 'λ≠');
});

test('the terminator and completion meta constants match the wire', () => {
  assert.equal(FRAME_TERMINATOR, '\x1e'); // \30, the RS line
  assert.equal(COMPLETION_META, '\x01complete'); // \1complete meta prefix
  assert.equal('OK'.charCodeAt(0), 0x4f);
});
