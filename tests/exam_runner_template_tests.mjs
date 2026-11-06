import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { createHash } from 'node:crypto';

const root = new URL('../', import.meta.url);
const context = vm.createContext({ window: {} });
vm.runInContext(await readFile(new URL('exam/studio/template.js', root), 'utf8'), context);
const assets = {indexHtml: 'index.html', stylesCss: 'styles.css', appJs: 'app.js'};
for (const [key, filename] of Object.entries(assets)) {
  const canonical = await readFile(new URL(`exam/web/${filename}`, root));
  const embedded = context.window.NSTU_RUNNER_TEMPLATE[key];
  assert.equal(typeof embedded, 'string', `${key} must contain asset text, not metadata`);
  assert.ok(embedded.length > 0, `${key} must not be empty`);
  assert.deepEqual(Buffer.from(embedded, 'utf8'), canonical,
    `${filename} must survive export with every source byte intact`);
}
console.log('Runner template literal strings and canonical byte parity: PASS');

const brand = context.window.NSTU_RUNNER_TEMPLATE.brandPngBase64;
assert.equal(typeof brand, 'string', 'Bundled brand image must be base64 text');
const png = Buffer.from(brand, 'base64');
assert.equal(png.toString('base64'), brand, 'Brand base64 must be canonical');
assert.deepEqual(png, await readFile(new URL('exam/web/assets/nstu-brand.png', root)));
assert.equal(png.length, 171569);
assert.equal(createHash('sha256').update(png).digest('hex'),
  '0b3cf7023212288d978bd898d0fa9ec1dcb7e82c92b678fa2bd1d42e18272486');
console.log('Bundled original PNG byte and hash parity: PASS');
