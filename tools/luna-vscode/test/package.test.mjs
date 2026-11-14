// package.json contract — activation, entry point, settings, and the
// packaging surface (.vscodeignore keeps what the vsix needs).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const extRoot = join(here, '..');

test('package.json wires activation, entry point, and settings', () => {
  const pkg = JSON.parse(readFileSync(join(extRoot, 'package.json'), 'utf8'));
  assert.equal(pkg.main, './out/extension.js', 'main must be the compiled JS');
  assert.ok(pkg.engines?.vscode, 'engines.vscode pins the client floor');
  assert.ok(
    pkg.activationEvents.includes('onLanguage:lua'),
    'completions/hover need activation on lua files',
  );
  const props = pkg.contributes?.configuration?.properties ?? {};
  assert.ok(props['luna.runtimePath'], 'luna.runtimePath feeds run/spawn');
  assert.ok(props['luna.attachTimeoutMs'], 'luna.attachTimeoutMs feeds exchanges');
  const commands = (pkg.contributes?.commands ?? []).map((c) => c.command);
  for (const cmd of [
    'luna.runFile',
    'luna.runSelection',
    'luna.attach',
    'luna.attach.sendLine',
    'luna.detach',
  ]) {
    assert.ok(commands.includes(cmd), `${cmd} must be contributed`);
  }
});

test('the extension ships no runtime dependencies (vscode only)', () => {
  const pkg = JSON.parse(readFileSync(join(extRoot, 'package.json'), 'utf8'));
  assert.deepEqual(
    Object.keys(pkg.dependencies ?? {}),
    [],
    'no bundling: the devcore uses only node builtins + vscode',
  );
});

test('vscodeignore keeps the vsix payload and drops the sources', () => {
  const ignore = readFileSync(join(extRoot, '.vscodeignore'), 'utf8');
  // the vsix payload: compiled out/, symbols.json, LICENSE — none of
  // them may appear as an ignore line
  for (const payload of ['out', 'symbols.json', 'LICENSE']) {
    assert.ok(
      !ignore.split('\n').some((l) => l.trim() === payload || l.trim().startsWith(payload + '/')),
      `${payload} must not be ignored`,
    );
  }
  assert.match(ignore, /node_modules/, 'node_modules must not ship');
  assert.match(ignore, /scripts\/\*\*/, 'dump scripts do not ship');
  assert.match(ignore, /test\/\*\*/, 'tests do not ship');
});
