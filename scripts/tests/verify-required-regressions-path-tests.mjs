import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { mkdir, mkdtemp, readFile, realpath, rename, rm, symlink, unlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, relative, resolve, sep } from 'node:path';

assert.equal(process.platform, 'win32', 'The temporary-path regression requires a real Windows junction');
const sourcePath = new URL('./verify-required-regressions-tests.mjs', import.meta.url);
const source = await readFile(sourcePath, 'utf8');
const initStart = source.indexOf('const tempBase = ');
const initEnd = source.indexOf('\nconst ownerFiles = ', initStart);
const guardStart = source.indexOf('async function assertOwnedLeaf() {');
const guardEnd = source.indexOf('\nfunction fixtureChildPath(', guardStart);
const hashSource = source.match(/^const hash = .+$/m)?.[0];
assert.ok(initStart >= 0 && initEnd > initStart && guardStart >= 0 && guardEnd > guardStart && hashSource,
  'Exact current fixture initialization and ownership guard must be available');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
// Execute the actual source guard; only tmpdir selects our real junction alias.
const initialize = new AsyncFunction('assert', 'createHash', 'randomBytes', 'realpath', 'resolve', 'tmpdir',
  'mkdtemp', 'join', 'writeFile', 'readFile', 'relative', 'sep', 'console',
  source.slice(initStart, initEnd) + '\n' + hashSource + '\n' + source.slice(guardStart, guardEnd) +
  '\nawait assertOwnedLeaf(); return {tempBase, root, markerPath, markerBytes, assertOwnedLeaf};');

const temporaryParent = await realpath(tmpdir());
const testRoot = await mkdtemp(join(temporaryParent, 'nstu-gate-path-'));
const ownerToken = randomBytes(32).toString('hex');
const ownerMarker = join(testRoot, '.path-test-owner');
await writeFile(ownerMarker, ownerToken);
const target = join(testRoot, 'canonical parent');
const alias = join(testRoot, 'parent alias');
let aliasCreated = false, primaryFailure;
try {
  await mkdir(target);
  await symlink(target, alias, 'junction'); aliasCreated = true;
  assert.notEqual(resolve(alias), await realpath(alias), 'A real filesystem alias must change canonical spelling');
  assert.equal(await realpath(alias), target);
  const outside = join(testRoot, 'outside parent'); await mkdir(outside);
  for (const parent of [target, alias]) {
    const fixture = await initialize(assert, createHash, randomBytes, realpath, resolve, () => parent,
      mkdtemp, join, writeFile, readFile, relative, sep, { log() {} });
    assert.equal(fixture.tempBase, target);
    assert.equal(await realpath(fixture.root), resolve(fixture.root));
    await mkdir(join(fixture.root, '.git'));
    assert.equal(await realpath(join(fixture.root, '.git')), resolve(fixture.root, '.git'));
    await writeFile(fixture.markerPath, 'changed owner marker');
    await assert.rejects(fixture.assertOwnedLeaf(), /Exact fixture ownership marker must remain/);
    await writeFile(fixture.markerPath, fixture.markerBytes);
    await fixture.assertOwnedLeaf();
    const retained = fixture.root + '.retained';
    await rename(fixture.root, retained);
    await symlink(outside, fixture.root, 'junction');
    await assert.rejects(fixture.assertOwnedLeaf(), { code: 'ERR_ASSERTION' });
    await unlink(fixture.root); await rename(retained, fixture.root);
    await fixture.assertOwnedLeaf();
  }
  console.log('GATE PATH TESTS: PASS (canonical parent, real junction alias, marker tamper, root escape); source SHA256=' +
    createHash('sha256').update(source).digest('hex').toUpperCase());
} catch (error) {
  primaryFailure = error; throw error;
} finally {
  try {
    assert.equal(await realpath(testRoot), resolve(testRoot));
    const leaf = relative(temporaryParent, testRoot);
    assert.ok(leaf.startsWith('nstu-gate-path-') && !leaf.includes(sep));
    assert.equal(await readFile(ownerMarker, 'utf8'), ownerToken);
    if (aliasCreated) await unlink(alias);
    await rm(testRoot, { recursive: true, force: true });
  } catch (error) {
    if (primaryFailure) throw new AggregateError([primaryFailure, error], 'Path regression and owned fixture finalization failed');
    throw error;
  }
}
