import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Evaluate only the actual pure producer functions; never boot Studio or touch autosave.
const studio = readFileSync(new URL('../exam/studio/studio.js', import.meta.url), 'utf8');
function producerFunction(name, nextFunction) {
  const start = studio.indexOf(`  function ${name}(`);
  const end = studio.indexOf(nextFunction, start);
  assert(start >= 0 && end > start, `Studio ${name} function boundary not found`);
  return new vm.Script(`(${studio.slice(start, end).trim()})`).runInNewContext();
}
const compileManifest = producerFunction('compileManifest', '  async function readRunnerAsset(');
const validateOrigin = producerFunction('validateOrigin', '  function validateExam(');
const schema = JSON.parse(readFileSync(new URL('../exam/schema/manifest.schema.json', import.meta.url), 'utf8'));
assert.equal(schema.additionalProperties, false);
assert.deepEqual(schema.properties.allowedOrigins, {
  type: 'array', maxItems: 32, uniqueItems: true,
  items: { type: 'string', minLength: 1, maxLength: 253 }
});
const authoring = {
  id: 'pkg01-origin-contract', title: 'Package origin contract test', subject: 'Test only',
  durationSeconds: 60, documents: [],
  questions: [{ id: 'q1', type: 'short_answer', prompt: 'Inert conformance fixture', points: 1 }]
};
const omitted = compileManifest(authoring);
const exported = compileManifest({ ...authoring, allowedOrigins: ['https://exam.example.test'] });
assert(!Object.hasOwn(omitted, 'allowedOrigins'));
assert.equal(exported.allowedOrigins[0], 'https://exam.example.test');
const cases = [];
function add(name, origins, accepted, kind = 'native-contract') {
  const manifest = { ...omitted, allowedOrigins: origins };
  cases.push({ name, kind, accepted, manifest: JSON.stringify(manifest),
    authoringValid: Array.isArray(origins) && origins.every(validateOrigin) });
}
cases.push({ name: 'studio-export-omitted', kind: 'studio-export', accepted: true, manifest: JSON.stringify(omitted) });
cases.push({ name: 'studio-export-nonempty', kind: 'studio-export', accepted: true, manifest: JSON.stringify(exported) });
add('schema-empty-array', [], true);
add('native-prefix-port-suffix', ['exam.test', 'HTTP://Exam.Test:80/path?x#part', 'https://exam.test:65535', '//exam.test:00001/path'], true);
add('native-max-count', Array.from({ length: 32 }, (_, i) => `room-${i}.example.test`), true);
add('native-max-length', [`${'a'.repeat(63)}.${'b'.repeat(63)}.${'c'.repeat(63)}.${'d'.repeat(61)}`], true);
add('ordinal-case-variants', ['Exam.Test', 'exam.test'], true);
for (const [name, value] of [
  ['null', null], ['scalar', 'exam.test'], ['object', { host: 'exam.test' }],
  ['number-item', [42]], ['boolean-item', [false]], ['object-item', [{ host: 'exam.test' }]],
  ['array-item', [['exam.test']]], ['null-item', [null]], ['empty-item', ['']],
  ['too-many', Array.from({ length: 33 }, (_, i) => `room-${i}.test`)],
  ['duplicate', ['exam.test', 'exam.test']], ['too-long', [`${'a'.repeat(63)}.${'b'.repeat(63)}.${'c'.repeat(63)}.${'d'.repeat(62)}`]],
  ['userinfo', ['https://user@exam.test']], ['suffix-userinfo', ['https://exam.test/path@user']],
  ['unsupported-scheme', ['ftp://exam.test']], ['scheme-without-host', ['https://']],
  ['port-zero', ['exam.test:0']], ['port-high', ['exam.test:65536']],
  ['port-six-digits', ['exam.test:000001']], ['port-empty', ['exam.test:']],
  ['port-sign', ['exam.test:+80']], ['port-letter', ['exam.test:a']],
  ['port-nonascii-digit', ['exam.test:８０']], ['ipv6', ['https://[::1]']],
  ['empty-label', ['exam..test']], ['leading-dot', ['.exam.test']], ['trailing-dot', ['exam.test.']],
  ['long-label', [`${'a'.repeat(64)}.test`]], ['leading-hyphen', ['-exam.test']],
  ['trailing-hyphen', ['exam-.test']], ['underscore', ['exam_test']], ['backslash', ['exam\\test']],
  ['wildcard', ['*.exam.test']], ['leading-space', [' exam.test']],
  ['suffix-space', ['https://exam.test/path name']], ['suffix-control', ['https://exam.test/path\n']],
  ['suffix-delete', ['https://exam.test/\x7f']], ['suffix-nonascii', ['https://exam.test/đề']]
]) add(`invalid-${name}`, value, false, 'invalid-conformance');
for (const property of ['unexpected', 'AllowedOrigins']) {
  cases.push({ name: `unknown-${property}`, kind: 'invalid-conformance', accepted: false,
    manifest: JSON.stringify({ ...exported, [property]: [] }) });
}
cases.push({ name: 'malformed-json', kind: 'invalid-conformance', accepted: false, manifest: '{' });
// Studio must preserve the existing native/stager syntax contract.
assert.equal(validateOrigin('//exam.test'), true);
assert.equal(validateOrigin('https://exam.test/đề'), false);
assert.equal(validateOrigin('https://exam.test/path name'), false);
process.stdout.write(JSON.stringify({ cases, signedManifest: JSON.stringify(exported),
  invalidOriginManifest: JSON.stringify({ ...exported, allowedOrigins: ['ftp://exam.test'] }),
  producerNativeDifferences: [] }));
