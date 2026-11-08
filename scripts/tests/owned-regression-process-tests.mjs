import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { createConnection, createServer } from 'node:net';
import { spawn, spawnSync } from 'node:child_process';
import { mkdtemp, readFile, writeFile, mkdir, cp, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve, relative, sep, delimiter } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const self = fileURLToPath(import.meta.url);
const delay = ms => new Promise(done => setTimeout(done, ms));
const hash = bytes => createHash('sha256').update(bytes).digest('hex').toUpperCase();
const pipeFrame = value => {
  const bytes = Buffer.from(JSON.stringify(value));
  const frame = Buffer.alloc(4 + bytes.length);
  frame.writeUInt32LE(bytes.length); bytes.copy(frame, 4); return frame;
};
function argvFixtureEnvironment(parentEnv) {
  const env = { ...parentEnv };
  for (const key of Object.keys(env)) {
    const folded = key.toUpperCase();
    if (folded === 'PSMODULEPATH' || folded === 'NSTU_OWNED_VALUE') delete env[key];
  }
  return { ...env, NSTU_OWNED_VALUE: 'private-round-trip', PSModulePath: 'incompatible-parent-modules' };
}
function summarize(result) {
  return { ...result, output: { ...result.output, stdout: undefined, stderr: undefined,
    stdoutSha256: hash(result.output.stdout), stderrSha256: hash(result.output.stderr) } };
}
function createOwnerLedger() {
  const attempts = [];
  return {
    async invoke(label, launch) {
      const attempt = { label, state: 'pending', result: null, error: null };
      attempts.push(attempt);
      try {
        const result = await launch();
        attempt.result = summarize(result);
        attempt.state = result.cleanup?.confirmed === true ? 'confirmed' : 'unconfirmed';
        return result;
      } catch (error) {
        attempt.state = 'unconfirmed';
        attempt.error = { message: error.message, stack: error.stack };
        throw error;
      }
    },
    mayDelete() { return attempts.every(attempt => attempt.state === 'confirmed'); },
    snapshot() { return attempts.map(attempt => structuredClone(attempt)); }
  };
}
async function finalizeOwnedFixture(fixture, ledger, primary, prefix = 'nstu-owned-') {
  const absolute = resolve(fixture);
  const leaf = relative(resolve(tmpdir()), absolute);
  assert.ok(leaf.startsWith(prefix) && !leaf.includes(sep) && !leaf.includes('/') && !leaf.includes('\\'));
  const preservation = { fixture: absolute, ledger: ledger.snapshot(), primary: primary ? { message: primary.message, stack: primary.stack } : null };
  if (!ledger.mayDelete()) {
    await writeFile(join(absolute, 'preservation-receipt.json'), JSON.stringify(preservation, null, 2) + '\n');
    console.error('PRESERVED OWNED FIXTURE: ' + absolute);
    return { ...preservation, deletionAttempted: false };
  }
  await rm(absolute, { recursive: true, force: true });
  return { ...preservation, deletionAttempted: true };
}
async function actor(kind, encoded) {
  const config = encoded ? JSON.parse(encoded) : {};
  if (kind === 'liveness-leaf') {
    const live = join(process.cwd(), config.token + '.child-live');
    const release = join(process.cwd(), config.token + '.release-child');
    await writeFile(live, config.token);
    while (!existsSync(release)) await delay(2);
    assert.equal(await readFile(release, 'utf8'), config.token);
    process.exit(config.exitCode);
  }
  if (kind === 'liveness-foreign-root') {
    process.stdout.write('LIVENESS_FOREIGN_LIVE\n');
    await new Promise(() => setInterval(() => {}, 1000)); return;
  }
  if (kind === 'liveness-root') {
    if (['liveness-foreign', 'liveness-trace'].includes(config.scenario)) process.exit(0);
    spawn(process.execPath, [self, '--actor', 'liveness-leaf', JSON.stringify(config)], { stdio: 'inherit', windowsHide: true, detached: true });
    const deadline = Date.now() + 10000;
    while (!existsSync(join(process.cwd(), config.token + '.child-live'))) {
      assert.ok(Date.now() < deadline, 'Source-bound liveness child did not initialize'); await delay(2);
    }
    process.exit(0);
  }
  if (kind === 'liveness-scenario') {
    const owner = await import('./owned-regression-process.mjs');
    const runtime = await owner.resolveControllerRuntime(process.env);
    const ledger = createOwnerLedger(), fixtures = [], results = [];
    let primary, foreignControls, foreignPromise, foreignResult, foreignFinished = false;
    try {
      let foreignPID = null;
      if (config.scenario === 'liveness-foreign') {
        const foreign = await mkdtemp(join(tmpdir(), 'nstu-owned-liveness-')); fixtures.push(foreign);
        let resolveOwned;
        const owned = new Promise(done => { resolveOwned = done; });
        foreignPromise = ledger.invoke('independent foreign liveness owner', () => owner.runOwned({ application: process.execPath,
          argv: [self, '--actor', 'liveness-foreign-root', '{}'], cwd: foreign, controllerRuntime: runtime,
          onEvent: (frame, controls) => { if (frame.type === 'OWNED') { foreignPID = frame.rootPid; foreignControls = controls; resolveOwned(); } }
        })).then(result => { foreignFinished = true; return result; });
        await Promise.race([owned, foreignPromise.then(() => { throw new Error('Foreign fixture owner ended before ownership'); })]);
        assert.ok(foreignPID && foreignControls);
      }
      for (const exitCode of config.scenario === 'liveness-signaled' ? [0, 259] : [0]) {
        const fixture = await mkdtemp(join(tmpdir(), 'nstu-owned-liveness-')); fixtures.push(fixture);
        const ticket = { token: randomBytes(32).toString('hex'), scenario: config.scenario, exitCode, foreignPID };
        const bytes = Buffer.from(JSON.stringify(ticket)); await writeFile(join(fixture, '.liveness-owner.json'), bytes);
        const binding = await owner.makeSourceBinding({ '.liveness-owner.json': hash(bytes) });
        const inner = await ledger.invoke(config.scenario, () => owner.runOwned({ application: process.execPath,
          argv: [self, '--actor', 'liveness-root', JSON.stringify(ticket)], cwd: fixture, controllerRuntime: runtime,
          sourceBinding: binding, testFault: config.scenario }));
        results.push({ scenario: config.scenario, exitCode, result: summarize(inner) });
        const operations = inner.cleanup.operations;
        const empty = operations.find(operation => operation.operation === 'LivenessEmptyProof' && operation.returnValue?.decision === 'empty');
        const terminated = operations.find(operation => operation.operation === 'TerminateJobObject');
        if (config.scenario === 'liveness-snapshot-exit') {
          const unavailable = operations.find(operation => operation.operation === 'OpenProcess' && operation.resourceRole === 'descendant-liveness' && operation.win32Error === 87);
          assert.ok(unavailable, 'NATIVE LIVENESS NOT_EXERCISED: actual OpenProcess87 absent');
          assert.ok(operations.some(operation => operation.operation === 'ProcessListSnapshot' && operation.returnValue.pids.includes(unavailable.returnValue.pid)));
          assert.equal(inner.execution.kind, 'worker-exit'); assert.equal(inner.execution.rawWorkerExitDWORD, 0);
          assert.ok(empty && empty.controllerElapsedMs < terminated.controllerElapsedMs);
        } else if (config.scenario === 'liveness-live') {
          assert.equal(inner.execution.kind, 'surviving-descendant'); assert.equal(inner.execution.rawWorkerExitDWORD, 0);
          assert.equal(inner.execution.effectiveExitCode, 1);
          assert.ok(operations.some(operation => operation.resourceRole === 'descendant-liveness' && operation.operation === 'WaitForSingleObject' && operation.returnValue === 258));
        } else if (config.scenario === 'liveness-signaled') {
          assert.ok(operations.some(operation => operation.resourceRole === 'test-signaled-descendant' && operation.operation === 'WaitForSingleObject' && operation.returnValue === 0));
          assert.ok(operations.some(operation => operation.operation === 'TestSignaledDescendantExit' && operation.returnValue === exitCode));
          assert.equal(inner.execution.kind, 'worker-exit'); assert.equal(inner.execution.effectiveExitCode, 0); assert.ok(empty);
        } else if (config.scenario === 'liveness-foreign') {
          assert.ok(operations.some(operation => operation.operation === 'InjectedSnapshotCandidate' && operation.returnValue.origin === 'injected-candidate'));
          assert.ok(operations.some(operation => operation.operation === 'IsProcessInJob' && operation.resourceRole === 'descendant-liveness' && operation.returnValue.contained === false));
          assert.equal(operations.some(operation => operation.operation === 'WaitForSingleObject' && operation.resourceRole.startsWith('descendant-')), false);
          assert.equal(foreignFinished, false); assert.equal(inner.execution.effectiveExitCode, 0); assert.ok(empty);
        } else if (config.scenario === 'liveness-trace') {
          const trace = operations.find(operation => operation.operation === 'InjectedLivenessTrace').returnValue;
          assert.equal(trace.origin, 'injected-trace'); assert.equal(trace.nativeCalls, 0);
          assert.deepEqual(trace.vectors, { 'invalid-pid': 'failure', unavailable87: 'resample', 'access-denied5': 'failure',
            'membership-failed': 'failure', 'foreign-reused': 'resample', 'owned-live-reused': 'live', 'owned-signaled259': 'exited',
            'wait-failed': 'failure', 'root-unsignaled': 'failure', 'incomplete-snapshot': 'failure', 'root-only-active': 'resample',
            'unaccounted-list': 'resample', 'accounting-failed': 'failure', 'close-failed': 'failure', 'empty-shaped': 'empty' });
          assert.equal(inner.execution.effectiveExitCode, 0); assert.ok(empty);
          assert.equal(operations.some(operation => operation.operation === 'OpenProcess' && operation.ok === false), false);
        } else if (config.scenario === 'liveness-denied') {
          assert.ok(operations.some(operation => operation.operation === 'OpenProcess' && operation.resourceRole === 'descendant-liveness' && operation.win32Error === 5), 'NATIVE LIVENESS NOT_EXERCISED: actual access denied5 absent');
          assert.equal(inner.execution.kind, 'setup-failure');
          assert.ok(inner.execution.primaryErrors.some(error => error.win32Error === 5));
          assert.ok(operations.some(operation => operation.operation === 'SetKernelObjectSecurity' && operation.resourceRole === 'test-owned-descendant-dacl-restore' && operation.ok));
        } else assert.fail('Unknown source-bound liveness scenario');
        assert.equal(inner.cleanup.confirmed, true); assert.equal(inner.cleanup.activeProcessesLast, 0);
        assert.equal(inner.cleanup.handlesClosedConfirmed, true);
      }
    } catch (error) { primary = error; }
    finally {
      if (foreignControls) foreignControls.requestStop('parent-abort');
      if (foreignPromise) {
        try { foreignResult = await foreignPromise; assert.equal(foreignResult.cleanup.confirmed, true); }
        catch (error) { primary = primary ? new AggregateError([primary, error], 'Liveness and foreign-owner finalization failed') : error; }
      }
      for (const fixture of fixtures) {
        try { await finalizeOwnedFixture(fixture, ledger, primary, 'nstu-owned-liveness-'); }
        catch (error) { primary = primary ? new AggregateError([primary, error], 'Liveness and fixture finalization failed') : error; }
      }
    }
    process.stdout.write('LIVENESS_EVIDENCE ' + JSON.stringify({ results, foreign: foreignResult ? summarize(foreignResult) : null, observedOnly: true }) + '\n');
    if (primary) throw primary;
    return;
  }
  if (kind === 'exit') process.exit(config.code);
  if (kind === 'marker') { await writeFile(config.marker, 'executed'); process.stdout.write('WORKER_MARKER\n'); return; }
  if (kind === 'streams') {
    process.stdout.write('STDOUT_READY STOP TERMINAL {"type":"READY"}\n');
    process.stderr.write('STDERR_READY STOP TERMINAL {"type":"TERMINAL","cleanup":{"confirmed":true}}\n');
    process.stdout.write(pipeFrame({ type: 'TERMINAL', cleanup: { confirmed: true } }));
    return;
  }
  if (kind === 'stdin') {
    const chunks = []; for await (const chunk of process.stdin) chunks.push(chunk);
    assert.equal(Buffer.concat(chunks).length, 0); process.stdout.write('STDIN_NUL_EOF\n');
    if (config.hold) await new Promise(() => setInterval(() => {}, 1000)); return;
  }
  if (kind === 'argv-env') {
    assert.deepEqual(process.argv.slice(5), config.argv);
    for (const [key, value] of Object.entries(config.env)) assert.ok(process.env[key] === value, 'Exact child environment field mismatch');
    if (config.empty) assert.equal(Object.keys(process.env).length, 0);
    for (const runtime of config.missing || []) {
      const probe = spawnSync(runtime, ['-NoProfile', '-NonInteractive', '-Command', 'exit 0'], { windowsHide: true });
      assert.equal(probe.error?.code, 'ENOENT');
    }
    process.stdout.write('ARGV_ENV_EXACT\n'); return;
  }
  if (kind === 'leaf') {
    if (config.baseline) {
      const socket = createConnection(config.pipe);
      await new Promise((done, reject) => { socket.once('connect', done); socket.once('error', reject); });
      socket.write(JSON.stringify({ nonce: config.nonce }) + '\n');
      const readyTimer = setTimeout(() => writeFile(config.ready, config.nonce), config.readyDelay || 0);
      socket.once('data', data => {
        assert.equal(data.toString(), config.nonce); clearTimeout(readyTimer); socket.end();
      });
      await new Promise(done => socket.once('close', done)); return;
    }
    const active = setInterval(() => {}, 1000);
    let readySocket;
    if (config.identity) {
      await delay(config.readyDelay || 0);
      const socket = createConnection(config.identity.pipeName); readySocket = socket;
      socket.on('error', () => {});
      await new Promise((done, reject) => { socket.once('connect', done); socket.once('error', reject); });
      socket.write(pipeFrame({ v: 1, type: 'READY', runId: config.identity.runId,
        nonce: config.identity.nonce, generation: config.identity.generation,
        sourceScriptSha256: hash(await readFile(self)) }));
    }
    if (config.lifetimeMs) { await delay(config.lifetimeMs); readySocket?.destroy(); clearInterval(active); return; }
    await new Promise(() => {}); clearInterval(active); return;
  }
  if (kind === 'tree') {
    for (let index = 0; index < (config.count || 1); ++index) {
      const leaf = { ...config, identity: index === 0 ? config.identity : null };
      // Detached libuv children avoid its parent's private kill-on-exit job;
      // the native strict outer job still contains them and must prove cleanup.
      spawn(process.execPath, [self, '--actor', 'leaf', JSON.stringify(leaf)], { stdio: 'inherit', windowsHide: true, detached: true });
    }
    if (config.rootExit) { await delay(100); process.exit(0); }
    await new Promise(() => {}); return;
  }
  if (kind === 'overflow') {
    const bytes = Buffer.alloc(16384, 'X');
    for (let index = 0; index < 1024; ++index) {
      if (!process.stdout.write(bytes)) await new Promise(done => process.stdout.once('drain', done));
    }
    return;
  }
  if (kind === 'supervised') {
    const owner = await import(pathToFileURL(config.ownerFile || join(dirname(self), 'owned-regression-process.mjs')));
    let changed = false;
    const protocol = config.protocol ? (frame, controls) => {
      if (frame.type !== 'OWNED' || changed) return; changed = true;
      const stop = { v: 1, type: 'STOP', runId: controls.identity.runId, nonce: controls.identity.nonce,
        generation: controls.identity.generation, sourceBindingSha256: controls.sourceBindingSha256, seq: 2, reason: 'parent-abort' };
      if (config.protocol === 'wrong-nonce') stop.nonce = '0'.repeat(64);
      if (config.protocol === 'wrong-generation') stop.generation = '0'.repeat(16);
      if (config.protocol === 'replayed-sequence') stop.seq = 1;
      if (config.protocol === 'malformed-length') { controls.sendRawControl(Buffer.alloc(4)); return; }
      if (config.protocol === 'invalid-utf8') { controls.sendRawControl(Buffer.from([1, 0, 0, 0, 255])); return; }
      if (config.protocol === 'eof') { controls.endControl(); return; }
      if (config.protocol === 'duplicate-key') {
        const text = JSON.stringify(stop).replace('"reason":"parent-abort"', '"reason":"parent-abort","reason":"parent-abort"');
        const bytes = Buffer.from(text), prefix = Buffer.alloc(4); prefix.writeUInt32LE(bytes.length);
        controls.sendRawControl(Buffer.concat([prefix, bytes])); return;
      }
      controls.sendRawControl(owner.encodeControlFrame(stop));
    } : undefined;
    const result = await owner.runOwned({ application: process.execPath,
      argv: [self, '--actor', config.innerActor || 'marker', JSON.stringify(config.inner || { marker: config.marker })],
      cwd: config.cwd, childEnv: process.env, controllerRuntime: await owner.resolveControllerRuntime(process.env),
      testFault: config.testFault || null, budgetsMs: config.budgetsMs,
      onEvent: protocol || (config.cleanupDeadline ? (frame, controls) => { if (frame.type === 'OWNED') controls.requestStop('parent-abort'); } : undefined) });
    if (config.testFault?.startsWith('assign') || config.testFault?.startsWith('resume')) {
      assert.notEqual(result.execution.effectiveExitCode, 0);
      assert.equal(existsSync(config.marker), false);
      assert.ok(result.execution.primaryErrors.some(error => error.win32Error === 6));
    } else if (config.testFault) {
      assert.equal(result.cleanup.confirmed, false);
      assert.ok(result.cleanup.errors.some(error => error.win32Error === (config.testFault === 'query-invalid-class' ? 87 : 6)));
      if (config.innerActor === 'exit') assert.equal(result.execution.rawWorkerExitDWORD, config.inner.code);
    } else if (config.bootstrapFailure) {
      assert.equal(result.cleanup.confirmed, false);
      assert.equal(result.execution.kind, 'controller-failure');
    } else if (config.cleanupDeadline) {
      assert.equal(result.cleanup.confirmed, false);
      assert.equal(result.cleanup.deadlineExpired, true);
    } else if (config.protocol) {
      assert.equal(changed, true); assert.equal(result.execution.kind, 'protocol-failure');
      assert.equal(result.cleanup.confirmed, false); assert.ok(result.execution.primaryErrors.length > 0);
    }
    process.stdout.write('INNER_RESULT ' + JSON.stringify(summarize(result)) + '\n'); return;
  }
  if (kind === 'correction-readiness') {
    const owner = await import('./owned-regression-process.mjs');
    const identity = owner.createRunIdentity();
    const result = await owner.runOwned({ application: process.execPath,
      argv: [self, '--actor', config.case === 'root' ? 'leaf' : 'tree', JSON.stringify({ identity })],
      cwd: config.cwd, childEnv: process.env, controllerRuntime: await owner.resolveControllerRuntime(process.env),
      mode: 'forced-2000ms', readiness: { ...identity, sourceScriptSha256: hash(await readFile(self)) },
      budgetsMs: config.case === 'root' ? undefined : { startupMs: 2000 },
      testFault: config.case === 'native-late' ? 'ready-accept-after-startup' : null,
      testReadyDelivery: config.case === 'node-late' ? 'stall-past-node-startup' :
        config.case === 'node-stop' ? 'stop-before-node-ready-acceptance' : null });
    process.stdout.write('CORRECTION_INNER ' + JSON.stringify({ case: config.case, result: summarize(result) }) + '\n');
    assert.equal(result.cleanup.confirmed, true);
    assert.equal(result.execution.effectiveExitCode, 1);
    assert.equal(result.events.filter(frame => frame.type === 'READY').length, 0);
    assert.equal(result.nodeTiming.readyNodeMs, undefined);
    assert.equal(result.nodeTiming.forcedArmedNodeMs, undefined);
    assert.equal(result.nodeTiming.forcedDeadlineNodeMs, undefined);
    if (config.case === 'root') {
      assert.equal(result.execution.kind, 'setup-failure');
      assert.ok(result.execution.primaryErrors.some(error => /launched root, not a descendant/.test(error.message)));
    } else if (config.case === 'node-late') {
      assert.equal(result.execution.kind, 'setup-failure');
      assert.ok(result.nodeTiming.testReadyDecodedNodeMs < result.nodeTiming.startNodeMs + 2000);
      assert.ok(result.nodeTiming.readyRejectedNodeMs >= result.nodeTiming.startNodeMs + 2000);
      assert.ok(result.nativeExecution);
    } else if (config.case === 'native-late') {
      assert.equal(result.execution.kind, 'setup-failure');
      assert.equal(result.controllerTiming.readyElapsedMs, null);
      assert.ok(result.execution.primaryErrors.some(error => /startup deadline exceeded before acceptance/.test(error.message)));
      assert.ok(result.cleanup.operations.some(operation => operation.operation === 'TestReadyAcceptanceReleased' && operation.returnValue >= 2050));
    } else {
      assert.equal(config.case, 'node-stop');
      assert.equal(result.execution.kind, 'controller-failure');
      assert.ok(result.execution.primaryErrors.some(error => error.kind === 'readiness-after-stop'));
      assert.ok(result.nodeTiming.stopRequestedNodeMs <= result.nodeTiming.readyRejectedNodeMs);
    }
    return;
  }
  if (kind === 'correction-logical-startup') {
    const owner = await import('./owned-regression-process.mjs');
    const forced = config.case === 'forced-early';
    const identity = forced ? owner.createRunIdentity() : null;
    const workerKind = forced ? 'tree' : config.case === 'zero-terminal' ? 'exit' : 'leaf';
    const workerConfig = forced ? { identity } : config.case === 'zero-terminal' ? { code: 0 } :
      { lifetimeMs: config.case === 'normal-early' ? 3000 : 5000 };
    let nativeTerminal = null;
    const inner = await owner.runOwned({ application: process.execPath,
      argv: [self, '--actor', workerKind, JSON.stringify(workerConfig)], cwd: config.cwd, childEnv: process.env,
      controllerRuntime: await owner.resolveControllerRuntime(process.env), budgetsMs: { startupMs: 2000 },
      mode: forced ? 'forced-2000ms' : 'normal',
      readiness: identity ? { ...identity, sourceScriptSha256: hash(await readFile(self)) } : undefined,
      testStartupControl: config.case.endsWith('-early') ? 'early-startup-wake' :
        config.case === 'owned-after-stop' ? 'stop-before-node-owned-acceptance' : 'stall-normal-owned-past-startup',
      onEvent: frame => {
        if (frame.type === 'TERMINAL') nativeTerminal = structuredClone({ execution: frame.execution, cleanup: frame.cleanup,
          controllerTiming: frame.controllerTiming, output: frame.output, bootstrap: frame.bootstrap });
      } });
    const observed = { case: config.case, inner: summarize(inner), nativeTerminal };
    process.stdout.write('LOGICAL_STARTUP_INNER ' + JSON.stringify(observed) + '\n');
    assert.ok(nativeTerminal, 'Actual validated native terminal must be observed');
    assert.deepEqual(inner.nativeExecution, nativeTerminal.execution);
    if (config.case === 'normal-early') {
      assert.equal(inner.execution.kind, 'worker-exit');
      assert.equal(inner.execution.rawWorkerExitDWORD, 0);
      assert.equal(inner.execution.effectiveExitCode, 0);
      assert.equal(inner.cleanup.confirmed, true);
      assert.ok(inner.nodeTiming.startupTimerRearmCount >= 1);
      assert.ok(inner.nodeTiming.startupTimerFirstRearmNodeMs < inner.nodeTiming.ownedNodeMs);
      assert.ok(inner.nodeTiming.startupTimerCanceledNodeMs < inner.nodeTiming.startupCutoffNodeMs);
      assert.equal(inner.nodeTiming.startupTimerCancelReason, 'accepted-owned');
      assert.ok(inner.nodeTiming.terminalNodeMs > inner.nodeTiming.startupCutoffNodeMs);
      assert.equal(inner.nodeTiming.firstStopReason, undefined);
    } else if (config.case === 'forced-early') {
      assert.equal(inner.execution.kind, 'forced-timeout');
      assert.equal(inner.execution.effectiveExitCode, 1);
      assert.equal(inner.cleanup.confirmed, true);
      assert.ok(inner.nodeTiming.startupTimerRearmCount >= 1);
      assert.ok(inner.nodeTiming.startupTimerFirstRearmNodeMs < inner.nodeTiming.readyNodeMs);
      assert.equal(inner.nodeTiming.startupTimerCancelReason, 'accepted-ready');
      assert.ok(inner.nodeTiming.startupTimerCanceledNodeMs < inner.nodeTiming.startupCutoffNodeMs);
      assert.equal(inner.nodeTiming.firstStopReason, 'forced-2000ms');
      assert.ok(inner.nodeTiming.stopRequestedNodeMs - inner.nodeTiming.forcedArmedNodeMs >= 2000);
      const ready = inner.events.find(frame => frame.type === 'READY');
      const owned = inner.events.find(frame => frame.type === 'OWNED');
      assert.notEqual(ready.peerPid, owned.rootPid);
    } else {
      assert.equal(inner.execution.effectiveExitCode, 1);
      assert.equal(inner.nodeTiming.ownedNodeMs, undefined);
      assert.equal(inner.nodeTiming.forcedArmedNodeMs, undefined);
      assert.notEqual(inner.nodeTiming.startupTimerCancelReason, 'accepted-owned');
      assert.ok(inner.nodeTiming.ownedRejectedNodeMs >= inner.nodeTiming.testOwnedDecodedNodeMs);
      if (config.case === 'owned-after-stop') {
        assert.equal(inner.nodeTiming.firstStopReason, 'parent-abort');
        assert.equal(inner.execution.kind, 'controller-failure');
        assert.equal(inner.nodeTiming.startupViolationNodeMs, undefined);
        assert.equal(inner.cleanup.confirmed, true);
      } else {
        assert.equal(inner.execution.kind, 'setup-failure');
        assert.equal(inner.nodeTiming.firstStopReason, 'startup-deadline');
        assert.ok(inner.nodeTiming.testOwnedDecodedNodeMs < inner.nodeTiming.startupCutoffNodeMs);
        assert.ok(inner.nodeTiming.ownedRejectedNodeMs >= inner.nodeTiming.startupCutoffNodeMs + 50);
        assert.ok(inner.nodeTiming.startupViolationNodeMs >= inner.nodeTiming.startupCutoffNodeMs);
        assert.ok(['ok', 'error'].includes(inner.nodeTiming.stopWrite.state));
        if (config.case === 'zero-terminal') {
          // Mandatory genuine zero terminal: another native outcome is inconclusive.
          assert.equal(nativeTerminal.execution.kind, 'worker-exit');
          assert.equal(nativeTerminal.execution.rawWorkerExitDWORD, 0);
          assert.equal(nativeTerminal.execution.nativeRootWait.signaled, true);
          assert.equal(nativeTerminal.execution.nativeRootWait.result, 0);
          assert.equal(inner.nativeExecution.kind, 'worker-exit');
          assert.equal(inner.nativeExecution.rawWorkerExitDWORD, 0);
          if (inner.nodeTiming.stopWrite.state === 'error') {
            assert.equal(inner.cleanup.confirmed, false);
            assert.ok(inner.execution.primaryErrors.some(error => error.kind === 'controller-failure'));
          }
        } else {
          assert.equal(config.case, 'owned-late');
          assert.equal(inner.cleanup.confirmed, true);
        }
      }
    }
    return;
  }
  if (kind === 'correction-copied-preservation') {
    const preservedLeaf = await mkdtemp(join(tmpdir(), 'nstu-owned-preserve-'));
    const copiedDirectory = join(preservedLeaf, 'copied-owner'); await mkdir(copiedDirectory);
    const sourceHashes = {};
    for (const filename of ['owned-regression-process.mjs', 'owned-regression-process.ps1', 'owned-regression-process-tests.mjs']) {
      await cp(join(dirname(self), filename), join(copiedDirectory, filename));
      sourceHashes[filename] = hash(await readFile(join(copiedDirectory, filename)));
    }
    const marker = join(preservedLeaf, 'preserved.marker'); await writeFile(marker, 'G1 copied-owner preservation');
    const copiedOwner = await import(pathToFileURL(join(copiedDirectory, 'owned-regression-process.mjs')));
    const ledger = createOwnerLedger();
    const inner = await ledger.invoke('actual copied-owner unconfirmed cleanup', async () => copiedOwner.runOwned({ application: process.execPath,
      argv: [join(copiedDirectory, 'owned-regression-process-tests.mjs'), '--actor', 'exit', '{"code":0}'],
      cwd: preservedLeaf, childEnv: process.env, controllerRuntime: await copiedOwner.resolveControllerRuntime(process.env),
      testFault: 'close-null-handle' }));
    assert.equal(inner.execution.kind, 'worker-exit');
    assert.equal(inner.execution.rawWorkerExitDWORD, 0);
    assert.equal(inner.cleanup.confirmed, false);
    assert.ok(inner.cleanup.errors.some(error => error.win32Error === 6));
    let primary;
    try { assert.equal(inner.cleanup.confirmed, true, 'G1 named copied-owner unconfirmed outcome'); }
    catch (error) { primary = error; }
    assert.ok(primary);
    const finalization = await finalizeOwnedFixture(preservedLeaf, ledger, primary, 'nstu-owned-preserve-');
    const proof = { ...finalization, sourceHashes, inner: summarize(inner), markerSha256: hash(Buffer.from('G1 copied-owner preservation')) };
    process.stdout.write('COPIED_PRESERVATION ' + JSON.stringify(proof) + '\n');
    assert.equal(ledger.mayDelete(), false);
    assert.equal(existsSync(preservedLeaf), true);
    assert.equal(finalization.deletionAttempted, false);
    assert.equal(existsSync(join(preservedLeaf, 'preservation-receipt.json')), true);
    return;
  }
  if (kind === 'two-owners') {
    const owner = await import('./owned-regression-process.mjs');
    let bControls, bDone = false, readyB;
    const observed = new Promise(done => { readyB = done; });
    const identity = owner.createRunIdentity();
    const runtime = await owner.resolveControllerRuntime(process.env);
    const b = owner.runOwned({ application: process.execPath, argv: [self, '--actor', 'tree', JSON.stringify({ identity })],
      cwd: config.cwd, childEnv: process.env, controllerRuntime: runtime,
      readiness: { ...identity, sourceScriptSha256: hash(await readFile(self)) },
      onEvent: (frame, controls) => { if (frame.type === 'READY') { bControls = controls; readyB(); } }
    }).then(result => { bDone = true; return result; });
    let a;
    try {
      await observed;
      a = await owner.runOwned({ application: process.execPath, argv: [self, '--actor', 'tree', '{}'],
        cwd: config.cwd, childEnv: process.env, controllerRuntime: runtime,
        onEvent: (frame, controls) => {
          if (frame.type === 'OWNED') controls.sendRawControl(owner.encodeControlFrame({ v: 1, type: 'STOP', runId: identity.runId,
            nonce: identity.nonce, generation: identity.generation, sourceBindingSha256: controls.sourceBindingSha256, seq: 2, reason: 'parent-abort' }));
        } });
      assert.equal(a.execution.kind, 'protocol-failure'); assert.equal(a.cleanup.confirmed, false);
      assert.equal(bDone, false, 'Wrong identity must not terminate the other active owner');
    } finally { if (bControls) bControls.requestStop('parent-abort'); }
    const stoppedB = await b;
    assert.equal(stoppedB.cleanup.confirmed, true); assert.equal(stoppedB.execution.kind, 'controller-failure');
    process.stdout.write('TWO_OWNERS ' + JSON.stringify({ a: summarize(a), b: summarize(stoppedB) }) + '\n'); return;
  }
  if (kind === 'outside-readiness') {
    const owner = await import('./owned-regression-process.mjs');
    const runtime = await owner.resolveControllerRuntime(process.env), identity = owner.createRunIdentity();
    let ownedA, controlsB, bDone = false;
    const listeningA = new Promise(done => { ownedA = done; });
    const a = owner.runOwned({ application: process.execPath, argv: [self, '--actor', 'tree', '{}'], cwd: config.cwd,
      childEnv: process.env, controllerRuntime: runtime, mode: 'forced-2000ms',
      readiness: { ...identity, sourceScriptSha256: hash(await readFile(self)) },
      onEvent: frame => { if (frame.type === 'OWNED') ownedA(); } });
    await listeningA;
    const b = owner.runOwned({ application: process.execPath, argv: [self, '--actor', 'leaf', JSON.stringify({ identity })],
      cwd: config.cwd, childEnv: process.env, controllerRuntime: runtime,
      onEvent: (frame, controls) => { if (frame.type === 'OWNED') controlsB = controls; } }).then(value => { bDone = true; return value; });
    let rejected, stopped, failure;
    try {
      rejected = await a;
      assert.equal(rejected.execution.kind, 'setup-failure');
      assert.equal(rejected.events.filter(frame => frame.type === 'READY').length, 0);
      assert.equal(rejected.cleanup.confirmed, true, 'Foreign readiness peer must not become an owned cleanup wait target');
      assert.equal(bDone, false, 'Readiness rejection must not terminate the other owner');
    } catch (error) { failure = error; }
    finally { if (controlsB) controlsB.requestStop('parent-abort'); stopped = await b; }
    assert.equal(stopped.cleanup.confirmed, true);
    process.stdout.write('OUTSIDE_READINESS ' + JSON.stringify({ a: summarize(rejected), b: summarize(stopped) }) + '\n');
    if (failure) throw failure;
    return;
  }
  if (kind === 'pending-js') {
    const marker = join(config.cwd, 'pending-js-marker');
    const child = spawn(process.execPath, [self, '--actor', 'marker', JSON.stringify({ marker })], { stdio: 'ignore', windowsHide: true });
    const closed = new Promise(done => child.once('close', (status, signal) => done({ status, signal })));
    const deadline = performance.now() + 3000;
    let nativeExited = false;
    while (performance.now() < deadline) {
      // Signal zero queries the same retained ChildProcess native handle. It
      // never targets a saved numeric PID and is not the cleanup oracle.
      if (existsSync(marker) && child.kill(0) === false) { nativeExited = true; break; }
    }
    assert.equal(nativeExited, true); assert.equal(child.exitCode, null); assert.equal(child.signalCode, null);
    const observed = await closed; assert.equal(observed.status, 0); assert.equal(observed.signal, null);
    process.stdout.write('NATIVE_EXIT_BEFORE_JS_EVENT\n'); return;
  }
  throw new Error('Unknown owned test actor');
}

