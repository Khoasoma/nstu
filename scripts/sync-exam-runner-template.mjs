import { readFile, writeFile } from 'node:fs/promises';

const args = process.argv.slice(2);
if (args.length > 1 || (args.length === 1 && args[0] !== '--check')) {
  throw new Error('Usage: node scripts/sync-exam-runner-template.mjs [--check]');
}
const assets = {indexHtml: 'index.html', stylesCss: 'styles.css', appJs: 'app.js', programmingJs: '../shared/programming.js'};
const runner = {};
for (const [key, filename] of Object.entries(assets)) {
  const bytes = await readFile(new URL(`../exam/web/${filename}`, import.meta.url));
  // Retain BOM, CRLF and all UTF8 bytes. Reject invalid input rather than
  // silently replacing it with characters that would change exported hashes.
  const value = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(bytes);
  if (!Buffer.from(value, 'utf8').equals(bytes)) {
    throw new Error(`${filename} cannot round-trip as UTF8`);
  }
  runner[key] = value;
}
const brandPng = await readFile(new URL('../exam/web/assets/nstu-brand.png', import.meta.url));
runner.brandPngBase64 = brandPng.toString('base64');
const output = '// Generated from exam/web by scripts/sync-exam-runner-template.mjs.\n' +
  `window.NSTU_RUNNER_TEMPLATE = ${JSON.stringify(runner)};\n`;
const target = new URL('../exam/studio/template.js', import.meta.url);
if (args[0] === '--check') {
  if (!(await readFile(target)).equals(Buffer.from(output, 'utf8'))) {
    throw new Error('Runner template is stale; run node scripts/sync-exam-runner-template.mjs');
  }
  console.log('Runner template is current.');
} else {
  await writeFile(target, output, 'utf8');
  console.log('Updated exam/studio/template.js from canonical runner assets.');
}
