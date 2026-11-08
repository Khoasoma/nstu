import { spawn, spawnSync } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { readFile, realpath, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { delimiter, dirname, isAbsolute, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { performance } from 'node:perf_hooks';

const bundleRoot = dirname(fileURLToPath(import.meta.url));
const script = join(bundleRoot, 'owned-regression-process.ps1');
const ownerFiles = ['owned-regression-process.mjs', 'owned-regression-process.ps1', 'owned-regression-process-tests.mjs'];
const maxOutput = 8 * 1024 * 1024;
const commonFields = ['v', 'type', 'runId', 'nonce', 'generation', 'sourceBindingSha256', 'seq'];
const payloadFields = {
  OWNED: ['rootPid', 'rootCreationTime', 'jobLimitFlags', 'handleListCount', 'excludedHandlesNoninheritable', 'rootInOwnedJob', 'controllerInJob', 'abi', 'controllerElapsedMs'],
  DATA: ['stream', 'bytesBase64'],
  READY: ['peerPid', 'peerCreationTime', 'peerInOwnedJob', 'peerWaitResult', 'sourceScriptSha256', 'controllerElapsedMs'],
  ROOT_EXIT: ['rawWorkerExitDWORD', 'nativeRootWait', 'controllerElapsedMs'],
  TERMINAL: ['execution', 'cleanup', 'controllerTiming', 'output', 'bootstrap']
};
const stopReasons = new Set(['forced-2000ms', 'execution-deadline', 'overall-deadline', 'startup-deadline', 'output-overflow', 'parent-abort']);
const livenessFaultNames = new Set(['liveness-snapshot-exit', 'liveness-live', 'liveness-signaled', 'liveness-foreign', 'liveness-trace', 'liveness-denied']);
const faultNames = new Set(['assign-null-handle', 'resume-null-handle', 'query-invalid-class', 'close-null-handle', 'ready-accept-after-startup', ...livenessFaultNames]);
const readyDeliveryControls = new Set(['stall-past-node-startup', 'stop-before-node-ready-acceptance']);
const startupControls = new Set(['early-startup-wake', 'stall-normal-owned-past-startup', 'stop-before-node-owned-acceptance']);
const digest = bytes => createHash('sha256').update(bytes).digest('hex').toUpperCase();
function safeError(kind, message, error) {
  return { kind, message, ...(error?.stack ? { stack: error.stack } : {}), ...(error?.code ? { code: error.code } : {}) };
}
function text(value, field) {
  if (typeof value !== 'string' || value.includes('\0')) throw new Error('Invalid string field: ' + field);
  for (let index = 0; index < value.length; ++index) {
    const code = value.charCodeAt(index);
    if (code >= 0xD800 && code <= 0xDBFF) {
      const next = value.charCodeAt(++index);
      if (!(next >= 0xDC00 && next <= 0xDFFF)) throw new Error('Invalid UTF-16 field: ' + field);
    } else if (code >= 0xDC00 && code <= 0xDFFF) throw new Error('Invalid UTF-16 field: ' + field);
  }
  return value;
}
export function quoteWindowsArg(value) {
  text(value, 'argv');
  let output = '"', slashes = 0;
  for (const char of value) {
    if (char === '\\') { ++slashes; continue; }
    if (char === '"') output += '\\'.repeat(2 * slashes + 1) + '"';
    else output += '\\'.repeat(slashes) + char;
    slashes = 0;
  }
  return output + '\\'.repeat(2 * slashes) + '"';
}
export function validateEnvironment(env) {
  const entries = Array.isArray(env) ? env : Object.entries(env).map(([key, value]) => ({ key, value }));
  const seen = new Set();
  for (const entry of entries) {
    if (!entry || Object.keys(entry).sort().join(',') !== 'key,value') throw new Error('Invalid environment entry');
    const key = text(entry.key, 'env key'), value = text(entry.value, 'env value');
    const folded = key.toUpperCase();
    if (!key || seen.has(folded)) throw new Error('Empty or duplicate environment key');
    seen.add(folded);
    if (key.startsWith('=')) {
      if (!/^=[A-Za-z]:$/.test(key) || value.slice(0, 2).toUpperCase() !== key.slice(1).toUpperCase() || !/^[A-Za-z]:[\\/]/.test(value)) throw new Error('Invalid reserved drive environment key');
    } else if (key.includes('=')) throw new Error('Invalid environment key');
  }
  const sorted = entries.map(entry => ({ ...entry })).sort((a, b) => a.key.toUpperCase() < b.key.toUpperCase() ? -1 : a.key.toUpperCase() > b.key.toUpperCase() ? 1 : 0);
  const block = sorted.map(entry => entry.key + '=' + entry.value + '\0').join('') + '\0' + (sorted.length ? '' : '\0');
  if (Buffer.byteLength(block, 'utf16le') > 1024 * 1024) throw new Error('Environment block exceeds bound');
  return sorted;
}
function scanJson(input) {
  let index = 0;
  const fail = () => { throw new Error('Invalid control JSON at offset ' + index); };
  const white = () => { while (/\s/.test(input[index] || '\0')) ++index; };
  function string() {
    const start = index++;
    for (; index < input.length; ++index) {
      if (input[index] === '\\') { ++index; continue; }
      if (input[index] === '"') {
        ++index;
        try { return JSON.parse(input.slice(start, index)); } catch { fail(); }
      }
      if (input.charCodeAt(index) < 32) fail();
    }
    fail();
  }
  function value(depth) {
    if (depth > 32) fail(); white();
    const token = input[index];
    if (token === '{') {
      ++index; white(); const keys = new Set();
      if (input[index] === '}') { ++index; return; }
      for (;;) {
        if (input[index] !== '"') fail(); const key = string();
        if (keys.has(key)) fail(); keys.add(key); white();
        if (input[index++] !== ':') fail(); value(depth + 1); white();
        if (input[index] === '}') { ++index; return; }
        if (input[index++] !== ',') fail(); white();
      }
    }
    if (token === '[') {
      ++index; white(); if (input[index] === ']') { ++index; return; }
      for (;;) { value(depth + 1); white(); if (input[index] === ']') { ++index; return; } if (input[index++] !== ',') fail(); }
    }
    if (token === '"') { string(); return; }
    const match = /^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/.exec(input.slice(index));
    if (!match) fail(); index += match[0].length;
  }
  value(0); white(); if (index !== input.length) fail();
}
export function decodeControlJson(bytes) {
  if (bytes.length >= 3 && bytes[0] === 239 && bytes[1] === 187 && bytes[2] === 191) throw new Error('Control UTF-8 BOM is not permitted');
  let input;
  try { input = new TextDecoder('utf-8', { fatal: true }).decode(bytes); } catch { throw new Error('Invalid control UTF-8'); }
  scanJson(input);
  try { return JSON.parse(input); } catch { throw new Error('Invalid control JSON'); }
}
export function encodeControlFrame(value) {
  const bytes = Buffer.from(JSON.stringify(value));
  if (!bytes.length || bytes.length > 512 * 1024) throw new Error('Control frame exceeds bound');
  const frame = Buffer.alloc(4 + bytes.length); frame.writeUInt32LE(bytes.length); bytes.copy(frame, 4); return frame;
}
export function validateControllerTerminal(frame) {
  const keys = (value, required, optional = []) => {
    if (!value || typeof value !== 'object' || Array.isArray(value) || required.some(key => !Object.hasOwn(value, key)) || Object.keys(value).some(key => !required.includes(key) && !optional.includes(key))) throw new Error('Invalid native terminal object fields');
  };
  const bool = value => { if (typeof value !== 'boolean') throw new Error('Native terminal boolean required'); };
  const uint = (value, nullable = false) => { if (nullable && value === null) return; if (!Number.isSafeInteger(value) || value < 0 || value > 0xFFFFFFFF) throw new Error('Invalid native terminal integer'); };
  const elapsed = (value, nullable = false) => { if (nullable && value === null) return; if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) throw new Error('Invalid terminal clock value'); };
  const error = value => {
    keys(value, ['kind', 'message'], ['win32Error', 'stack', 'cause', 'operation', 'code']);
    text(value.kind, 'terminal error kind'); text(value.message, 'terminal error message');
    if (Object.hasOwn(value, 'win32Error')) uint(value.win32Error);
    if (Object.hasOwn(value, 'stack')) text(value.stack, 'terminal error stack');
  };
  keys(frame, ['execution', 'cleanup', 'controllerTiming', 'output', 'bootstrap']);
  const execution = frame.execution, cleanup = frame.cleanup;
  keys(execution, ['kind', 'rawWorkerExitDWORD', 'effectiveExitCode', 'nativeRootWait', 'primaryErrors']);
  if (!['worker-exit', 'forced-timeout', 'execution-timeout', 'setup-failure', 'worker-spawn-failure', 'protocol-failure', 'output-overflow', 'controller-failure', 'surviving-descendant'].includes(execution.kind)) throw new Error('Invalid native terminal execution kind');
  uint(execution.rawWorkerExitDWORD, true);
  if (![0, 1].includes(execution.effectiveExitCode)) throw new Error('Invalid portable native exit');
  keys(execution.nativeRootWait, ['result', 'signaled', 'win32Error']);
  uint(execution.nativeRootWait.result, true); uint(execution.nativeRootWait.win32Error, true); bool(execution.nativeRootWait.signaled);
  if (execution.rawWorkerExitDWORD !== null && (!execution.nativeRootWait.signaled || execution.nativeRootWait.result !== 0)) throw new Error('Raw native exit without signaled native wait');
  if (!Array.isArray(execution.primaryErrors)) throw new Error('Native primary errors array required');
  execution.primaryErrors.forEach(error);
  keys(cleanup, ['scope', 'confirmed', 'activeProcessesLast', 'rootWaitSignaled', 'peerWaitSignaled', 'handlesClosedConfirmed', 'operations', 'errors', 'deadlineExpired']);
  if (!['job-tree', 'suspended-root', 'no-child-created'].includes(cleanup.scope)) throw new Error('Invalid native cleanup scope');
  for (const field of ['confirmed', 'rootWaitSignaled', 'handlesClosedConfirmed', 'deadlineExpired']) bool(cleanup[field]);
  if (cleanup.peerWaitSignaled !== null) bool(cleanup.peerWaitSignaled);
  uint(cleanup.activeProcessesLast, true);
  if (!Array.isArray(cleanup.operations) || cleanup.operations.length > 4096 || !Array.isArray(cleanup.errors)) throw new Error('Native cleanup operation bound exceeded');
  cleanup.errors.forEach(error);
  for (const operation of cleanup.operations) {
    keys(operation, ['operation', 'resourceRole', 'ok', 'returnValue', 'win32Error', 'controllerElapsedMs'], ['observations', 'controllerLastElapsedMs', 'nativeReturn', 'observedError']);
    text(operation.operation, 'native operation'); text(operation.resourceRole, 'native role'); bool(operation.ok);
    uint(operation.win32Error, true); elapsed(operation.controllerElapsedMs);
    if (Object.hasOwn(operation, 'observations')) uint(operation.observations);
    if (Object.hasOwn(operation, 'controllerLastElapsedMs')) elapsed(operation.controllerLastElapsedMs);
  }
  keys(frame.output, ['stdoutBytes', 'stderrBytes', 'overflow', 'workerStdoutEOF', 'workerStderrEOF']);
  for (const field of ['stdoutBytes', 'stderrBytes']) uint(frame.output[field]);
  for (const field of ['overflow', 'workerStdoutEOF', 'workerStderrEOF']) bool(frame.output[field]);
  keys(frame.bootstrap, ['outsideWorkerJob', 'completed', 'wholeControllerTreeCleanupConfirmed']);
  for (const value of Object.values(frame.bootstrap)) bool(value);
  if (!frame.bootstrap.outsideWorkerJob || frame.bootstrap.wholeControllerTreeCleanupConfirmed) throw new Error('Unsupported whole-controller bootstrap cleanup claim');
  keys(frame.controllerTiming, ['originUtc', 'ownedElapsedMs', 'readyElapsedMs', 'stopElapsedMs', 'cleanupFinishedElapsedMs', 'terminalElapsedMs']);
  text(frame.controllerTiming.originUtc, 'controller UTC label');
  for (const [field, value] of Object.entries(frame.controllerTiming)) if (field !== 'originUtc') elapsed(value, true);
  if (Buffer.byteLength(JSON.stringify([execution.primaryErrors, cleanup.errors])) > 256 * 1024) throw new Error('Terminal diagnostic bound exceeded');
  if (cleanup.confirmed) {
    if (!cleanup.handlesClosedConfirmed || cleanup.deadlineExpired || cleanup.errors.length) throw new Error('Inconsistent native cleanup confirmation');
    if (cleanup.scope !== 'no-child-created' && (cleanup.activeProcessesLast !== 0 || !cleanup.rootWaitSignaled || !frame.output.workerStdoutEOF || !frame.output.workerStderrEOF)) throw new Error('Incomplete native cleanup wait/EOF proof');
    if (cleanup.peerWaitSignaled === false) throw new Error('Incomplete native peer cleanup');
    if (cleanup.scope === 'job-tree' && !cleanup.operations.some(operation => operation.operation === 'TerminateJobObject' && operation.ok)) throw new Error('Native job termination proof missing');
  }
  const expected = execution.kind === 'worker-exit' && execution.rawWorkerExitDWORD === 0 && execution.nativeRootWait.signaled && !execution.primaryErrors.length && cleanup.confirmed && !frame.output.overflow ? 0 : 1;
  if (execution.effectiveExitCode !== expected) throw new Error('Native terminal exit consistency failed');
}
export function createRunIdentity() {
  const runId = randomBytes(16).toString('hex'), nonce = randomBytes(32).toString('hex'), generation = randomBytes(8).toString('hex');
  return { runId, nonce, generation, pipeName: '\\\\.\\pipe\\nstu-owned-' + runId + '-' + generation };
}
export function validateReadyPeer(frame, { rootPid, sourceScriptSha256 }) {
  const uintPid = value => Number.isSafeInteger(value) && value > 0 && value <= 0xFFFFFFFF;
  if (!uintPid(rootPid) || !uintPid(frame.peerPid) || frame.peerPid === rootPid ||
      frame.peerInOwnedJob !== true || frame.peerWaitResult !== 258 ||
      typeof frame.peerCreationTime !== 'string' || !/^[A-F0-9]{16}$/.test(frame.peerCreationTime) ||
      frame.sourceScriptSha256 !== sourceScriptSha256) {
    throw new Error('Invalid native descendant readiness evidence');
  }
}
export async function makeSourceBinding(scenarioInputs = {}) {
  const runtimeBundle = {};
  for (const file of ownerFiles) runtimeBundle[file] = digest(await readFile(join(bundleRoot, file)));
  const records = [];
  for (const [role, values] of Object.entries({ runtimeBundle, scenarioInputs })) {
    for (const [path, hash] of Object.entries(values)) {
      text(path, 'source path');
      if (!/^(?:[A-Fa-f0-9]{64}|ABSENT)$/.test(hash) || path.includes('\\') || path.startsWith('/') || path.split('/').includes('..')) throw new Error('Invalid source binding');
      records.push([role, path, hash]);
    }
  }
  records.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0);
  return { bundleRoot, runtimeBundle, scenarioInputs, sha256: digest(Buffer.from(records.map(record => record.join('\0') + '\0').join(''))) };
}
export async function resolveControllerRuntime(parentEnv = process.env) {
  if (process.platform !== 'win32' || process.arch !== 'x64') throw new Error('Windows x64 owner runtime required');
  const systemRoot = Object.entries(parentEnv).find(([key]) => key.toUpperCase() === 'SYSTEMROOT')?.[1];
  if (!systemRoot || !isAbsolute(systemRoot)) throw new Error('Trusted absolute parent SystemRoot missing');
  const exe = await realpath(join(systemRoot, 'System32/WindowsPowerShell/v1.0/powershell.exe'));
  const bytes = await readFile(exe);
  const pe = bytes.readUInt32LE(0x3C);
  if (bytes.toString('ascii', pe, pe + 4) !== 'PE\0\0' || bytes.readUInt16LE(pe + 4) !== 0x8664) throw new Error('Controller executable is not x64 PE');
  const env = { ...parentEnv };
  for (const key of Object.keys(env)) if (key.toUpperCase() === 'PSMODULEPATH') delete env[key];
  const version = spawnSync(exe, ['-NoProfile', '-NonInteractive', '-Command', '$PSVersionTable.PSVersion.ToString()'],
    { env, encoding: 'utf8', windowsHide: true, timeout: 10000, maxBuffer: 65536 });
  if (version.error || version.signal || version.status !== 0 || !/^5\.1\./.test((version.stdout || '').trim())) throw new Error('Trusted Windows PowerShell5.1 runtime probe failed');
  const runtime = { exe, version: version.stdout.trim(), architecture: 'x64', sha256: digest(bytes) };
  Object.defineProperty(runtime, 'environment', { value: env });
  return runtime;
}
async function applicationPath(application, entries) {
  text(application, 'application');
  if (isAbsolute(application)) { const path = await realpath(application); if (!(await stat(path)).isFile()) throw new Error('Worker executable is not a file'); return path; }
  if (/[\\/]/.test(application)) throw new Error('Worker path must be absolute or a bare command');
  const env = Object.fromEntries(entries.map(entry => [entry.key.toUpperCase(), entry.value]));
  const suffixes = /\.[^\\/]+$/.test(application) ? [''] : (env.PATHEXT || '.COM;.EXE').split(';');
  for (const directory of (env.PATH || '').split(delimiter)) {
    if (!directory || !isAbsolute(directory)) continue;
    for (const suffix of suffixes) { const candidate = join(directory, application + suffix); if (existsSync(candidate) && (await stat(candidate)).isFile()) return realpath(candidate); }
  }
  throw new Error('Requested worker executable missing from supplied child PATH');
}
function budgets(input = {}) {
  const result = { overallMs: 600000, startupMs: 30000, cleanupMs: 10000, controllerExitMs: 2000, ...input };
  for (const [key, max] of Object.entries({ overallMs: 600000, startupMs: 30000, cleanupMs: 10000, controllerExitMs: 2000 })) {
    if (!Number.isSafeInteger(result[key]) || result[key] < 1 || result[key] > max) throw new Error('Invalid deadline budget: ' + key);
  }
  if (Object.keys(result).length !== 4 || result.overallMs <= result.cleanupMs + result.controllerExitMs + 2000) throw new Error('Insufficient overall owner budget');
  return result;
}

