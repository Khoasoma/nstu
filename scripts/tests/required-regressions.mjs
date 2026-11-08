import { stat } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

// Shared by the standard local gate and every Windows build variant.
// These are actual current-source suites; missing inputs/runtimes are failures.
const root = resolve(fileURLToPath(new URL('../../', import.meta.url)));
// A PowerShell 7 parent can export a PSModulePath unusable by Windows PowerShell.
// Let each real child runtime initialize its own built-in module search paths.
const powerShellEnv = { ...process.env };
for (const key of Object.keys(powerShellEnv)) {
  if (key.toUpperCase() === 'PSMODULEPATH') delete powerShellEnv[key];
}
const required = [
  'exam/web/app.js', 'exam/studio/studio.js', 'exam/studio/index.html',
  'exam/studio/template.js', 'exam/studio/zip.js', 'exam/web/assets/nstu-brand.png',
  'config/ui-desktop-tokens.json', 'server/app/ui_desktop_tokens.hpp',
  'exam/shared/ui-desktop-tokens.css', 'exam/schema/manifest.schema.json',
  'tests/exam_web_recovery_tests.js', 'tests/exam_runner_template_tests.mjs',
  'tests/ui_desktop_token_tests.mjs', 'scripts/generate-ui-desktop-tokens.mjs',
  'tests/exam_origin_package_projection.mjs', 'packaging/stage-exam-package.ps1',
  'packaging/test-exam-origin-contract.ps1', 'packaging/test-exam-package-staging.ps1',
  'scripts/tests/verify-required-regressions-tests.mjs',
  'scripts/tests/owned-regression-process.mjs', 'scripts/tests/owned-regression-process.ps1',
  'scripts/tests/owned-regression-process-tests.mjs'
];
function failure(message) { throw new Error(message); }
function run(command, args, env = process.env) {
  const label = [command, ...args].join(' ');
  console.log('\n=== ' + label + ' ===');
  const child = spawnSync(command, args, { cwd: root, env, stdio: 'inherit', windowsHide: true });
  if (child.error) failure(label + ': ' + child.error.message);
  if (child.signal) failure(label + ': terminated by ' + child.signal);
  if (child.status !== 0) failure(label + ': exit ' + child.status);
}
function requireRuntime(command, version) {
  const child = spawnSync(command, [
    '-NoProfile', '-NonInteractive', '-Command', '$PSVersionTable.PSVersion.ToString()'
  ], { cwd: root, env: powerShellEnv, encoding: 'utf8', windowsHide: true });
  if (child.error || child.signal || child.status !== 0 ||
      !version.test((child.stdout || '').trim())) {
    failure('Required runtime unavailable or unsupported: ' + command +
      (child.error ? ' (' + child.error.message + ')' : ' (' + (child.stdout || '').trim() + ')'));
  }
}
async function main() {
  if (process.argv.length !== 2) failure('Usage: node scripts/tests/required-regressions.mjs');
  for (const path of required) {
    let input;
    try { input = await stat(resolve(root, path)); }
    catch { failure('Required input missing: ' + path); }
    if (!input.isFile()) failure('Required input missing or not a regular file: ' + path);
  }
  requireRuntime('pwsh.exe', /^7\./);
  requireRuntime('powershell.exe', /^5\.1\./);
  run(process.execPath, ['scripts/generate-ui-desktop-tokens.mjs', '--check']);
  run(process.execPath, ['--check', 'exam/web/app.js']);
  for (const suite of [
    'tests/exam_web_recovery_tests.js',
    'tests/exam_runner_template_tests.mjs',
    'tests/ui_desktop_token_tests.mjs'
  ]) run(process.execPath, [suite]);
  for (const runtime of ['pwsh.exe', 'powershell.exe']) {
    for (const suite of [
      'packaging/test-exam-package-staging.ps1', 'packaging/test-exam-origin-contract.ps1'
    ]) run(runtime, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', suite], powerShellEnv);
  }
  console.log('\nREQUIRED REGRESSIONS: PASS');
}
main().catch(error => {
  console.error('REQUIRED REGRESSIONS: FAIL: ' + error.message);
  // Windows status 256 truncates to zero in Git Bash. Preserve it in the
  // diagnostic above, and always return a portable nonzero failure status.
  process.exitCode = 1;
});
