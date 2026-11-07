import assert from 'node:assert/strict';
import { cp, mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, spawnSync } from 'node:child_process';

const repo = resolve(fileURLToPath(new URL('../../', import.meta.url)));
const helper = 'scripts/tests/required-regressions.mjs';
const gate = 'scripts/verify-gate.sh';
const bash = process.platform === 'win32'
  ? join(process.env.ProgramFiles || 'C:\\Program Files', 'Git/bin/bash.exe') : 'bash';
const root = await mkdtemp(join(tmpdir(), 'nstu-gate01-'));
console.log('Owned entrypoint fixture: ' + root);
const commands = { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024, windowsHide: true };
let passed = 0;
const unsafeCleanup = new Set();

async function invoke(command, args, env = process.env, timeoutMs = 600000) {
  const result = await new Promise(resolveResult => {
    const child = spawn(command, args, { cwd: root, env, windowsHide: true,
      detached: process.platform !== 'win32' });
    const stdout = [], stderr = [];
    let size = 0, error, timedOut = false, overflow = false, cleanup;
    function result(status, signal) {
      return { pid: child.pid, status, signal, error, timedOut, overflow, cleanup,
        output: Buffer.concat([...stdout, ...stderr]).toString('utf8') };
    }
    function cleanupFailure(message) {
      unsafeCleanup.add(child.pid);
      error = new Error(message);
      try { child.kill('SIGKILL'); } catch { /* Preserve fixture and report failed cleanup. */ }
      child.stdout.destroy(); child.stderr.destroy();
      clearTimeout(timer);
      resolveResult(result(child.exitCode, child.signalCode));
    }
    function stopOwnedTree() {
      if (cleanup || !child.pid) return;
      if (child.exitCode !== null || child.signalCode !== null) {
        cleanupFailure('Owned root exited with surviving streams; preserve fixture for descendant reconciliation');
        return;
      }
      // This is our still-live ChildProcess handle/PID, never a process-name kill.
      if (process.platform === 'win32') {
        const killed = spawnSync(join(process.env.SystemRoot || 'C:\\Windows', 'System32/taskkill.exe'),
          ['/PID', String(child.pid), '/T', '/F'], { ...commands, timeout: 10000 });
        cleanup = { pid: child.pid, status: killed.status, error: killed.error?.message,
          output: (killed.stdout || '') + (killed.stderr || '') };
        if (killed.status !== 0 || killed.error) {
          cleanupFailure('Exact owned process-tree cleanup failed: ' + cleanup.output);
        }
      } else {
        process.kill(-child.pid, 'SIGKILL');
        cleanup = { pid: child.pid, status: 0 };
      }
    }
    function capture(target, chunk) {
      size += chunk.length;
      if (size > commands.maxBuffer) { overflow = true; stopOwnedTree(); }
      else target.push(chunk);
    }
    child.stdout.on('data', chunk => capture(stdout, chunk));
    child.stderr.on('data', chunk => capture(stderr, chunk));
    child.on('error', caught => { error = caught; });
    const timer = setTimeout(() => { timedOut = true; stopOwnedTree(); }, timeoutMs);
    child.on('close', (status, signal) => {
      clearTimeout(timer);
      resolveResult(result(status, signal));
    });
  });
  assert.equal(result.error, undefined, 'Cannot launch actual entrypoint: ' + result.error);
  return result;
}
function rejected(result, diagnostic) {
  assert.equal(result.timedOut, false, 'Timeout is not intended failure proof');
  assert.equal(result.overflow, false, 'Output overflow is not intended failure proof');
  assert.equal(result.signal, null, 'Signal termination is not intended failure proof');
  assert.notEqual(result.status, 0, 'A rejected verification must return nonzero');
  assert.match(result.output, diagnostic);
  assert.match(result.output, /LOCAL GATE: FAIL/);
  assert.doesNotMatch(result.output, /LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
  assert.doesNotMatch(result.output, /PASS  required current-source suites/,
    'A failed required child must not be accepted before entrypoint self-tests');
  assert.doesNotMatch(result.output, /GATE01 unexpected fixture self-test reentry/,
    'A recursion guard failure is not intended helper-boundary failure proof');
}
function withoutRuntime(filename) {
  const env = { ...process.env };
  const pathKey = Object.keys(env).find(key => key.toUpperCase() === 'PATH');
  const entries = (env[pathKey] || '').split(delimiter)
    .filter(entry => !existsSync(join(entry, filename)));
  for (const key of Object.keys(env)) if (key.toUpperCase() === 'PATH') delete env[key];
  env.PATH = entries.join(delimiter);
  return env;
}
async function changed(path, bytes, scenario) {
  const filename = join(root, path), original = await readFile(filename);
  try {
    if (bytes === null) await rm(filename);
    else await writeFile(filename, bytes(original));
    await scenario();
  } finally { await writeFile(filename, original); }
}
function pass(name) { console.log('PASS: ' + name); ++passed; }

try {
  // Copy real suite implementations/assets; never substitute fake builds or passing tests.
  for (const directory of ['exam/web', 'exam/studio', 'exam/shared']) {
    await cp(join(repo, directory), join(root, directory), { recursive: true });
  }
  const files = [
    gate, '.github/workflows/windows.yml', '.gitattributes', 'config/ui-desktop-tokens.json',
    'server/app/ui_desktop_tokens.hpp', 'exam/schema/manifest.schema.json',
    'scripts/generate-ui-desktop-tokens.mjs', 'tests/exam_web_recovery_tests.js',
    'tests/exam_runner_template_tests.mjs', 'tests/ui_desktop_token_tests.mjs',
    'tests/exam_origin_package_projection.mjs', 'packaging/stage-exam-package.ps1',
    'packaging/test-exam-origin-contract.ps1', 'packaging/test-exam-package-staging.ps1'
  ];
  for (const path of files) {
    await mkdir(dirname(join(root, path)), { recursive: true });
    await cp(join(repo, path), join(root, path));
  }
  await mkdir(join(root, 'scripts/tests'), { recursive: true });
  // Allow the absent new helper during RED; the positive child control then fails.
  if (existsSync(join(repo, helper))) await cp(join(repo, helper), join(root, helper));
  const copiedSelfTest = 'scripts/tests/verify-required-regressions-tests.mjs';
  await writeFile(join(root, copiedSelfTest),
    "throw new Error('GATE01 unexpected fixture self-test reentry');\n" +
    await readFile(fileURLToPath(import.meta.url), 'utf8'));
  assert.equal((await invoke('git', ['init', '--quiet'])).status, 0);
  assert.equal((await invoke('git', ['add', '.'])).status, 0);

  const controlEnv = { ...process.env };
  for (const key of Object.keys(controlEnv)) if (key.toUpperCase() === 'PSMODULEPATH') delete controlEnv[key];
  controlEnv.PSModulePath = join(root, 'incompatible-parent-modules');
  const control = await invoke(process.execPath, [helper], controlEnv);
  assert.equal(control.timedOut, false, 'Positive real-suite control must finish');
  assert.equal(control.overflow, false);
  assert.equal(control.signal, null);
  assert.equal(control.status, 0, control.output);
  assert.match(control.output, /REQUIRED REGRESSIONS: PASS/);
  pass('actual current suites pass with runtime-owned PowerShell module paths');
  const reentry = await invoke(process.execPath, [copiedSelfTest]);
  assert.equal(reentry.timedOut, false);
  assert.equal(reentry.status, 1);
  assert.match(reentry.output, /GATE01 unexpected fixture self-test reentry/);
  assert.doesNotMatch(reentry.output, /GATE ENTRYPOINT TESTS: PASS/);
  pass('owned negative-fixture self-test reentry fails explicitly');

  for (const path of [
    'tests/exam_runner_template_tests.mjs', 'tests/ui_desktop_token_tests.mjs',
    'scripts/generate-ui-desktop-tokens.mjs', 'packaging/test-exam-origin-contract.ps1',
    'packaging/test-exam-package-staging.ps1'
  ]) {
    await changed(path, null, async () => {
      rejected(await invoke(bash, [gate]), /Required input missing/);
      pass('standard local gate rejects omitted ' + path);
    });
  }
  await changed('server/app/ui_desktop_tokens.hpp',
    bytes => Buffer.concat([bytes, Buffer.from('\n// stale generated bytes\n')]), async () => {
      rejected(await invoke(bash, [gate]), /Stale output: server\/app\/ui_desktop_tokens\.hpp/);
      pass('standard local gate rejects stale generated desktop tokens');
    });
  await changed('tests/exam_runner_template_tests.mjs',
    bytes => Buffer.concat([bytes, Buffer.from("\nassert.fail('GATE01 injected assertion failure');\n")]), async () => {
      rejected(await invoke(bash, [gate]), /GATE01 injected assertion failure/);
      pass('standard local gate propagates an actual suite assertion failure');
    });
  for (const code of [23, 256]) {
    await changed('packaging/test-exam-package-staging.ps1',
      bytes => {
        const source = bytes.toString('utf8');
        assert.equal(source.match(/\nexit 0\s*$/g)?.length, 1,
          'Exactly one reachable final staging success exit follows all actual checks/finally');
        return source.replace(/\nexit 0\s*$/, '\nexit ' + code + '\n');
      }, async () => {
        rejected(await invoke(bash, [gate]), new RegExp('test-exam-package-staging\\.ps1.*exit ' + code));
        pass('standard local gate rejects actual staging child exit ' + code);
      });
  }
  rejected(await invoke(bash, [gate], withoutRuntime('node.exe')), /node is not on PATH/);
  pass('standard local gate rejects missing Node');
  rejected(await invoke(bash, [gate], withoutRuntime('pwsh.exe')), /Required runtime.*pwsh\.exe/);
  pass('standard local gate rejects missing PowerShell 7');
  rejected(await invoke(bash, [gate], withoutRuntime('powershell.exe')), /Required runtime.*powershell\.exe/);
  pass('standard local gate rejects missing Windows PowerShell 5.1');

  // Timeout injection tests resource cleanup only, never a required-suite failure proof.
  const probeFile = join(root, 'timeout-descendant.json');
  await changed(helper, bytes => (
    "import { spawn as gateProbeSpawn } from 'node:child_process';\n" +
    "import { writeFileSync as gateProbeWrite } from 'node:fs';\n" +
    "const gateProbeChild = gateProbeSpawn(process.execPath, ['-e', 'setInterval(()=>{},1000)'], {stdio:'ignore', windowsHide:true});\n" +
    "gateProbeWrite(" + JSON.stringify(probeFile) + ", JSON.stringify({pid:gateProbeChild.pid}));\n" +
    "await new Promise(resolve => setTimeout(resolve, 60000));\n" + bytes.toString('utf8')
  ), async () => {
    const timeout = await invoke(bash, [gate], process.env, 2000);
    assert.equal(timeout.timedOut, true);
    assert.equal(timeout.cleanup?.status, 0, 'Exact owned tree cleanup must complete');
    assert.notEqual(timeout.status, 0);
    assert.doesNotMatch(timeout.output, /LOCAL GATE: PASS|REQUIRED REGRESSIONS: PASS/);
    const probe = JSON.parse(await readFile(probeFile, 'utf8'));
    for (const pid of [timeout.pid, probe.pid]) {
      let alive = true;
      for (let count = 0; count < 20 && alive; ++count) {
        try { process.kill(pid, 0); }
        catch (error) { if (error.code !== 'ESRCH') throw error; alive = false; }
        if (alive) await new Promise(resolve => setTimeout(resolve, 100));
      }
      assert.equal(alive, false, 'Owned process/descendant must be gone: ' + pid);
    }
    console.log('Owned timeout tree root/descendant: ' + timeout.pid + '/' + probe.pid);
    pass('timeout stops exact owned entrypoint and descendant; no suite failure proof');
  });

  const workflow = await readFile(join(repo, '.github/workflows/windows.yml'), 'utf8');
  for (const job of ['build-test', 'internal-vm-build', 'dev-unprotected-build', 'webview2-host-build']) {
    const start = workflow.indexOf('  ' + job + ':\n');
    assert.ok(start >= 0, job + ' must exist');
    // Job boundaries have two spaces, while nested keys have at least four.
    const block = workflow.slice(start).split(/\n(?=  [a-z][a-z0-9-]*:\n)/)[0];
    for (const path of [helper, 'scripts/tests/verify-required-regressions-tests.mjs']) {
      assert.ok(block.includes('node ' + path + '\n          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }'),
        job + ' must run ' + path + ' and propagate its native exit immediately');
    }
  }
  pass('all four Windows build variants require both checked entrypoints');
  console.log('GATE ENTRYPOINT TESTS: PASS (' + passed + ' cases)');
} finally {
  if (unsafeCleanup.size) throw new Error('Preserved owned fixture ' + root +
    ' after failed process cleanup; reconcile exact PIDs ' + [...unsafeCleanup].join(','));
  const offset = relative(resolve(tmpdir()), resolve(root));
  assert.ok(offset && offset !== '..' && !offset.startsWith('..' + sep) && !offset.includes(sep),
    'Cleanup must stay in the owned temporary leaf');
  assert.ok(offset.startsWith('nstu-gate01-'));
  await rm(root, { recursive: true, force: true });
}