/** Run one worker under checked native job ownership. Importing this module starts nothing.
 * onEvent is a trusted local observation hook; its controls are never inherited by the worker.
 * Cleanup covers the worker job. Add-Type bootstrap is outside that later-created job.
 */
export async function runOwned(options) {
  const timing = { startUtc: new Date().toISOString(), startNodeMs: performance.now() };
  let identity = createRunIdentity(), binding, request, runtime, deadline;
  const empty = { schemaVersion: 1, ...identity, nodeTiming: timing, events: [],
    sourceBinding: null, controller: { exitCode: null, signal: null, bootstrapErrors: [], bootstrapStderrBytes: 0, controlStdoutEOF: false, controlStderrEOF: false },
    output: { stdout: Buffer.alloc(0), stderr: Buffer.alloc(0), stdoutBytes: 0, stderrBytes: 0, overflow: false, workerStdoutEOF: false, workerStderrEOF: false },
    cleanup: { scope: 'no-child-created', confirmed: false, activeProcessesLast: null, rootWaitSignaled: false, peerWaitSignaled: null, handlesClosedConfirmed: false, operations: [], errors: [], deadlineExpired: false },
    execution: { kind: 'setup-failure', rawWorkerExitDWORD: null, effectiveExitCode: 1, nativeRootWait: { result: null, signaled: false, win32Error: null }, primaryErrors: [] } };
  try {
    if (process.platform !== 'win32' || process.arch !== 'x64') throw new Error('Windows x64 native ownership required');
    const mode = options.mode || 'normal'; if (!['normal', 'forced-2000ms'].includes(mode)) throw new Error('Invalid owner mode');
    deadline = budgets(options.budgetsMs);
    const entries = validateEnvironment(options.childEnv || process.env);
    const argv = options.argv.map(value => text(value, 'argv'));
    if (options.testFault != null && !faultNames.has(options.testFault)) throw new Error('Invalid named native fault');
    runtime = options.controllerRuntime || await resolveControllerRuntime(process.env);
    if (!isAbsolute(runtime.exe) || runtime.architecture !== 'x64' || digest(await readFile(runtime.exe)) !== runtime.sha256) throw new Error('Controller runtime binding changed');
    binding = options.sourceBinding || await makeSourceBinding();
    if (options.testReadyDelivery != null && !readyDeliveryControls.has(options.testReadyDelivery)) throw new Error('Invalid named READY delivery control');
    if (options.testStartupControl != null && !startupControls.has(options.testStartupControl)) throw new Error('Invalid named startup control');
    if (options.testFault != null || options.testReadyDelivery != null || options.testStartupControl != null) {
      const siblingTest = join(bundleRoot, 'owned-regression-process-tests.mjs');
      if (argv.length < 3 || resolve(argv[0]) !== resolve(siblingTest) || argv[1] !== '--actor' ||
          binding.bundleRoot !== bundleRoot || binding.runtimeBundle?.['owned-regression-process-tests.mjs'] !== digest(await readFile(siblingTest))) {
        throw new Error('Named test control source boundary mismatch');
      }
    }
    if (options.testFault === 'ready-accept-after-startup' && deadline.startupMs !== 2000) throw new Error('Native late readiness control requires startup2000ms');
    if (options.testReadyDelivery != null && deadline.startupMs !== 2000) throw new Error('READY delivery control requires startup2000ms');
    if (options.testStartupControl != null && deadline.startupMs !== 2000) throw new Error('Startup control requires startup2000ms');
    if (options.testStartupControl != null && options.testStartupControl !== 'early-startup-wake' && mode !== 'normal') throw new Error('OWNED startup control requires normal mode');
    if (options.readiness) {
      const ready = options.readiness;
      const matched = /^\\\\\.\\pipe\\nstu-owned-([0-9a-f]{32})-([0-9a-f]{16})$/.exec(ready.pipeName);
      if (!matched || matched[2] !== ready.generation || !/^[0-9a-f]{64}$/.test(ready.nonce) || !/^[A-Fa-f0-9]{64}$/.test(ready.sourceScriptSha256)) throw new Error('Invalid readiness identity');
      identity = { runId: matched[1], generation: matched[2], nonce: ready.nonce, pipeName: ready.pipeName };
    } else if (mode === 'forced-2000ms') throw new Error('Forced timeout requires native readiness');
    const application = await applicationPath(options.application, entries);
    if ([application, ...argv].map(quoteWindowsArg).join(' ').length + 1 > 32767) throw new Error('Worker Unicode command line exceeds bound');
    const cwd = await realpath(text(options.cwd, 'cwd')); if (!(await stat(cwd)).isDirectory()) throw new Error('Worker cwd is not a directory');
    if (livenessFaultNames.has(options.testFault)) {
      if (mode !== 'normal' || argv.length !== 4 || argv[2] !== 'liveness-root') throw new Error('Liveness control actor/mode boundary mismatch');
      const base = await realpath(tmpdir()), offset = relative(base, cwd);
      if (!offset.startsWith('nstu-owned-liveness-') || offset.includes('/') || offset.includes('\\') || offset === '..') throw new Error('Liveness control requires one owned temporary leaf');
      const config = decodeControlJson(Buffer.from(argv[3]));
      if (!config || Object.keys(config).sort().join(',') !== 'exitCode,foreignPID,scenario,token' ||
          !/^[a-f0-9]{64}$/.test(config.token) || config.scenario !== options.testFault || ![0, 259].includes(config.exitCode) ||
          (options.testFault === 'liveness-foreign' ? !Number.isSafeInteger(config.foreignPID) || config.foreignPID <= 0 || config.foreignPID > 0xFFFFFFFF : config.foreignPID !== null)) throw new Error('Invalid source-bound liveness fixture configuration');
      const marker = await readFile(join(cwd, '.liveness-owner.json'));
      if (binding.scenarioInputs['.liveness-owner.json'] !== digest(marker) || JSON.stringify(decodeControlJson(marker)) !== JSON.stringify(config)) throw new Error('Liveness fixture marker/source binding mismatch');
    }
    request = { v: 1, type: 'LAUNCH', runId: identity.runId, nonce: identity.nonce, generation: identity.generation,
      sourceBindingSha256: binding.sha256, seq: 1, application, argv, cwd, envEntries: entries,
      budgetsMs: deadline, mode, sourceBinding: binding, readiness: options.readiness ? {
        pipeName: identity.pipeName, nonce: identity.nonce, generation: identity.generation,
        sourceScriptSha256: options.readiness.sourceScriptSha256.toUpperCase(), sourceScriptPath: resolve(options.readiness.sourceScriptPath || argv[0]) } : null,
      testFault: options.testFault || null };
    if (encodeControlFrame(request).length > 256 * 1024 + 4) throw new Error('Launch frame exceeds bound');
  } catch (error) {
    empty.execution.primaryErrors.push(safeError('setup-failure', error.message, error)); return empty;
  }
  return new Promise(resolveResult => {
    const result = { ...empty, ...identity, sourceBindingSha256: binding.sha256, sourceBinding: binding,
      fixtureRelativeLabel: options.label || 'owned-worker', controllerRuntime: { exe: runtime.exe, version: runtime.version, sha256: runtime.sha256 }, nodeTiming: timing };
    const stdout = [], stderr = [], bootstrap = [], localErrors = [];
    let child, terminal, rootPid, buffer = Buffer.alloc(0), settled = false, receivedSeq = 0, owned = false, ready = false, rootExit = false;
    let outputBytes = 0, bootstrapBytes = 0, dataCount = 0, controlCount = 0, stopSent = false, cleanupTimer, localDeadlineExpired = false;
    let localStartupFailure = false, stoppedReason = null;
    const timers = new Set();
    const logicalTimers = new Set();
    const later = (fn, ms) => {
      const timer = setTimeout(() => { timers.delete(timer); if (!settled) fn(); }, Math.max(1, ms));
      timers.add(timer); return timer;
    };
    const clear = timer => {
      if (timer && typeof timer.cancel === 'function') timer.cancel();
      else { clearTimeout(timer); timers.delete(timer); }
    };
    function laterAt(action, cutoff, { firstWakeMs = null, onRearm = null } = {}) {
      const owner = {
        handle: null, canceled: false, completed: false,
        cancel() {
          this.canceled = true;
          if (this.handle !== null) { clearTimeout(this.handle); timers.delete(this.handle); this.handle = null; }
          logicalTimers.delete(this);
        }
      };
      logicalTimers.add(owner);
      function schedule(ms) {
        if (owner.canceled || owner.completed || settled) return;
        const handle = later(() => {
          if (owner.handle === handle) owner.handle = null;
          if (owner.canceled || owner.completed || settled) return;
          const remaining = cutoff - performance.now();
          if (remaining > 0) {
            if (onRearm) onRearm(performance.now(), remaining);
            if (!owner.canceled && !settled) schedule(Math.ceil(remaining));
          } else {
            owner.completed = true; logicalTimers.delete(owner); action();
          }
        }, Math.max(1, ms));
        owner.handle = handle;
      }
      schedule(firstWakeMs === null ? Math.ceil(cutoff - performance.now()) : firstWakeMs);
      return owner;
    }
    function clearStartup(reason) {
      if (!startupTimer.canceled && !startupTimer.completed) {
        timing.startupTimerCanceledNodeMs = performance.now();
        timing.startupTimerCancelReason = reason;
      }
      clear(startupTimer);
    }
    function note(kind, message, error) { localErrors.push(safeError(kind, message, error)); }
    function finish(status = null, signal = null) {
      if (settled) return; settled = true;
      for (const owner of logicalTimers) owner.cancel();
      for (const timer of timers) clearTimeout(timer);
      timers.clear();
      timing.controllerClosedNodeMs = performance.now();
      result.controller.exitCode = status; result.controller.signal = signal;
      result.controller.bootstrapStderrBytes = bootstrapBytes;
      if (bootstrapBytes) result.controller.bootstrapErrors.push({ kind: 'controller-bootstrap', message: Buffer.concat(bootstrap).toString('utf8') });
      if (terminal) {
        result.nativeExecution = structuredClone(terminal.execution);
        result.execution = terminal.execution; result.cleanup = terminal.cleanup;
        result.controllerTiming = terminal.controllerTiming; result.bootstrap = terminal.bootstrap;
        Object.assign(result.output, terminal.output);
      } else note('controller-failure', 'Controller closed without a valid native terminal receipt');
      if (timing.stopWrite?.state === 'pending') note('controller-failure', 'Private STOP write completion was not observed before controller close');
      result.cleanup.deadlineExpired ||= localDeadlineExpired;
      result.output.stdout = Buffer.concat(stdout); result.output.stderr = Buffer.concat(stderr);
      result.output.stdoutBytes = result.output.stdout.length; result.output.stderrBytes = result.output.stderr.length;
      result.execution.primaryErrors.push(...localErrors);
      if (!terminal || localErrors.some(error => ['protocol-failure', 'controller-failure'].includes(error.kind)) || status !== 0 || signal || !result.controller.controlStdoutEOF || !result.controller.controlStderrEOF || buffer.length) {
        result.cleanup.confirmed = false;
        if (!terminal) result.execution.kind = 'controller-failure';
        else if (localErrors.some(error => error.kind === 'protocol-failure')) result.execution.kind = 'protocol-failure';
      }
      if (result.output.overflow || localErrors.some(error => error.kind === 'output-overflow')) { result.output.overflow = true; result.execution.kind = 'output-overflow'; }
      if (localStartupFailure) result.execution.kind = 'setup-failure';
      result.execution.effectiveExitCode = result.execution.kind === 'worker-exit' && result.execution.rawWorkerExitDWORD === 0 && result.execution.nativeRootWait.signaled && result.execution.primaryErrors.length === 0 && result.cleanup.confirmed && status === 0 && !signal && !result.output.overflow ? 0 : 1;
      resolveResult(result);
    }
    function emergency(reason) {
      if (settled) return;
      note('controller-failure', reason);
      localDeadlineExpired = true;
      try { child?.kill('SIGKILL'); } catch (error) { note('controller-failure', 'Held controller fallback failed', error); }
      child?.stdin.destroy(); child?.stdout.destroy(); child?.stderr.destroy();
      finish();
    }
    function armCleanup() {
      if (cleanupTimer) return;
      cleanupTimer = later(() => emergency('Native cleanup/terminal deadline exceeded; bootstrap containment is unconfirmed'), deadline.cleanupMs + deadline.controllerExitMs);
    }
    function stop(reason, source = 'local-request') {
      if (settled || terminal || stopSent) return;
      if (!stopReasons.has(reason)) throw new Error('Invalid local stop reason');
      stopSent = true; stoppedReason = reason; timing.stopRequestedNodeMs = performance.now(); timing.firstStopReason = reason; armCleanup();
      if (reason === 'startup-deadline') {
        localStartupFailure = true;
        timing.startupViolationNodeMs = timing.stopRequestedNodeMs;
        timing.startupViolationCutoffNodeMs = startupCutoff;
        timing.startupViolationSource = source;
        note('setup-failure', 'Node startup deadline exceeded; first accepted STOP remains startup-deadline');
      }
      const frame = { v: 1, type: 'STOP', runId: identity.runId, nonce: identity.nonce, generation: identity.generation, sourceBindingSha256: binding.sha256, seq: 2, reason };
      timing.stopWrite = { reason, state: 'pending', requestedNodeMs: performance.now() };
      const written = error => {
        timing.stopWrite.state = error ? 'error' : 'ok';
        timing.stopWrite.callbackNodeMs = performance.now();
        if (error) { timing.stopWrite.error = safeError('controller-failure', 'Private controller command write failed', error); note('controller-failure', 'Private controller command write failed', error); }
      };
      try { child.stdin.write(encodeControlFrame(frame), written); }
      catch (error) { written(error); }
    }
    function protocol(error) { note('protocol-failure', error.message, error); stop('parent-abort'); }
    function acceptStartupEvidence(type, receivedAt) {
      const rejectionField = type === 'OWNED' ? 'ownedRejectedNodeMs' : 'readyRejectedNodeMs';
      if (stopSent && stoppedReason !== 'startup-deadline') {
        timing[rejectionField] = receivedAt;
        note(type === 'READY' ? 'readiness-after-stop' : 'ownership-after-stop', type + ' startup evidence received after STOPPING');
        return false;
      }
      if (stopSent || receivedAt >= startupCutoff) {
        timing[rejectionField] = receivedAt;
        if (!stopSent) stop('startup-deadline', type);
        return false;
      }
      return true;
    }
    function receive(frame) {
      if (terminal) throw new Error('Control frame after terminal');
      if (!frame || typeof frame !== 'object' || Array.isArray(frame) || !payloadFields[frame.type]) throw new Error('Unknown controller frame type');
      const expected = [...commonFields, ...payloadFields[frame.type]].sort();
      if (Object.keys(frame).sort().join('\0') !== expected.join('\0')) throw new Error('Controller frame fields differ from schema');
      if (frame.v !== 1 || frame.runId !== identity.runId || frame.nonce !== identity.nonce || frame.generation !== identity.generation || frame.sourceBindingSha256 !== binding.sha256 || frame.seq !== receivedSeq + 1) throw new Error('Controller identity or sequence mismatch');
      receivedSeq = frame.seq;
      if (frame.type === 'DATA') {
        if (!owned || ++dataCount > 8192 || !['stdout', 'stderr'].includes(frame.stream) || typeof frame.bytesBase64 !== 'string') throw new Error('Invalid worker data frame');
        const bytes = Buffer.from(frame.bytesBase64, 'base64');
        if (bytes.toString('base64') !== frame.bytesBase64 || bytes.length > 16384) throw new Error('Invalid bounded worker byte payload');
        outputBytes += bytes.length;
        if (outputBytes > maxOutput) { result.output.overflow = true; note('output-overflow', 'Worker output exceeds8MiB'); stop('output-overflow'); }
        else (frame.stream === 'stdout' ? stdout : stderr).push(bytes);
      } else {
        if (++controlCount > 256) throw new Error('Controller frame count exceeds bound');
        if (frame.type === 'OWNED') {
          if (owned || frame.jobLimitFlags !== 0x2000 || frame.handleListCount !== 3 || frame.excludedHandlesNoninheritable !== true || frame.rootInOwnedJob !== true ||
              !Number.isSafeInteger(frame.rootPid) || frame.rootPid <= 0 || frame.rootPid > 0xFFFFFFFF ||
              typeof frame.rootCreationTime !== 'string' || !/^[A-F0-9]{16}$/.test(frame.rootCreationTime)) throw new Error('Native ownership assertion missing');
          rootPid = frame.rootPid;
          owned = true;
          timing.authenticatedOwnedNodeMs = performance.now();
          if (options.testStartupControl === 'stall-normal-owned-past-startup') {
            timing.testOwnedDecodedNodeMs = performance.now();
            while (performance.now() < startupCutoff + 50) { /* bounded real-clock OWNED delivery stall */ }
            timing.testOwnedReleasedNodeMs = performance.now();
          } else if (options.testStartupControl === 'stop-before-node-owned-acceptance') {
            timing.testOwnedDecodedNodeMs = performance.now();
            stop('parent-abort');
          }
          const receivedAt = performance.now();
          if (acceptStartupEvidence('OWNED', receivedAt)) {
            timing.ownedNodeMs = receivedAt;
            if (request.mode === 'normal') clearStartup('accepted-owned');
          }
        } else if (frame.type === 'READY') {
          if (!owned || ready || !request.readiness) throw new Error('Invalid native readiness evidence');
          validateReadyPeer(frame, { rootPid, sourceScriptSha256: request.readiness.sourceScriptSha256 });
          if (options.testReadyDelivery === 'stall-past-node-startup') {
            timing.testReadyDecodedNodeMs = performance.now();
            while (performance.now() < startupCutoff + 50) { /* bounded real-clock I/O delivery stall */ }
            timing.testReadyReleasedNodeMs = performance.now();
          } else if (options.testReadyDelivery === 'stop-before-node-ready-acceptance') {
            timing.testReadyDecodedNodeMs = performance.now();
            stop('parent-abort');
          }
          const receivedAt = performance.now();
          if (!acceptStartupEvidence('READY', receivedAt)) return;
          if (request.mode === 'forced-2000ms') {
            if (overallCutoff - receivedAt < 2000 + deadline.cleanupMs + deadline.controllerExitMs) throw new Error('Insufficient remaining forced timeout budget');
          }
          ready = true; timing.readyNodeMs = receivedAt; clearStartup('accepted-ready');
          if (request.mode === 'forced-2000ms') {
            timing.forcedArmedNodeMs = performance.now(); timing.forcedDeadlineNodeMs = timing.forcedArmedNodeMs + 2000;
            laterAt(() => stop('forced-2000ms'), timing.forcedDeadlineNodeMs);
          }
        } else if (frame.type === 'ROOT_EXIT') {
          if (!owned || rootExit || !frame.nativeRootWait?.signaled || !Number.isInteger(frame.rawWorkerExitDWORD) || frame.rawWorkerExitDWORD < 0 || frame.rawWorkerExitDWORD > 0xFFFFFFFF) throw new Error('Invalid native root exit evidence');
          rootExit = true; if (request.mode === 'normal') { clear(startupTimer); armCleanup(); }
        } else if (frame.type === 'TERMINAL') {
          validateControllerTerminal({ execution: frame.execution, cleanup: frame.cleanup, controllerTiming: frame.controllerTiming, output: frame.output, bootstrap: frame.bootstrap });
          if (ready && frame.cleanup.confirmed && frame.cleanup.peerWaitSignaled !== true) throw new Error('Verified readiness peer cleanup proof missing');
          terminal = frame; timing.terminalNodeMs = performance.now(); clear(startupTimer); clear(executionTimer); clear(cleanupTimer);
          later(() => emergency('Controller exit grace exceeded after terminal'), deadline.controllerExitMs);
          child.stdin.end();
        }
        result.events.push(frame.type === 'TERMINAL' ? { type: 'TERMINAL', seq: frame.seq, cleanupConfirmed: frame.cleanup.confirmed } : { ...frame });
      }
      if (options.onEvent) options.onEvent(structuredClone(frame), {
        identity: { ...identity }, sourceBindingSha256: binding.sha256, requestStop: stop,
        sendRawControl: bytes => child.stdin.write(bytes), endControl: () => child.stdin.end(),
        pauseControlOutput: () => child.stdout.pause(), resumeControlOutput: () => child.stdout.resume()
      });
    }
    const controllerEnv = runtime.environment || (() => { const env = { ...process.env }; for (const key of Object.keys(env)) if (key.toUpperCase() === 'PSMODULEPATH') delete env[key]; return env; })();
    const started = performance.now();
    const startupCutoff = started + deadline.startupMs;
    const executionCutoff = started + deadline.overallMs - deadline.cleanupMs - deadline.controllerExitMs;
    const overallCutoff = started + deadline.overallMs;
    Object.assign(timing, { startNodeMs: started, startupCutoffNodeMs: startupCutoff, executionCutoffNodeMs: executionCutoff,
      overallCutoffNodeMs: overallCutoff, startupBudgetMs: deadline.startupMs, cleanupBudgetMs: deadline.cleanupMs,
      overallBudgetMs: deadline.overallMs, ...(request.mode === 'forced-2000ms' ? { forcedIntervalMs: 2000 } : {}) });
    child = spawn(runtime.exe, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script],
      { cwd: bundleRoot, env: controllerEnv, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
    const startupTimer = laterAt(() => stop('startup-deadline', 'timer'), startupCutoff, options.testStartupControl === 'early-startup-wake' ? {
      firstWakeMs: 5,
      onRearm: observedAt => {
        timing.startupTimerRearmCount = (timing.startupTimerRearmCount || 0) + 1;
        timing.startupTimerFirstRearmNodeMs ??= observedAt;
        timing.startupTimerLastRearmNodeMs = observedAt;
      }
    } : {});
    const executionTimer = laterAt(() => stop('execution-deadline'), executionCutoff);
    laterAt(() => emergency('Overall owner deadline exceeded'), overallCutoff);
    child.stdin.on('error', error => { if (!terminal) note('controller-failure', 'Private controller stdin failed', error); });
    child.stdout.on('data', chunk => {
      if (settled) return;
      try {
        buffer = Buffer.concat([buffer, chunk]);
        while (buffer.length >= 4) {
          const length = buffer.readUInt32LE(0);
          if (!length || length > 512 * 1024) throw new Error('Invalid controller frame length');
          if (buffer.length < length + 4) break;
          const bytes = buffer.subarray(4, length + 4); buffer = buffer.subarray(length + 4);
          const frame = decodeControlJson(bytes);
          const max = frame.type === 'TERMINAL' ? 512 * 1024 : 128 * 1024;
          if (length > max) throw new Error('Controller frame exceeds type bound'); receive(frame);
        }
      } catch (error) { protocol(error); }
    });
    child.stderr.on('data', chunk => { bootstrapBytes += chunk.length; if (bootstrapBytes <= 128 * 1024) bootstrap.push(chunk); else emergency('Controller bootstrap stderr exceeds bound'); });
    child.stdout.on('end', () => { result.controller.controlStdoutEOF = true; });
    child.stderr.on('end', () => { result.controller.controlStderrEOF = true; });
    child.stdout.on('error', error => note('controller-failure', 'Private controller stdout failed', error));
    child.stderr.on('error', error => note('controller-failure', 'Private controller stderr failed', error));
    child.on('error', error => note('controller-failure', 'Controller spawn failed', error));
    child.on('close', finish);
    child.stdin.write(encodeControlFrame(request), error => { if (error) note('controller-failure', 'Private launch write failed', error); });
  });
}
