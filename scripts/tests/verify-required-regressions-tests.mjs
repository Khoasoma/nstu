import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { cp, mkdtemp, mkdir, readFile, readdir, realpath, rm, stat, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const self = fileURLToPath(import.meta.url);
const repo = resolve(fileURLToPath(new URL('../../', import.meta.url)));
const helper = 'scripts/tests/required-regressions.mjs';
const copiedSelfTest = 'scripts/tests/verify-required-regressions-tests.mjs';
const gate = 'scripts/verify-gate.sh';
const bash = join(process.env.ProgramFiles || 'C:\\Program Files', 'Git/bin/bash.exe');
const tempBase = await realpath(tmpdir());
const root = await mkdtemp(join(tempBase, 'nstu-gate01-'));
const markerBytes = Buffer.from(JSON.stringify({ schemaVersion: 1, token: randomBytes(32).toString('hex'), root }));
const markerPath = join(root, '.fixture-owner.json');
const emptyGitConfig = join(root, '.fixture-git-empty.config');
await writeFile(markerPath, markerBytes); await writeFile(emptyGitConfig, '');
console.log('Owned entrypoint fixture: ' + root);
const ownerFiles = ['owned-regression-process.mjs', 'owned-regression-process.ps1', 'owned-regression-process-tests.mjs'];
const directories = ['exam/web', 'exam/studio', 'exam/shared'];
const files = [
  gate, '.github/workflows/windows.yml', '.gitattributes', 'config/ui-desktop-tokens.json',
  'server/app/ui_desktop_tokens.hpp', 'exam/schema/manifest.schema.json',
  'scripts/generate-ui-desktop-tokens.mjs', 'tests/exam_web_recovery_tests.js',
  'tests/exam_runner_template_tests.mjs', 'tests/ui_desktop_token_tests.mjs',
  'tests/exam_origin_package_projection.mjs', 'packaging/stage-exam-package.ps1',
  'packaging/test-exam-origin-contract.ps1', 'packaging/test-exam-package-staging.ps1',
  helper, copiedSelfTest, 'scripts/tests/verify-required-regressions-path-tests.mjs',
  'tests/programming_web_tests.mjs', 'packaging/test-programming-package.ps1',
  'packaging/cs-policy-functions.ps1', 'packaging/prepare-cs-policy.ps1',
  'packaging/verify-cs-policy.ps1', 'packaging/test-cs-policy.ps1',
  'tools/cs-judge/broker.py', 'tools/cs-judge/worker.py', 'tools/cs-judge/isolate_runner.py',
  'tools/cs-judge/codeforces_import.py', 'tools/cs-judge/operator_client.py',
  'tools/cs-judge/test_broker.py', 'tools/cs-judge/test_isolate_runner.py',
  'tools/cs-judge/test_codeforces_import.py', 'tools/cs-judge/test_operator.py',
  ...ownerFiles.map(name => 'scripts/tests/' + name)
];
const hash = bytes => createHash('sha256').update(bytes).digest('hex').toUpperCase();
const original49LabelsSHA256 = '15B7073F8E0C95B1CFE2633E9141FE5145D3122E154FAD5A80871F6F84EB7FE5';
const livenessLabels = [
  'job snapshot descendant exits before retained open; fresh empty proof',
  'job snapshot live descendant cannot become empty through refresh',
  'owned signaled descendant and exit259 remain exited',
  'changed snapshot candidate never waits a foreign owner',
  'shared liveness decision rejects invalid and inconsistent observations',
  'access-denied descendant remains failure under outer containment'
];
const outputText = result => Buffer.concat([result.output.stdout, result.output.stderr]).toString('utf8');
const safeError = error => ({ message: error.message, stack: error.stack, ...(error.code ? { code: error.code } : {}) });
const summary = result => ({ ...result, output: { ...result.output, stdout: undefined, stderr: undefined,
  stdoutSha256: hash(result.output.stdout), stderrSha256: hash(result.output.stderr) } });
let copiedOwner, parentRuntime, ownerBaseline, sourcePaths, lastResult, primaryFailure, passed = 0;
const attempts = [], caseRecords = [], mutations = [];
const outputChunkBytes = 3072;
function emitOwnedOutput(invocationId, output) {
  assert.ok(Number.isSafeInteger(invocationId) && invocationId > 0);
  assert.ok(Buffer.isBuffer(output.stdout) && Buffer.isBuffer(output.stderr));
  assert.ok(output.stdout.length + output.stderr.length <= 8 * 1024 * 1024);
  for (const stream of ['stdout', 'stderr']) {
    for (let offset = 0, seq = 0; offset < output[stream].length; offset += outputChunkBytes, ++seq)
      console.log('GATE02_OUTPUT_CHUNK ' + JSON.stringify({ invocationId, stream, seq,
        bytesBase64: output[stream].subarray(offset, offset + outputChunkBytes).toString('base64') }));
  }
  console.log('GATE02_OUTPUT_END ' + JSON.stringify({ invocationId, stdoutBytes: output.stdout.length, stderrBytes: output.stderr.length,
    stdoutChunks: Math.ceil(output.stdout.length / outputChunkBytes), stderrChunks: Math.ceil(output.stderr.length / outputChunkBytes),
    stdoutSha256: hash(output.stdout), stderrSha256: hash(output.stderr) }));
}
function decodeOwnedOutput(lines, invocationId) {
  assert.ok(Number.isSafeInteger(invocationId) && invocationId > 0);
  const chunks = { stdout: [], stderr: [] };
  let footer = null, totalBytes = 0;
  for (const line of lines) {
    const chunkLine = line.startsWith('GATE02_OUTPUT_CHUNK '), endLine = line.startsWith('GATE02_OUTPUT_END ');
    if (!chunkLine && !endLine) continue;
    assert.ok(Buffer.byteLength(line) <= 6144, 'Bounded output frame required');
    const frame = copiedOwner.decodeControlJson(Buffer.from(line.slice(chunkLine ? 'GATE02_OUTPUT_CHUNK '.length : 'GATE02_OUTPUT_END '.length)));
    assert.ok(frame && typeof frame === 'object' && !Array.isArray(frame));
    assert.deepEqual(Object.keys(frame).sort(), (chunkLine ? ['invocationId', 'stream', 'seq', 'bytesBase64'] :
      ['invocationId', 'stdoutBytes', 'stderrBytes', 'stdoutChunks', 'stderrChunks', 'stdoutSha256', 'stderrSha256']).sort());
    assert.ok(Number.isSafeInteger(frame.invocationId) && frame.invocationId > 0);
    if (chunkLine) {
      assert.ok(['stdout', 'stderr'].includes(frame.stream));
      assert.ok(Number.isSafeInteger(frame.seq) && frame.seq >= 0);
      assert.ok(typeof frame.bytesBase64 === 'string' && frame.bytesBase64.length <= 4096, 'Bounded Base64 string required');
      const bytes = Buffer.from(frame.bytesBase64, 'base64');
      assert.ok(bytes.length > 0 && bytes.length <= outputChunkBytes);
      assert.equal(bytes.toString('base64'), frame.bytesBase64, 'Canonical Base64 required');
      if (frame.invocationId !== invocationId) continue;
      assert.equal(footer, null, 'No output chunks after END');
      assert.equal(frame.seq, chunks[frame.stream].length, 'Exact ordered output sequence required');
      totalBytes += bytes.length;
      assert.ok(totalBytes <= 8 * 1024 * 1024, 'Native output byte bound retained');
      chunks[frame.stream].push(bytes);
    } else {
      for (const key of ['stdoutBytes', 'stderrBytes', 'stdoutChunks', 'stderrChunks'])
        assert.ok(Number.isSafeInteger(frame[key]) && frame[key] >= 0 && frame[key] <= 8 * 1024 * 1024);
      assert.ok(frame.stdoutBytes + frame.stderrBytes <= 8 * 1024 * 1024);
      for (const key of ['stdoutSha256', 'stderrSha256']) assert.ok(typeof frame[key] === 'string' && /^[A-F0-9]{64}$/.test(frame[key]));
      if (frame.invocationId !== invocationId) continue;
      assert.equal(footer, null, 'Exactly one output END required'); footer = frame;
    }
  }
  assert.ok(footer, 'Complete output END required');
  const output = {};
  for (const stream of ['stdout', 'stderr']) {
    assert.equal(chunks[stream].length, footer[stream + 'Chunks']);
    output[stream] = Buffer.concat(chunks[stream]);
    assert.equal(output[stream].length, footer[stream + 'Bytes']);
    assert.equal(hash(output[stream]), footer[stream + 'Sha256']);
  }
  return output;
}

async function assertOwnedLeaf() {
  const absolute = await realpath(root);
  const offset = relative(tempBase, absolute);
  assert.equal(absolute, resolve(root));
  assert.ok(offset && offset !== '..' && !offset.startsWith('..' + sep) && !offset.includes(sep),
    'Cleanup must stay in the owned temporary leaf');
  assert.ok(offset.startsWith('nstu-gate01-'));
  assert.equal(hash(await readFile(markerPath)), hash(markerBytes), 'Exact fixture ownership marker must remain');
}
function fixtureChildPath(path) {
  const absolute = resolve(root, path), offset = relative(resolve(root), absolute);
  assert.ok(offset && offset !== '..' && !offset.startsWith('..' + sep) && !offset.startsWith(sep));
  return absolute;
}
async function walkFiles(directory, base) {
  const paths = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) paths.push(...await walkFiles(path, base));
    else if (entry.isFile()) paths.push(relative(base, path).split(sep).join('/'));
  }
  return paths;
}
async function copyFixture(target, origin, guarded) {
  for (const directory of directories) await cp(join(origin, directory), join(target, directory), { recursive: true });
  for (const path of files) {
    assert.equal((await stat(join(origin, path))).isFile(), true, 'Actual fixture source required: ' + path);
    await mkdir(dirname(join(target, path)), { recursive: true });
    if (guarded && path === copiedSelfTest) {
      await writeFile(join(target, path), "throw new Error('GATE01 unexpected fixture self-test reentry');\n" + await readFile(join(origin, path), 'utf8'));
    } else await cp(join(origin, path), join(target, path));
  }
}
async function checkRuntime() {
  for (const [name, expected] of Object.entries(ownerBaseline))
    assert.equal(hash(await readFile(join(root, '_owner-runtime/scripts/tests', name))), expected, 'Immutable copied owner changed');
}
async function currentInputs(cwd) {
  const inputs = {};
  for (const path of sourcePaths) {
    try { inputs[path] = hash(await readFile(join(cwd, path))); }
    catch (error) { if (error.code !== 'ENOENT') throw error; inputs[path] = 'ABSENT'; }
  }
  if (cwd === root) {
    inputs['.fixture-owner.json'] = hash(await readFile(markerPath));
    inputs['.fixture-git-empty.config'] = hash(await readFile(emptyGitConfig));
  }
  return inputs;
}
function fixtureGitEnvironment(childEnv = process.env, initialized = false) {
  const env = { ...childEnv };
  for (const key of Object.keys(env)) if (/^GIT_/i.test(key)) delete env[key];
  env.GIT_CONFIG_NOSYSTEM = '1'; env.GIT_CONFIG_GLOBAL = emptyGitConfig;
  if (initialized) {
    env.GIT_DIR = join(root, '.git'); env.GIT_WORK_TREE = root;
    env.GIT_INDEX_FILE = join(root, '.git/index'); env.GIT_COMMON_DIR = join(root, '.git');
  }
  return env;
}
async function invoke(command, args, env = process.env, options = {}) {
  await assertOwnedLeaf(); await checkRuntime();
  const cwd = options.cwd || root;
  assert.equal(await realpath(cwd), resolve(cwd));
  if (cwd !== root) fixtureChildPath(relative(root, cwd));
  const entry = { id: attempts.length + 1, label: options.label || 'owned entrypoint invocation', state: 'pending', result: null, error: null };
  attempts.push(entry);
  try {
    const sourceBinding = await copiedOwner.makeSourceBinding(await currentInputs(cwd));
    const gitScoped = command === 'git' || command === bash;
    const childEnv = command === bash ? fixtureGitEnvironment(env, true) : env;
    const result = await copiedOwner.runOwned({ application: command, argv: args, cwd, childEnv,
      controllerRuntime: parentRuntime, sourceBinding, mode: options.mode || 'normal', readiness: options.readiness,
      label: entry.label });
    entry.result = summary(result); entry.state = result.cleanup?.confirmed === true ? 'confirmed' : 'unconfirmed';
    lastResult = result;
    // Source-progress markers keep the native receipt visible before large writes.
    console.log('GATE02_RESULT_READY ' + JSON.stringify({ invocationId: entry.id, executionKind: result.execution.kind,
      rawWorkerExitDWORD: result.execution.rawWorkerExitDWORD, effectiveExitCode: result.execution.effectiveExitCode,
      cleanupConfirmed: result.cleanup.confirmed, stdoutBytes: result.output.stdout.length, stderrBytes: result.output.stderr.length,
      stdoutSha256: entry.result.output.stdoutSha256, stderrSha256: entry.result.output.stderrSha256 }));
    console.log('GATE02_INVOCATION ' + JSON.stringify({ id: entry.id, label: entry.label, state: entry.state, result: entry.result,
      ...(gitScoped ? { fixtureGitIsolation: { cwd, markerSha256: hash(markerBytes), relocationEnvironmentRemovedOrBound: true, privateGlobalConfig: emptyGitConfig } } : {}) }));
    const failedOwner = entry.label === 'copied current55 owner suite retaining original49' && result.execution.effectiveExitCode !== 0;
    if (failedOwner) {
      let lastCase = null, stderrTail = result.output.stderr.subarray(-768);
      try {
        const row = JSON.parse(result.output.stderr.toString('utf8').split(/\r?\n/).filter(line => line.startsWith('{"label":')).at(-1));
        const inner = Array.isArray(row.livenessEvidence?.results) ? row.livenessEvidence.results.at(-1)?.result : null;
        const innerCause = inner?.execution?.primaryErrors?.[0]?.message;
        const cause = typeof innerCause === 'string' ? innerCause : row.execution?.primaryErrors?.[0]?.message;
        lastCase = { label: typeof row.label === 'string' ? row.label.slice(0, 160) : null,
          executionKind: typeof row.execution?.kind === 'string' ? row.execution.kind.slice(0, 64) : null,
          primaryError: typeof cause === 'string' ? cause.slice(0, 192) : null };
        if (inner && typeof inner === 'object' && !Array.isArray(inner)) {
          const operations = Array.isArray(inner.cleanup?.operations) ? inner.cleanup.operations : [];
          const nativeError = inner.execution?.primaryErrors?.[0]?.win32Error;
          lastCase.innerProbe = { executionKind: typeof inner.execution?.kind === 'string' ? inner.execution.kind.slice(0, 64) : null,
            primaryWin32Error: Number.isInteger(nativeError) && nativeError >= 0 && nativeError <= 0xFFFFFFFF ? nativeError : null,
            deniedOpenObserved: operations.some(operation => operation?.operation === 'OpenProcess' && operation.resourceRole === 'descendant-liveness' && operation.ok === false && operation.win32Error === 5),
            daclSet: operations.some(operation => operation?.operation === 'SetKernelObjectSecurity' && operation.resourceRole === 'test-owned-descendant-dacl' && operation.ok === true),
            daclRestored: operations.some(operation => operation?.operation === 'SetKernelObjectSecurity' && operation.resourceRole === 'test-owned-descendant-dacl-restore' && operation.ok === true),
            cleanupConfirmed: inner.cleanup?.confirmed === true };
        }
        if (typeof row.livenessOutput?.stderrBase64 === 'string') {
          const encodedTail = row.livenessOutput.stderrBase64.slice(-1024), decodedTail = Buffer.from(encodedTail, 'base64');
          if (decodedTail.length && decodedTail.toString('base64') === encodedTail) stderrTail = decodedTail.subarray(-768);
        }
      } catch { /* Stderr is opaque diagnostic data; malformed rows cannot change acceptance. */ }
      console.error('GATE02_OWNER_FAILURE ' + JSON.stringify({ invocationId: entry.id, executionKind: result.execution.kind,
        rawWorkerExitDWORD: result.execution.rawWorkerExitDWORD, cleanupConfirmed: result.cleanup.confirmed, lastCase,
        stderrTail: stderrTail.toString('utf8') }));
    }
    if (failedOwner && result.output.stdout.length === 0) {
      await assertOwnedLeaf();
      entry.outputFiles = { stdoutPath: join(root, 'owner-failure-' + entry.id + '.stdout.bin'),
        stderrPath: join(root, 'owner-failure-' + entry.id + '.stderr.bin') };
      await writeFile(entry.outputFiles.stdoutPath, result.output.stdout, { flag: 'wx' });
      await writeFile(entry.outputFiles.stderrPath, result.output.stderr, { flag: 'wx' });
      console.log('GATE02_OUTPUT_FILES ' + JSON.stringify({ invocationId: entry.id, ...entry.outputFiles,
        stdoutSha256: hash(result.output.stdout), stderrSha256: hash(result.output.stderr) }));
    } else emitOwnedOutput(entry.id, result.output);
    console.log('GATE02_OUTPUT_COMPLETE ' + JSON.stringify({ invocationId: entry.id }));
    await checkRuntime();
    console.log('GATE02_RUNTIME_COMPLETE ' + JSON.stringify({ invocationId: entry.id }));
    return result;
  } catch (error) {
    entry.state = 'unconfirmed'; entry.error = safeError(error);
    console.error('GATE02_LAUNCH_FAILURE ' + JSON.stringify(entry));
    throw error;
  }
}
function nativeBoundary(result) {
  assert.equal(result.execution.kind, 'worker-exit', 'Timeout/setup failure is not intended failure proof');
  assert.equal(result.output.overflow, false, 'Output overflow is not intended failure proof');
  assert.equal(result.controller.signal, null, 'Signal termination is not intended failure proof');
  assert.equal(result.controller.exitCode, 0);
  assert.equal(result.execution.nativeRootWait.signaled, true);
  assert.equal(result.execution.nativeRootWait.result, 0);
  assert.equal(result.cleanup.confirmed, true);
  assert.equal(result.cleanup.activeProcessesLast, 0);
  assert.equal(result.cleanup.handlesClosedConfirmed, true);
  assert.equal(result.output.workerStdoutEOF, true); assert.equal(result.output.workerStderrEOF, true);
  assert.equal(result.controller.controlStdoutEOF, true); assert.equal(result.controller.controlStderrEOF, true);
  assert.equal(result.nodeTiming.forcedArmedNodeMs, undefined);
}
function positive(result) {
  nativeBoundary(result);
  assert.equal(result.execution.rawWorkerExitDWORD, 0);
  assert.equal(result.execution.effectiveExitCode, 0);
}
function rejected(result, diagnostic) {
  nativeBoundary(result);
  assert.notEqual(result.execution.rawWorkerExitDWORD, 0, 'A rejected verification must return nonzero');
  assert.equal(result.execution.effectiveExitCode, 1);
  const output = outputText(result);
  assert.match(output, diagnostic);
  assert.match(output, /LOCAL GATE: FAIL/);
  assert.doesNotMatch(output, /LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
  assert.doesNotMatch(output, /PASS  required current-source suites/,
    'A failed required child must not be accepted before entrypoint self-tests');
  assert.doesNotMatch(output, /GATE01 unexpected fixture self-test reentry/,
    'A recursion guard failure is not intended helper-boundary failure proof');
}
function withoutRuntime(filename) {
  const env = { ...process.env };
  const pathKey = Object.keys(env).find(key => key.toUpperCase() === 'PATH');
  const entries = (env[pathKey] || '').split(delimiter).filter(entry => !existsSync(join(entry, filename)));
  for (const key of Object.keys(env)) if (key.toUpperCase() === 'PATH') delete env[key];
  env.PATH = entries.join(delimiter); return env;
}
async function changed(path, bytes, scenario) {
  const filename = fixtureChildPath(path), original = await readFile(filename);
  const mutation = { path, originalSha256: hash(original), changedSha256: null, restored: false };
  mutations.push(mutation);
  const backup = join(root, '_source-backups', 'mutation-' + mutations.length + '.bin');
  await mkdir(dirname(backup), { recursive: true }); await writeFile(backup, original);
  try {
    if (bytes === null) await rm(filename); else await writeFile(filename, bytes(original));
    mutation.changedSha256 = bytes === null ? 'ABSENT' : hash(await readFile(filename));
    await scenario();
  } finally {
    if (attempts.every(entry => entry.state === 'confirmed')) {
      await writeFile(filename, original); mutation.restored = true;
      assert.equal(hash(await readFile(filename)), mutation.originalSha256);
    } else mutation.preservedForReconciliation = true;
  }
}
function pass(name, details = {}) {
  const record = details.kind === 'workflow-contract' ? { label: name, kind: details.kind, evidence: details.evidence } :
    { label: name, kind: 'invocation', invocationId: attempts.at(-1).id, result: summary(lastResult), ...details };
  caseRecords.push(record); console.log('GATE02_OUTER_CASE ' + JSON.stringify(record));
  console.log('PASS: ' + name); ++passed;
}
function ownerCohort(result, expectedBundle) {
  const stdout = result.output.stdout.toString('utf8');
  const rows = stdout.split(/\r?\n/).filter(line => line.startsWith('{"label":')).map(line => JSON.parse(line)).filter(row => row.schemaVersion === 1 && row.execution);
  assert.equal(rows.length, 55); assert.equal(new Set(rows.map(row => row.label)).size, 55);
  assert.equal(hash(Buffer.from(JSON.stringify(rows.slice(0, 49).map(row => row.label)))), original49LabelsSHA256);
  assert.deepEqual(rows.slice(49).map(row => row.label), livenessLabels);
  assert.ok(rows.slice(49).every(row => row.livenessEvidence?.observedOnly === true && row.livenessEvidence.results.length > 0));
  assert.ok(rows.every(row => row.cleanup.confirmed === true));
  const binding = JSON.parse(stdout.split('OWNER_SOURCE_BINDING ')[1].split(/\r?\n/)[0]);
  assert.deepEqual(binding.runtimeBundle, expectedBundle);
  const zero = rows.find(row => row.logicalStartupEvidence?.case === 'zero-terminal').logicalStartupEvidence;
  assert.equal(zero.nativeTerminal.execution.kind, 'worker-exit');
  assert.equal(zero.nativeTerminal.execution.rawWorkerExitDWORD, 0);
  assert.equal(zero.nativeTerminal.execution.nativeRootWait.signaled, true);
  assert.equal(zero.nativeTerminal.execution.nativeRootWait.result, 0);
  assert.equal(zero.inner.execution.kind, 'setup-failure'); assert.equal(zero.inner.execution.effectiveExitCode, 1);
  assert.deepEqual(zero.inner.nativeExecution, zero.nativeTerminal.execution);
  if (zero.inner.nodeTiming.stopWrite.state === 'error') assert.equal(zero.inner.cleanup.confirmed, false);
  return { count: 55, original49LabelsSHA256, labels: rows.map(row => row.label), runtimeBundle: binding.runtimeBundle,
    stdoutOnly: true, stdoutSha256: hash(result.output.stdout), stderrSha256: hash(result.output.stderr),
    mandatoryNativeZero: { kind: 'worker-exit', rawDWORD: 0, signaledWait: true, localKind: zero.inner.execution.kind,
      stopWriteState: zero.inner.nodeTiming.stopWrite.state, nodeCleanupConfirmed: zero.inner.cleanup.confirmed,
      nativeCleanupConfirmed: zero.nativeTerminal.cleanup.confirmed } };
}

try {
  assert.equal(process.platform, 'win32'); assert.equal(process.arch, 'x64');
  await import('./verify-required-regressions-path-tests.mjs');
  await copyFixture(root, repo, true);
  sourcePaths = [...files];
  for (const directory of directories) sourcePaths.push(...await walkFiles(join(root, directory), root));
  sourcePaths = [...new Set(sourcePaths)].sort();
  const runtimeDirectory = join(root, '_owner-runtime/scripts/tests');
  await mkdir(runtimeDirectory, { recursive: true }); ownerBaseline = {};
  for (const name of ownerFiles) {
    const original = await readFile(join(repo, 'scripts/tests', name));
    await cp(join(root, 'scripts/tests', name), join(runtimeDirectory, name));
    ownerBaseline[name] = hash(original);
    assert.equal(hash(await readFile(join(root, 'scripts/tests', name))), ownerBaseline[name]);
    assert.equal(hash(await readFile(join(runtimeDirectory, name))), ownerBaseline[name]);
  }
  copiedOwner = await import(pathToFileURL(join(runtimeDirectory, 'owned-regression-process.mjs')));
  parentRuntime = await copiedOwner.resolveControllerRuntime(process.env);
  positive(await invoke('git', ['init', '--quiet'], fixtureGitEnvironment(), { label: 'fixture-only git init' }));
  assert.equal((await stat(join(root, '.git'))).isDirectory(), true);
  assert.equal(await realpath(join(root, '.git')), resolve(root, '.git'));
  positive(await invoke('git', ['add', '.'], fixtureGitEnvironment(), { label: 'fixture-only git add' }));

  const ownerResult = await invoke(process.execPath, [join(runtimeDirectory, 'owned-regression-process-tests.mjs')], process.env, { label: 'copied current55 owner suite retaining original49' });
  const cohort = ownerCohort(ownerResult, ownerBaseline);
  console.log('GATE02_OWNER_COHORT ' + JSON.stringify({ invocationId: attempts.at(-1).id, phase: 'initial', cohort }));
  positive(ownerResult); assert.match(ownerResult.output.stdout.toString(), /OWNED PROCESS TESTS: PASS \(55 cases\)/);
  pass('actual copied owner suite passes55 cases retaining original49 from immutable sibling bytes', { cohort });

  const controlEnv = { ...process.env };
  for (const key of Object.keys(controlEnv)) if (key.toUpperCase() === 'PSMODULEPATH') delete controlEnv[key];
  controlEnv.PSModulePath = join(root, 'incompatible-parent-modules');
  const control = await invoke(process.execPath, [helper], controlEnv);
  positive(control); assert.match(outputText(control), /REQUIRED REGRESSIONS: PASS/);
  pass('actual current suites pass with runtime-owned PowerShell module paths');
  const reentry = await invoke(process.execPath, [copiedSelfTest]);
  nativeBoundary(reentry); assert.equal(reentry.execution.rawWorkerExitDWORD, 1);
  assert.match(outputText(reentry), /GATE01 unexpected fixture self-test reentry/);
  assert.doesNotMatch(outputText(reentry), /GATE ENTRYPOINT TESTS: PASS|OWNED PROCESS TESTS: PASS/);
  pass('owned negative-fixture self-test reentry fails explicitly');
  for (const path of ['tests/exam_runner_template_tests.mjs', 'tests/ui_desktop_token_tests.mjs', 'scripts/generate-ui-desktop-tokens.mjs',
    'packaging/test-exam-origin-contract.ps1', 'packaging/test-exam-package-staging.ps1',
    'tests/programming_web_tests.mjs', 'tools/cs-judge/broker.py', 'packaging/test-cs-policy.ps1']) {
    await changed(path, null, async () => { rejected(await invoke(bash, [gate]), /Required input missing/); pass('standard local gate rejects omitted ' + path); });
  }
  await changed('server/app/ui_desktop_tokens.hpp', bytes => Buffer.concat([bytes, Buffer.from('\n// stale generated bytes\n')]), async () => {
    rejected(await invoke(bash, [gate]), /Stale output: server\/app\/ui_desktop_tokens\.hpp/); pass('standard local gate rejects stale generated desktop tokens');
  });
  await changed('tests/exam_runner_template_tests.mjs', bytes => Buffer.concat([bytes, Buffer.from("\nassert.fail('GATE01 injected assertion failure');\n")]), async () => {
    rejected(await invoke(bash, [gate]), /GATE01 injected assertion failure/); pass('standard local gate propagates an actual suite assertion failure');
  });
  await changed('tools/cs-judge/test_broker.py', bytes => Buffer.concat([bytes, Buffer.from("\nraise AssertionError('GATE CS injected Python assertion failure')\n")]), async () => {
    rejected(await invoke(bash, [gate]), /GATE CS injected Python assertion failure/); pass('standard local gate propagates an actual Python suite assertion failure');
  });
  for (const code of [23, 256]) {
    await changed('packaging/test-exam-package-staging.ps1', bytes => {
      const source = bytes.toString('utf8');
      assert.equal(source.match(/\nexit 0\s*$/g)?.length, 1, 'Exactly one reachable final staging success exit follows all actual checks/finally');
      return source.replace(/\nexit 0\s*$/, '\nexit ' + code + '\n');
    }, async () => { rejected(await invoke(bash, [gate]), new RegExp('test-exam-package-staging\\.ps1.*exit ' + code)); pass('standard local gate rejects actual staging child exit ' + code); });
  }
  rejected(await invoke(bash, [gate], withoutRuntime('node.exe')), /node is not on PATH/); pass('standard local gate rejects missing Node');
  rejected(await invoke(bash, [gate], withoutRuntime('pwsh.exe')), /Required runtime.*pwsh\.exe/); pass('standard local gate rejects missing PowerShell 7');
  rejected(await invoke(bash, [gate], withoutRuntime('powershell.exe')), /Required runtime.*powershell\.exe/); pass('standard local gate rejects missing Windows PowerShell 5.1');
  for (const name of ownerFiles) {
    const path = 'scripts/tests/' + name;
    await changed(path, null, async () => { rejected(await invoke(bash, [gate]), new RegExp('Required input missing: ' + path.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))); pass('standard local gate rejects omitted ' + path); });
  }

  const faultRoot = fixtureChildPath('_owner-fault-repo'); await mkdir(faultRoot);
  await copyFixture(faultRoot, repo, false);
  const faultTest = join(faultRoot, 'scripts/tests/owned-regression-process-tests.mjs');
  const originalFaultTest = await readFile(faultTest, 'utf8');
  const successLine = "    console.log('OWNED PROCESS TESTS: PASS (' + records.length + ' cases)');";
  assert.equal(originalFaultTest.split(successLine).length, 2);
  await writeFile(faultTest, originalFaultTest.replace(successLine, "    assert.fail('GATE02 injected owner-suite assertion failure');\n" + successLine));
  const faultBundle = { ...ownerBaseline, 'owned-regression-process-tests.mjs': hash(await readFile(faultTest)) };
  const fault = await invoke(process.execPath, [join(faultRoot, copiedSelfTest)], process.env, { cwd: faultRoot, label: 'actual unguarded final55-assertion fault entrypoint retaining original49' });
  nativeBoundary(fault); assert.notEqual(fault.execution.rawWorkerExitDWORD, 0); assert.equal(fault.execution.effectiveExitCode, 1);
  assert.doesNotMatch(outputText(fault), /OWNED PROCESS TESTS: PASS|GATE ENTRYPOINT TESTS: PASS|LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
  const faultLines = fault.output.stdout.toString().split(/\r?\n/);
  const faultCohort = JSON.parse(faultLines.find(line => line.startsWith('GATE02_OWNER_COHORT ')).slice('GATE02_OWNER_COHORT '.length));
  assert.equal(faultCohort.cohort.count, 55); assert.equal(faultCohort.cohort.original49LabelsSHA256, original49LabelsSHA256); assert.deepEqual(faultCohort.cohort.runtimeBundle, faultBundle);
  const nested = faultLines.filter(line => line.startsWith('GATE02_INVOCATION ')).map(line => JSON.parse(line.slice('GATE02_INVOCATION '.length)));
  assert.equal(nested.length, 3);
  assert.equal(nested.at(-1).result.execution.kind, 'worker-exit'); assert.notEqual(nested.at(-1).result.execution.rawWorkerExitDWORD, 0);
  assert.equal(nested.at(-1).result.cleanup.confirmed, true);
  const nestedOutput = decodeOwnedOutput(faultLines, nested.at(-1).id);
  assert.ok(nestedOutput);
  const faultStdout = nestedOutput.stdout, faultStderr = nestedOutput.stderr;
  assert.equal(hash(faultStdout), nested.at(-1).result.output.stdoutSha256);
  assert.equal(hash(faultStderr), nested.at(-1).result.output.stderrSha256);
  assert.match(faultStderr.toString(), /GATE02 injected owner-suite assertion failure/);
  assert.doesNotMatch(Buffer.concat([faultStdout, faultStderr]).toString(), /OWNED PROCESS TESTS: PASS|GATE ENTRYPOINT TESTS: PASS|LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
  const faultRows = faultStdout.toString().split(/\r?\n/).filter(line => line.startsWith('{"label":')).map(line => JSON.parse(line));
  const faultDiagnosticRows = faultStderr.toString().split(/\r?\n/).filter(line => line.startsWith('{"label":')).map(line => JSON.parse(line));
  assert.equal(faultRows.length, 55); assert.equal(faultDiagnosticRows.length, 55);
  assert.equal(hash(Buffer.from(JSON.stringify(faultRows.slice(0, 49).map(row => row.label)))), original49LabelsSHA256);
  assert.deepEqual(faultRows.slice(49).map(row => row.label), livenessLabels);
  assert.deepEqual(faultDiagnosticRows.map(row => row.label), faultRows.map(row => row.label));
  const preservedFaultFixture = faultLines.find(line => line.startsWith('Owned entrypoint fixture: '))?.slice('Owned entrypoint fixture: '.length);
  assert.ok(preservedFaultFixture, 'Failed entrypoint must identify its actual fixture');
  assert.ok(fault.output.stderr.toString().split(/\r?\n/).includes('PRESERVED OWNED FIXTURE: ' + preservedFaultFixture),
    'Primary entrypoint failure must preserve its fixture even when every owned attempt is confirmed');
  assert.equal(await realpath(preservedFaultFixture), resolve(preservedFaultFixture));
  const preservedFaultLeaf = relative(tempBase, preservedFaultFixture);
  assert.ok(preservedFaultLeaf.startsWith('nstu-gate01-') && !preservedFaultLeaf.includes(sep));
  assert.equal(hash(await readFile(join(preservedFaultFixture, '.fixture-owner.json'))), nested[0].fixtureGitIsolation.markerSha256);
  const faultReceipt = JSON.parse(await readFile(join(preservedFaultFixture, 'preservation-receipt.json')));
  assert.equal(faultReceipt.root, preservedFaultFixture);
  assert.ok(faultReceipt.primary?.message && faultReceipt.primary.stack, 'Primary entrypoint error must remain in the receipt');
  assert.equal(faultReceipt.attempts.length, 3);
  assert.ok(faultReceipt.attempts.every(entry => entry.state === 'confirmed' && entry.result.cleanup.confirmed === true));
  assert.deepEqual(faultReceipt.attempts.map(entry => entry.result), nested.map(entry => entry.result));
  const preservedOwnerLines = faultStderr.toString().split(/\r?\n/).filter(line => line.startsWith('PRESERVED OWNED FIXTURE: '));
  assert.equal(preservedOwnerLines.length, 1, 'Injected owner primary failure must preserve its own fixture');
  const preservedOwnerFixture = preservedOwnerLines[0].slice('PRESERVED OWNED FIXTURE: '.length);
  assert.equal(await realpath(preservedOwnerFixture), resolve(preservedOwnerFixture));
  const preservedOwnerLeaf = relative(tempBase, preservedOwnerFixture);
  assert.ok(preservedOwnerLeaf.startsWith('nstu-owned-') && !preservedOwnerLeaf.includes(sep));
  const ownerFaultReceipt = JSON.parse(await readFile(join(preservedOwnerFixture, 'preservation-receipt.json')));
  assert.equal(ownerFaultReceipt.fixture, preservedOwnerFixture);
  assert.equal(ownerFaultReceipt.primary.message, 'GATE02 injected owner-suite assertion failure');
  assert.match(ownerFaultReceipt.primary.stack, /GATE02 injected owner-suite assertion failure/);
  assert.equal(ownerFaultReceipt.ledger.length, 55);
  assert.ok(ownerFaultReceipt.ledger.every(entry => entry.state === 'confirmed' && entry.result.cleanup.confirmed === true));
  const firstOwnerAssertion = '    assert.equal(owned.handleListCount, 3);';
  assert.equal(originalFaultTest.split(firstOwnerAssertion).length, 2);
  await writeFile(faultTest, originalFaultTest.replace(firstOwnerAssertion,
    "    assert.fail('GATE02 injected early owner-suite assertion failure');\n" + firstOwnerAssertion));
  const earlyFault = await invoke(process.execPath, [join(faultRoot, copiedSelfTest)], process.env,
    { cwd: faultRoot, label: 'actual early-owner assertion preserves complete diagnostic bytes' });
  nativeBoundary(earlyFault); assert.notEqual(earlyFault.execution.rawWorkerExitDWORD, 0); assert.equal(earlyFault.execution.effectiveExitCode, 1);
  assert.doesNotMatch(outputText(earlyFault), /OWNED PROCESS TESTS: PASS|GATE ENTRYPOINT TESTS: PASS|LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
  const earlyLines = earlyFault.output.stdout.toString().split(/\r?\n/);
  const earlyNested = earlyLines.filter(line => line.startsWith('GATE02_INVOCATION ')).map(line => JSON.parse(line.slice('GATE02_INVOCATION '.length)));
  assert.equal(earlyNested.length, 3); assert.ok(earlyNested.every(entry => entry.state === 'confirmed' && entry.result.cleanup.confirmed === true));
  const earlyStderr = earlyFault.output.stderr.toString();
  const earlyDiagnosticLine = earlyStderr.split(/\r?\n/).find(line => line.startsWith('GATE02_OWNER_FAILURE '));
  assert.ok(earlyDiagnosticLine, 'Failed owner must name its actual case and error before a missing stdout cohort masks it');
  assert.ok(Buffer.byteLength(earlyDiagnosticLine) <= 8192, 'Owner failure console diagnostic must remain bounded');
  assert.ok(earlyStderr.indexOf(earlyDiagnosticLine) < earlyStderr.indexOf('GATE02_PRIMARY_FAILURE '));
  const earlyDiagnostic = JSON.parse(earlyDiagnosticLine.slice('GATE02_OWNER_FAILURE '.length));
  assert.equal(earlyDiagnostic.invocationId, 3);
  assert.equal(earlyDiagnostic.lastCase.label, 'native exit0 and explicit x64 whitelist');
  assert.match(earlyDiagnostic.stderrTail, /GATE02 injected early owner-suite assertion failure/);
  const earlyFixture = earlyLines.find(line => line.startsWith('Owned entrypoint fixture: '))?.slice('Owned entrypoint fixture: '.length);
  assert.ok(earlyFixture); assert.equal(await realpath(earlyFixture), resolve(earlyFixture));
  const earlyLeaf = relative(tempBase, earlyFixture);
  assert.ok(earlyLeaf.startsWith('nstu-gate01-') && !earlyLeaf.includes(sep));
  assert.equal(hash(await readFile(join(earlyFixture, '.fixture-owner.json'))), earlyNested[0].fixtureGitIsolation.markerSha256);
  assert.ok(earlyStderr.split(/\r?\n/).includes('PRESERVED OWNED FIXTURE: ' + earlyFixture));
  const earlyOutputLine = earlyLines.find(line => line.startsWith('GATE02_OUTPUT_FILES '));
  assert.ok(earlyOutputLine, 'Early failed owner must preserve full stdout/stderr outside giant console lines');
  const earlyOutput = JSON.parse(earlyOutputLine.slice('GATE02_OUTPUT_FILES '.length));
  assert.equal(earlyOutput.invocationId, 3);
  assert.equal(earlyOutput.stdoutPath, join(earlyFixture, 'owner-failure-3.stdout.bin'));
  assert.equal(earlyOutput.stderrPath, join(earlyFixture, 'owner-failure-3.stderr.bin'));
  const earlyStdoutBytes = await readFile(earlyOutput.stdoutPath), earlyStderrBytes = await readFile(earlyOutput.stderrPath);
  assert.equal(earlyStdoutBytes.length, 0);
  assert.equal(hash(earlyStdoutBytes), earlyOutput.stdoutSha256); assert.equal(hash(earlyStderrBytes), earlyOutput.stderrSha256);
  assert.equal(hash(earlyStdoutBytes), earlyNested.at(-1).result.output.stdoutSha256);
  assert.equal(hash(earlyStderrBytes), earlyNested.at(-1).result.output.stderrSha256);
  assert.match(earlyStderrBytes.toString(), /GATE02 injected early owner-suite assertion failure/);
  const earlyReceipt = JSON.parse(await readFile(join(earlyFixture, 'preservation-receipt.json')));
  assert.equal(earlyReceipt.root, earlyFixture); assert.equal(earlyReceipt.attempts.length, 3);
  assert.ok(earlyReceipt.attempts.every(entry => entry.state === 'confirmed' && entry.result.cleanup.confirmed === true));
  assert.deepEqual(earlyReceipt.attempts.at(-1).outputFiles, { stdoutPath: earlyOutput.stdoutPath, stderrPath: earlyOutput.stderrPath });
  pass('actual copied owner-suite assertion fails before outer positives', { faultCohort: faultCohort.cohort, faultNestedInvocations: nested,
    earlyOwnerFailure: { diagnostic: earlyDiagnostic, outputFiles: earlyOutput, nestedInvocations: earlyNested } });

  const identity = copiedOwner.createRunIdentity();
  const actorPath = join(runtimeDirectory, 'owned-regression-process-tests.mjs'), actorHash = hash(await readFile(actorPath));
  await changed(helper, bytes => "import { spawn as gateProbeSpawn } from 'node:child_process';\n" +
    'gateProbeSpawn(process.execPath, ' + JSON.stringify([actorPath, '--actor', 'leaf', JSON.stringify({ identity })]) +
    ', {stdio:"inherit", windowsHide:true, detached:true});\n' +
    'await new Promise(() => setInterval(() => {}, 1000));\n' + bytes.toString('utf8'), async () => {
    const timeout = await invoke(bash, [gate], process.env, { mode: 'forced-2000ms', readiness: { ...identity, sourceScriptSha256: actorHash, sourceScriptPath: actorPath }, label: 'fixed2000 verified readiness timeout' });
    assert.equal(timeout.execution.kind, 'forced-timeout'); assert.equal(timeout.execution.effectiveExitCode, 1);
    assert.equal(timeout.cleanup.confirmed, true, 'Exact owned tree cleanup must complete');
    assert.equal(timeout.cleanup.activeProcessesLast, 0); assert.equal(timeout.cleanup.rootWaitSignaled, true);
    assert.equal(timeout.cleanup.peerWaitSignaled, true); assert.equal(timeout.cleanup.handlesClosedConfirmed, true);
    assert.equal(timeout.output.workerStdoutEOF, true); assert.equal(timeout.output.workerStderrEOF, true);
    assert.ok(timeout.nodeTiming.stopRequestedNodeMs - timeout.nodeTiming.forcedArmedNodeMs >= 2000);
    assert.equal(timeout.nodeTiming.forcedIntervalMs, 2000);
    const ready = timeout.events.filter(event => event.type === 'READY'); assert.equal(ready.length, 1);
    assert.notEqual(ready[0].peerPid, timeout.events.find(event => event.type === 'OWNED').rootPid);
    assert.doesNotMatch(outputText(timeout), /LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
    pass('timeout stops exact owned entrypoint and descendant; no suite failure proof');
  });
  const workflow = await readFile(join(repo, '.github/workflows/windows.yml'), 'utf8'), blocks = [];
  for (const job of ['build-test', 'internal-vm-build', 'dev-unprotected-build', 'webview2-host-build']) {
    const start = workflow.indexOf('  ' + job + ':\n'); assert.ok(start >= 0, job + ' must exist');
    const block = workflow.slice(start).split(/\n(?=  [a-z][a-z0-9-]*:\n)/)[0];
    for (const path of [helper, copiedSelfTest]) assert.ok(block.includes('node ' + path + '\n          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }'), job + ' must run ' + path + ' and propagate its native exit immediately');
    blocks.push({ job, blockSha256: hash(Buffer.from(block)), immediateExitChecks: true });
  }
  pass('all four Windows build variants require both checked entrypoints', { kind: 'workflow-contract', evidence: { workflowSha256: hash(Buffer.from(workflow)), blocks } });
  assert.equal(passed, 25); assert.equal(caseRecords.filter(record => record.kind === 'invocation').length, 24);
  console.log('GATE ENTRYPOINT TESTS: PASS (' + passed + ' cases)');
} catch (error) {
  primaryFailure = error; console.error('GATE02_PRIMARY_FAILURE ' + JSON.stringify(safeError(error))); throw error;
} finally {
  try {
    await assertOwnedLeaf();
    if (primaryFailure || attempts.some(entry => entry.state !== 'confirmed')) {
      const receipt = { root, attempts, mutations, primary: primaryFailure ? safeError(primaryFailure) : null };
      console.error('PRESERVED OWNED FIXTURE: ' + root);
      await writeFile(join(root, 'preservation-receipt.json'), JSON.stringify(receipt, null, 2) + '\n');
      if (!primaryFailure) throw new Error('Unconfirmed owned launch requires fixture preservation');
    } else await rm(root, { recursive: true, force: true });
  } catch (error) {
    if (primaryFailure) throw new AggregateError([primaryFailure, error], 'Primary entrypoint failure and fixture finalization failure');
    throw error;
  }
}