async function baselineRed() {
  const sourceFile = resolve(dirname(self), 'verify-required-regressions-tests.mjs');
  const source = await readFile(sourceFile, 'utf8');
  const begin = source.indexOf('async function invoke('), end = source.indexOf('\nfunction rejected(', begin);
  assert.ok(begin >= 0 && end > begin);
  const exact = source.slice(begin, end);
  assert.equal(exact.match(/assert.equal\(result.error, undefined/g)?.length, 1);
  const observed = exact.replace('  assert.equal(result.error, undefined', '  observe(result);\n  assert.equal(result.error, undefined');
  const leafRoot = await mkdtemp(join(tmpdir(), 'nstu-gate01-'));
  const fixtureActor = join(leafRoot, 'owned-regression-process-tests.mjs');
  await cp(self, fixtureActor);
  const unsafeCleanup = new Set();
  const commands = { encoding: 'utf8', maxBuffer: 256 * 1024, windowsHide: true };
  const bash = 'C:/Program Files/Git/bin/bash.exe';
  const records = [];
  const releaseProofs = [];
  let baselineFailure;
  try {
    for (const [label, readyDelay, rootExit] of [['launcher-exited-inherited-streams', 0, true], ['timer-before-descendant-ready', 3000, false]]) {
      const release = { label, confirmed: false }; releaseProofs.push(release);
      const nonce = randomBytes(16).toString('hex');
      const pipe = '\\\\.\\pipe\\nstu-baseline-' + nonce;
      const ready = join(leafRoot, 'ready-' + nonce);
      let connection;
      const server = createServer(socket => {
        let text = '';
        socket.on('data', chunk => {
          text += chunk.toString();
          if (text.endsWith('\n')) { assert.equal(JSON.parse(text).nonce, nonce); connection = socket; }
        });
      });
      await new Promise(done => server.listen(pipe, done));
      const configFile = join(leafRoot, 'config-' + nonce + '.json');
      await writeFile(configFile, JSON.stringify({ baseline: true, pipe, nonce, ready, readyDelay }));
      const shellScript = join(leafRoot, 'baseline-' + nonce + '.sh');
      const shellQuote = value => "'" + value.replaceAll("'", "'\\''") + "'";
      const leafCommand = shellQuote(process.execPath.replaceAll('\\', '/')) + ' ' + shellQuote(fixtureActor.replaceAll('\\', '/')) + " --actor leaf \"$(cat " + shellQuote(configFile.replaceAll('\\', '/')) + ')\" &\n';
      const waitReady = 'while [ ! -f ' + shellQuote(ready.replaceAll('\\', '/')) + ' ]; do sleep 0.02; done\n';
      await writeFile(shellScript, '#!/usr/bin/env bash\n' + leafCommand + (rootExit ? waitReady + 'exit 0\n' : 'wait\n'));
      let observedResult, withheld = 0;
      const rootClosures = [];
      const observedSpawn = (...args) => {
        const child = spawn(...args);
        rootClosures.push(new Promise(done => child.once('close', done)));
        return child;
      };
      const intercept = () => { ++withheld; return { status: 1, stderr: 'DIAGNOSTIC: numeric taskkill withheld', stdout: '' }; };
      const create = new Function('spawn', 'spawnSync', 'join', 'assert', 'root', 'unsafeCleanup', 'commands', 'observe', exact ? observed + '\nreturn invoke;' : '');
      const invoke = create(observedSpawn, intercept, join, assert, leafRoot, unsafeCleanup, commands, value => { observedResult = value; });
      let primary;
      try { await invoke(bash, [shellScript], process.env, 2000); }
      catch (error) { primary = error; }
      assert.ok(primary && observedResult);
      assert.equal(observedResult.timedOut, true);
      if (rootExit) { assert.equal(withheld, 0); assert.match(observedResult.error.message, /root exited with surviving streams/); }
      else { assert.equal(withheld, 1); assert.equal(existsSync(ready), false); }
      const preservation = source.slice(source.indexOf('  if (unsafeCleanup.size) throw new Error('), source.indexOf('  const offset = relative(', source.indexOf('} finally {')));
      let final;
      try { new Function('unsafeCleanup', 'root', preservation)(unsafeCleanup, leafRoot); }
      catch (error) { final = error; }
      assert.match(final.message, /Preserved owned fixture/);
      assert.doesNotMatch(final.message, /root exited with surviving streams|numeric taskkill withheld/);
      records.push({ label, sourceSha256: hash(Buffer.from(source)), invokeSha256: hash(Buffer.from(exact)),
        primary: primary.message, final: final.message, timerMs: 2000, descendantReadyAtTimeout: existsSync(ready),
        numericTaskkillExecuted: false, interceptedCleanupCalls: withheld,
        limits: 'Actual source function plus one result observation; numeric taskkill intercepted; cooperative own leaf stop, not native job qualification.' });
      for (let index = 0; !connection && index < 100; ++index) await delay(20);
      assert.ok(connection, 'Nonce-owned leaf must remain cooperatively reachable');
      const ended = new Promise(done => connection.once('close', done));
      connection.write(nonce); await ended;
      await new Promise(done => server.close(done));
      await Promise.race([Promise.all(rootClosures), delay(3000).then(() => { throw new Error('Own baseline root close not observed'); })]);
      release.confirmed = true;
    }
    for (const record of records) console.log(JSON.stringify(record));
    console.log('BASELINE RED: named existing-source failures reproduced; intended nonzero');
    process.exitCode = 1;
  } catch (error) {
    baselineFailure = error;
    throw error;
  } finally {
    const leaf = relative(resolve(tmpdir()), resolve(leafRoot));
    assert.ok(leaf.startsWith('nstu-gate01-') && !leaf.includes(sep));
    if (releaseProofs.length === 2 && releaseProofs.every(proof => proof.confirmed)) {
      await rm(leafRoot, { recursive: true, force: true, maxRetries: 10, retryDelay: 50 });
    } else {
      const proof = { releaseProofs, records, primary: baselineFailure ? { message: baselineFailure.message, stack: baselineFailure.stack } : null };
      console.error('PRESERVED BASELINE FIXTURE: ' + leafRoot);
      try { await writeFile(join(leafRoot, 'preservation-receipt.json'), JSON.stringify(proof, null, 2) + '\n'); }
      catch (error) { if (baselineFailure) throw new AggregateError([baselineFailure, error], 'Baseline failure and preservation receipt failure'); throw error; }
    }
  }
}

async function main() {
  assert.equal(process.platform, 'win32', 'Windows native owner qualification is required');
  assert.equal(process.arch, 'x64', 'x64 native ABI is required');
  const owner = await import('./owned-regression-process.mjs');
  const runtime = await owner.resolveControllerRuntime(process.env);
  const fixture = await mkdtemp(join(tmpdir(), 'nstu-owned-'));
  const records = [];
  const ledger = createOwnerLedger();
  let primaryFailure;
  async function run(label, options = {}, expectedFailure = false, captureResult = null) {
    const result = await ledger.invoke(label, () => owner.runOwned({ application: process.execPath,
      argv: [self, '--actor', 'exit', '{"code":0}'], cwd: fixture, childEnv: process.env,
      controllerRuntime: runtime, ...options }));
    records.push({ label, ...summarize(result) });
    let captureFailure;
    if (captureResult) {
      try { captureResult(result, records.at(-1)); }
      catch (error) { captureFailure = error; records.at(-1).evidenceCaptureError = { message: error.message, stack: error.stack }; }
    }
    if (!expectedFailure) {
      assert.equal(result.execution.effectiveExitCode, 0, label + ': ' + JSON.stringify(summarize(result)));
      assert.equal(result.cleanup.confirmed, true);
      assert.equal(result.cleanup.activeProcessesLast, 0);
      assert.equal(result.cleanup.handlesClosedConfirmed, true);
      assert.equal(result.controller.exitCode, 0);
    }
    if (captureFailure) throw captureFailure;
    return result;
  }
  const actorArgs = (kind, config) => [self, '--actor', kind, JSON.stringify(config)];
  try {
    const simple = await run('native exit0 and explicit x64 whitelist');
    const owned = simple.events.find(event => event.type === 'OWNED');
    assert.equal(owned.handleListCount, 3);
    assert.equal(owned.excludedHandlesNoninheritable, true);
    assert.equal(owned.rootInOwnedJob, true);
    assert.equal(owned.abi.startupInfoEx, 112); assert.equal(owned.abi.processInformation, 24);
    await run('native exited child observed while JS exit delivery remains pending', { argv: actorArgs('pending-js', { cwd: fixture }) });
    const terminalSample = { execution: simple.execution, cleanup: simple.cleanup, controllerTiming: simple.controllerTiming,
      output: { stdoutBytes: 0, stderrBytes: 0, overflow: false, workerStdoutEOF: true, workerStderrEOF: true }, bootstrap: simple.bootstrap };
    owner.validateControllerTerminal(terminalSample);
    for (const [target, value] of [['confirmed', 'true'], ['handlesClosedConfirmed', false], ['activeProcessesLast', 1]]) {
      const invalid = structuredClone(terminalSample); invalid.cleanup[target] = value;
      assert.throws(() => owner.validateControllerTerminal(invalid), /terminal|cleanup|boolean|native/i);
    }
    const invalidExit = structuredClone(terminalSample); invalidExit.execution.rawWorkerExitDWORD = 256;
    assert.throws(() => owner.validateControllerTerminal(invalidExit), /terminal|native|exit/i);
    const streams = await run('stdout/stderr control spoof bytes stay opaque', { argv: actorArgs('streams', {}) });
    assert.match(streams.output.stdout.toString(), /STDOUT_READY/);
    assert.match(streams.output.stderr.toString(), /STDERR_READY/);
    assert.equal(streams.events.filter(event => event.type === 'READY').length, 0);
    await run('worker stdin is NUL', { argv: actorArgs('stdin', {}) });
    let stdinStop = false;
    const stdinControl = await run('worker stdin cannot receive controller STOP', {
      argv: actorArgs('stdin', { hold: true }), onEvent: (frame, controls) => {
        if (frame.type === 'DATA' && frame.stream === 'stdout' && Buffer.from(frame.bytesBase64, 'base64').toString().includes('STDIN_NUL_EOF')) {
          stdinStop = true; controls.requestStop('parent-abort');
        }
      } }, true);
    assert.equal(stdinStop, true); assert.equal(stdinControl.cleanup.confirmed, true);
    for (const code of [256, 259]) {
      const result = await run('native raw exit ' + code, { argv: actorArgs('exit', { code }) }, true);
      assert.equal(result.execution.kind, 'worker-exit');
      assert.equal(result.execution.rawWorkerExitDWORD, code);
      assert.equal(result.execution.nativeRootWait.signaled, true);
      assert.equal(result.execution.effectiveExitCode, 1); assert.equal(result.cleanup.confirmed, true);
    }
    const edgeArgs = ['', 'space value', 'quote"value', 'tail\\', 'Tiếng Việt', '&|<>^%$()'];
    for (const inheritedKey of ['PSMODULEPATH', 'pSmOdUlEpAtH', 'PSModulePath']) {
      const parent = { PATH: 'fixture-path', KeepCase: 'untouched', [inheritedKey]: 'inherited-modules', nstu_owned_value: 'inherited-owned-value' };
      const before = structuredClone(parent), injected = argvFixtureEnvironment(parent);
      assert.deepEqual(parent, before);
      assert.deepEqual(injected, { PATH: 'fixture-path', KeepCase: 'untouched', NSTU_OWNED_VALUE: 'private-round-trip', PSModulePath: 'incompatible-parent-modules' });
      owner.validateEnvironment(injected);
    }
    assert.throws(() => owner.validateEnvironment({ PSMODULEPATH: 'x', PSModulePath: 'y' }), /duplicate environment key/);
    assert.throws(() => owner.validateEnvironment(argvFixtureEnvironment({ Path: 'x', PATH: 'y' })), /duplicate environment key/);
    const env = argvFixtureEnvironment(process.env);
    await run('Unicode/empty/quote/backslash/metachar argv and exact child env', {
      argv: [...actorArgs('argv-env', { argv: edgeArgs, env: { NSTU_OWNED_VALUE: env.NSTU_OWNED_VALUE, PSModulePath: env.PSModulePath } }), ...edgeArgs], childEnv: env });
    const environmentProbe = join(fixture, 'environment-probe.exe');
    const environmentCompile = join(fixture, 'compile-environment-probe.ps1');
    await writeFile(environmentCompile, `param([string] $Target)
$ErrorActionPreference = 'Stop'
$source = @'
using System;
using System.Runtime.InteropServices;
public static class EnvironmentProbe {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr GetEnvironmentStringsW();
  [DllImport("kernel32.dll", SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] static extern bool FreeEnvironmentStringsW(IntPtr block);
  public static int Main() {
    IntPtr block = GetEnvironmentStringsW();
    if (block == IntPtr.Zero) return 44;
    int count = 0, offset = 0;
    try {
      while (Marshal.ReadInt16(block, offset) != 0) {
        ++count;
        do { offset += 2; if (offset > 2097152) return 45; } while (Marshal.ReadInt16(block, offset) != 0);
        offset += 2;
      }
      Console.Write("ENV_KEY_COUNT=" + count);
    } finally { if (!FreeEnvironmentStringsW(block)) throw new Exception("Environment block release failed"); }
    return count == 0 ? 0 : 46;
  }
}
'@
$null = Add-Type -TypeDefinition $source -Language CSharp -OutputAssembly $Target -OutputType ConsoleApplication -ErrorAction Stop -WarningAction Stop
`);
    await run('compile private native environment fixture within the owned worker job', { application: runtime.exe,
      argv: ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', environmentCompile, '-Target', environmentProbe] });
    const emptyEnvironment = await run('empty Unicode environment block remains empty natively', {
      application: environmentProbe, argv: [], childEnv: {} });
    assert.equal(emptyEnvironment.output.stdout.toString(), 'ENV_KEY_COUNT=0');
    const poisonedRuntime = await owner.resolveControllerRuntime({ ...process.env, PSModulePath: 'invalid-parent-module-path' });
    await run('parent runtime poison is sanitized independently', { controllerRuntime: poisonedRuntime });
    const unicodeDirectory = join(fixture, 'Tiếng Việt space'); await mkdir(unicodeDirectory);
    const unicodeExe = join(unicodeDirectory, 'node space.exe'); await cp(process.execPath, unicodeExe);
    await run('actual Unicode and space executable path', { application: unicodeExe,
      argv: [...actorArgs('argv-env', { argv: edgeArgs, env: {} }), ...edgeArgs] });
    for (const childEnv of [[{ key: 'Path', value: 'x' }, { key: 'PATH', value: 'y' }], [{ key: 'X', value: '\0' }], [{ key: 'X', value: '\uD800' }]]) {
      const invalid = await owner.runOwned({ application: process.execPath, argv: actorArgs('exit', { code: 0 }), cwd: fixture,
        childEnv, controllerRuntime: runtime });
      assert.equal(invalid.execution.kind, 'setup-failure'); assert.equal(invalid.events.length, 0);
      assert.equal(invalid.execution.effectiveExitCode, 1);
    }
    assert.throws(() => owner.decodeControlJson(Buffer.from('{"x":1,"x":2}')), /JSON/);
    assert.throws(() => owner.decodeControlJson(Buffer.from([255])), /UTF-8/);
    assert.throws(() => owner.decodeControlJson(Buffer.concat([Buffer.from([239, 187, 191]), Buffer.from('{}')])), /JSON|UTF-8|BOM/);
    for (const filename of ['node.exe', 'pwsh.exe', 'powershell.exe']) {
      const childEnv = { ...process.env };
      const pathKey = Object.keys(childEnv).find(key => key.toUpperCase() === 'PATH');
      const entries = (childEnv[pathKey] || '').split(delimiter).filter(entry => !existsSync(join(entry, filename)));
      for (const key of Object.keys(childEnv)) if (key.toUpperCase() === 'PATH') delete childEnv[key];
      childEnv.PATH = entries.join(delimiter);
      await run('trusted controller preserves missing child ' + filename, {
        argv: actorArgs('argv-env', { argv: [], env: { PATH: childEnv.PATH }, missing: [filename] }), childEnv });
    }
    const identity = owner.createRunIdentity();
    const surviving = await run('normal root exit0 with a live descendant remains a rejection', {
      argv: actorArgs('tree', { rootExit: true }) }, true);
    assert.equal(surviving.execution.kind, 'surviving-descendant'); assert.equal(surviving.execution.rawWorkerExitDWORD, 0);
    assert.equal(surviving.execution.effectiveExitCode, 1); assert.equal(surviving.cleanup.confirmed, true);
    const forced = await run('verified descendant, exited launcher, two grandchildren, fixed2000ms', {
      argv: actorArgs('tree', { count: 2, rootExit: true, identity }),
      mode: 'forced-2000ms', readiness: { ...identity, sourceScriptSha256: hash(await readFile(self)) } }, true);
    assert.equal(forced.execution.kind, 'forced-timeout');
    assert.equal(forced.execution.rawWorkerExitDWORD, 0);
    assert.equal(forced.cleanup.confirmed, true); assert.equal(forced.cleanup.peerWaitSignaled, true);
    assert.ok(forced.nodeTiming.stopRequestedNodeMs - forced.nodeTiming.forcedArmedNodeMs >= 2000);
    assert.equal(forced.events.filter(event => event.type === 'READY').length, 1);
    const realReady = forced.events.find(event => event.type === 'READY');
    const realRoot = forced.events.find(event => event.type === 'OWNED');
    assert.notEqual(realReady.peerPid, realRoot.rootPid);
    owner.validateReadyPeer(realReady, { rootPid: realRoot.rootPid, sourceScriptSha256: realReady.sourceScriptSha256 });
    const rootAsPeer = structuredClone(realReady); rootAsPeer.peerPid = realRoot.rootPid;
    assert.throws(() => owner.validateReadyPeer(rootAsPeer, { rootPid: realRoot.rootPid, sourceScriptSha256: realReady.sourceScriptSha256 }), /descendant readiness/);
    for (const [field, value] of [['peerPid', 0x100000000], ['peerCreationTime', 'malformed'], ['peerInOwnedJob', false], ['peerWaitResult', 0], ['sourceScriptSha256', '0'.repeat(64)]]) {
      const invalid = structuredClone(realReady); invalid[field] = value;
      assert.throws(() => owner.validateReadyPeer(invalid, { rootPid: realRoot.rootPid, sourceScriptSha256: realReady.sourceScriptSha256 }), /descendant readiness/);
    }
    const earlyIdentity = owner.createRunIdentity();
    const early = await run('verified descendant exits before fixed interval and cannot prove timeout', {
      argv: actorArgs('tree', { identity: earlyIdentity, lifetimeMs: 100 }), mode: 'forced-2000ms',
      readiness: { ...earlyIdentity, sourceScriptSha256: hash(await readFile(self)) } }, true);
    assert.equal(early.execution.kind, 'setup-failure'); assert.equal(early.cleanup.confirmed, true);
    assert.equal(early.events.filter(event => event.type === 'READY').length, 1);
    const bashIdentity = owner.createRunIdentity();
    const bashConfig = join(fixture, 'bash-leaf.json');
    const bashScript = join(fixture, 'bash-launcher.sh');
    await writeFile(bashConfig, JSON.stringify({ identity: bashIdentity }));
    const shellQuote = value => "'" + value.replaceAll("'", "'\\''") + "'";
    await writeFile(bashScript, '#!/usr/bin/env bash\n' + shellQuote(process.execPath.replaceAll('\\', '/')) + ' ' + shellQuote(self.replaceAll('\\', '/')) +
      " --actor leaf \"$(cat " + shellQuote(bashConfig.replaceAll('\\', '/')) + ')\" &\nexit 0\n');
    const bashOwned = await run('actual Git Bash root exit0 with inherited-stream descendant', {
      application: 'C:/Program Files/Git/bin/bash.exe', argv: [bashScript], mode: 'forced-2000ms',
      readiness: { ...bashIdentity, sourceScriptSha256: hash(await readFile(self)), sourceScriptPath: self },
      sourceBinding: await owner.makeSourceBinding({ 'bash-launcher.sh': hash(await readFile(bashScript)), 'bash-leaf.json': hash(await readFile(bashConfig)) }) }, true);
    assert.equal(bashOwned.execution.rawWorkerExitDWORD, 0); assert.equal(bashOwned.execution.kind, 'forced-timeout');
    assert.equal(bashOwned.cleanup.confirmed, true); assert.equal(bashOwned.cleanup.peerWaitSignaled, true);
    for (const [label, readyDelay] of [['late-readiness', 10000], ['no-readiness', null]]) {
      const id = owner.createRunIdentity();
      const result = await run(label + ' is setup failure, not intended timeout', {
        argv: actorArgs('tree', { identity: readyDelay == null ? null : id, readyDelay }), mode: 'forced-2000ms',
        readiness: { ...id, sourceScriptSha256: hash(await readFile(self)) },
        budgetsMs: { startupMs: 7000 } }, true);
      assert.equal(result.execution.kind, 'setup-failure'); assert.equal(result.cleanup.confirmed, true);
      assert.equal(result.nodeTiming.forcedArmedNodeMs, undefined); assert.ok(result.events.some(event => event.type === 'OWNED'));
    }
    for (const fault of ['assign-null-handle', 'resume-null-handle', 'query-invalid-class', 'close-null-handle']) {
      const marker = join(fixture, fault + '.marker');
      const outer = await run('actual native ' + fault + ' with separate outer containment', {
        argv: actorArgs('supervised', { cwd: fixture, marker, testFault: fault,
          innerActor: fault === 'query-invalid-class' ? 'exit' : 'marker',
          inner: fault === 'query-invalid-class' ? { code: 7 } : { marker } }) });
      const inner = JSON.parse(outer.output.stdout.toString().split('INNER_RESULT ')[1].trim());
      records.at(-1).innerNativeEvidence = inner;
      assert.equal(inner.execution.effectiveExitCode, 1);
      if (fault === 'query-invalid-class') assert.equal(inner.execution.rawWorkerExitDWORD, 7);
      if (fault.startsWith('assign') || fault.startsWith('resume')) assert.equal(existsSync(marker), false);
      else assert.equal(inner.cleanup.confirmed, false);
    }
    for (const protocol of ['wrong-nonce', 'wrong-generation', 'replayed-sequence', 'malformed-length', 'invalid-utf8', 'duplicate-key', 'eof']) {
      const outer = await run('private control ' + protocol + ' fails closed under outer native ownership', {
        argv: actorArgs('supervised', { cwd: fixture, protocol, innerActor: 'tree', inner: {} }) });
      const inner = JSON.parse(outer.output.stdout.toString().split('INNER_RESULT ')[1].trim());
      assert.equal(inner.execution.kind, 'protocol-failure'); assert.equal(inner.cleanup.confirmed, false);
    }
    await run('wrong identity cannot stop another simultaneous native owner', { argv: actorArgs('two-owners', { cwd: fixture }) });
    await run('outside-job readiness peer is rejected without becoming a cleanup target', { argv: actorArgs('outside-readiness', { cwd: fixture }) });
    const cleanupDeadline = await run('native cleanup deadline stays unconfirmed within independent outer containment', {
      argv: actorArgs('supervised', { cwd: fixture, innerActor: 'tree', inner: {}, cleanupDeadline: true, budgetsMs: { cleanupMs: 1 } }) });
    const deadlineInner = JSON.parse(cleanupDeadline.output.stdout.toString().split('INNER_RESULT ')[1].trim());
    assert.equal(deadlineInner.cleanup.deadlineExpired, true); assert.equal(deadlineInner.cleanup.confirmed, false);
    const copiedDirectory = join(fixture, 'copied-owner'); await mkdir(copiedDirectory);
    for (const file of ['owned-regression-process.mjs', 'owned-regression-process.ps1', 'owned-regression-process-tests.mjs']) await cp(join(dirname(self), file), join(copiedDirectory, file));
    const copiedOwner = await import(pathToFileURL(join(copiedDirectory, 'owned-regression-process.mjs')));
    const copiedResult = await ledger.invoke('actual copied owner modules resolve and execute only sibling bytes', () => copiedOwner.runOwned({ application: process.execPath,
      argv: [join(copiedDirectory, 'owned-regression-process-tests.mjs'), '--actor', 'exit', '{"code":0}'],
      cwd: fixture, childEnv: process.env, controllerRuntime: runtime }));
    records.push({ label: 'actual copied owner modules resolve and execute only sibling bytes', ...summarize(copiedResult) });
    assert.equal(copiedResult.execution.effectiveExitCode, 0); assert.equal(copiedResult.cleanup.confirmed, true);
    assert.equal(copiedResult.sourceBinding.bundleRoot, copiedDirectory);
    for (const [label, correctionCase] of [
      ['root readiness cannot substitute for a native descendant', 'root'],
      ['genuine READY delayed past absolute Node startup cutoff is rejected', 'node-late'],
      ['genuine peer delayed past native startup acceptance is rejected', 'native-late'],
      ['genuine in-flight READY received after STOPPING is rejected', 'node-stop']
    ]) {
      const outer = await run(label, { argv: actorArgs('correction-readiness', { case: correctionCase, cwd: fixture }) });
      const inner = JSON.parse(outer.output.stdout.toString().split('CORRECTION_INNER ')[1].trim());
      records.at(-1).correctionInnerEvidence = inner;
      assert.equal(inner.case, correctionCase);
      assert.equal(inner.result.cleanup.confirmed, true);
      assert.equal(inner.result.events.filter(event => event.type === 'READY').length, 0);
      assert.equal(outer.execution.rawWorkerExitDWORD, 0);
    }
    for (const [label, startupCase] of [
      ['timely normal OWNED cancels all early-rearmed startup timers', 'normal-early'],
      ['timely descendant READY cancels all early-rearmed startup timers', 'forced-early'],
      ['late normal OWNED is rejected before overdue startup callback', 'owned-late'],
      ['late normal OWNED startup failure stays failed beside genuine native exit0 terminal', 'zero-terminal'],
      ['timely normal OWNED after STOPPING cannot complete startup', 'owned-after-stop']
    ]) {
      const outer = await run(label, { argv: actorArgs('correction-logical-startup', { case: startupCase, cwd: fixture }) });
      const observed = JSON.parse(outer.output.stdout.toString().split('LOGICAL_STARTUP_INNER ')[1].trim());
      records.at(-1).logicalStartupEvidence = observed;
      assert.equal(observed.case, startupCase);
      assert.equal(outer.execution.rawWorkerExitDWORD, 0);
      assert.equal(outer.cleanup.confirmed, true);
      if (startupCase === 'zero-terminal') {
        assert.equal(observed.nativeTerminal.execution.kind, 'worker-exit');
        assert.equal(observed.nativeTerminal.execution.rawWorkerExitDWORD, 0);
        assert.equal(observed.nativeTerminal.execution.nativeRootWait.signaled, true);
        assert.equal(observed.nativeTerminal.execution.nativeRootWait.result, 0);
        assert.equal(observed.inner.execution.kind, 'setup-failure');
        assert.equal(observed.inner.execution.effectiveExitCode, 1);
      }
    }
    const preservedOuter = await run('actual copied-owner unconfirmed fixture is preserved until outer reconciliation', {
      argv: actorArgs('correction-copied-preservation', {}) });
    const preserved = JSON.parse(preservedOuter.output.stdout.toString().split('COPIED_PRESERVATION ')[1].trim());
    records.at(-1).copiedPreservationProof = preserved;
    const preservedLeaf = resolve(preserved.fixture);
    const preservedOffset = relative(resolve(tmpdir()), preservedLeaf);
    assert.ok(preservedOffset.startsWith('nstu-owned-preserve-') && !preservedOffset.includes(sep) && !preservedOffset.includes('/') && !preservedOffset.includes('\\'));
    assert.equal(preservedOuter.execution.rawWorkerExitDWORD, 0);
    assert.equal(preservedOuter.cleanup.confirmed, true);
    assert.equal(preservedOuter.cleanup.activeProcessesLast, 0);
    assert.equal(preserved.deletionAttempted, false);
    assert.equal(preserved.ledger.length, 1);
    assert.equal(preserved.ledger[0].state, 'unconfirmed');
    assert.equal(existsSync(preservedLeaf), true);
    for (const [filename, expected] of Object.entries(preserved.sourceHashes)) {
      assert.equal(hash(await readFile(join(preservedLeaf, 'copied-owner', filename))), expected);
      assert.equal(expected, hash(await readFile(join(dirname(self), filename))));
    }
    assert.equal(hash(await readFile(join(preservedLeaf, 'preserved.marker'))), preserved.markerSha256);
    const leafReceipt = JSON.parse(await readFile(join(preservedLeaf, 'preservation-receipt.json')));
    assert.equal(leafReceipt.ledger[0].state, 'unconfirmed');
    await writeFile(join(fixture, 'copied-preservation-proof.json'), JSON.stringify({ preserved, outer: summarize(preservedOuter), independentlyReconciled: true }, null, 2) + '\n');
    await rm(preservedLeaf, { recursive: true, force: true });
    const boundInput = join(fixture, 'bound-input.txt'); await writeFile(boundInput, 'pinned');
    const bound = await owner.makeSourceBinding({ 'bound-input.txt': hash(await readFile(boundInput)) });
    await writeFile(boundInput, 'changed');
    const changedInput = await run('changed scenario source is rejected before worker creation', { sourceBinding: bound }, true);
    assert.equal(changedInput.execution.kind, 'setup-failure'); assert.equal(changedInput.events.some(event => event.type === 'OWNED'), false);
    const bootstrapDirectory = join(fixture, 'bootstrap-fault'); await mkdir(bootstrapDirectory);
    for (const file of ['owned-regression-process.mjs', 'owned-regression-process.ps1', 'owned-regression-process-tests.mjs']) await cp(join(dirname(self), file), join(bootstrapDirectory, file));
    await writeFile(join(bootstrapDirectory, 'owned-regression-process.ps1'), "throw 'G1 named bootstrap failure'\n" + await readFile(join(bootstrapDirectory, 'owned-regression-process.ps1'), 'utf8'));
    const bootstrap = await run('compiler/controller bootstrap failure remains unconfirmed within separate outer containment', {
      argv: actorArgs('supervised', { cwd: fixture, ownerFile: join(bootstrapDirectory, 'owned-regression-process.mjs'), bootstrapFailure: true }) });
    const bootstrapInner = JSON.parse(bootstrap.output.stdout.toString().split('INNER_RESULT ')[1].trim());
    assert.equal(bootstrapInner.cleanup.confirmed, false); assert.equal(bootstrapInner.events.length, 0);
    const overflow = await run('bounded stdout overflow is failure with native cleanup', { argv: actorArgs('overflow', {}) }, true);
    assert.equal(overflow.execution.kind, 'output-overflow'); assert.equal(overflow.output.overflow, true);
    assert.equal(overflow.cleanup.confirmed, true);
    for (const [label, scenario] of [
      ['job snapshot descendant exits before retained open; fresh empty proof', 'liveness-snapshot-exit'],
      ['job snapshot live descendant cannot become empty through refresh', 'liveness-live'],
      ['owned signaled descendant and exit259 remain exited', 'liveness-signaled'],
      ['changed snapshot candidate never waits a foreign owner', 'liveness-foreign'],
      ['shared liveness decision rejects invalid and inconsistent observations', 'liveness-trace'],
      ['access-denied descendant remains failure under outer containment', 'liveness-denied']
    ]) {
      await run(label, { argv: actorArgs('liveness-scenario', { scenario }) }, false, (result, record) => {
        record.livenessOutput = { stdoutBase64: result.output.stdout.toString('base64'), stderrBase64: result.output.stderr.toString('base64') };
        record.livenessEvidence = JSON.parse(result.output.stdout.toString().split('LIVENESS_EVIDENCE ')[1].trim());
      });
    }
    assert.equal(records.length, 55);
    console.log('OWNER_SOURCE_BINDING ' + JSON.stringify(await owner.makeSourceBinding()));
    for (const record of records) console.log(JSON.stringify(record));
    console.log('OWNED PROCESS TESTS: PASS (' + records.length + ' cases)');
  } catch (error) {
    primaryFailure = error;
    for (const record of records) console.error(JSON.stringify(record));
    throw error;
  } finally {
    try {
      const leaf = relative(resolve(tmpdir()), resolve(fixture));
      assert.ok(leaf.startsWith('nstu-owned-') && !leaf.includes(sep));
      await finalizeOwnedFixture(fixture, ledger, primaryFailure);
    }
    catch (error) {
      if (primaryFailure) throw new AggregateError([primaryFailure, error], 'Primary test failure and fixture finalization failure');
      throw error;
    }
  }
}

if (process.argv[2] === '--actor') await actor(process.argv[3], process.argv[4]);
else if (process.argv[2] === '--baseline-red') await baselineRed();
else main().catch(error => { console.error(error.stack); process.exitCode = 1; });
