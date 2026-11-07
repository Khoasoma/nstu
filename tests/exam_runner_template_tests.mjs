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

// Fault-inject the actual private reader helper without exporting production APIs.
const studioSource = await readFile(new URL('exam/studio/studio.js', root), 'utf8');
const readerStart = studioSource.indexOf('  async function readRunnerAsset(');
const readerEnd = studioSource.indexOf('  async function verifiedBrandPng(', readerStart);
assert.ok(readerStart >= 0 && readerEnd > readerStart, 'asset-reader source boundary must exist');
const readerSource = studioSource.slice(readerStart, readerEnd).trim();
async function readerScenario(options = {}) {
  let reads = 0, cancelled = 0, released = 0;
  const reader = {
    async read() {
      if (options.readFails) throw options.readError;
      const chunk = (options.chunks || [new Uint8Array([1, 2]), new Uint8Array([3])])[reads++];
      return chunk ? { value: chunk, done: false } : { done: true };
    },
    async cancel() { ++cancelled; if (options.cancelFails) throw options.cancelError; },
    releaseLock() { ++released; if (options.releaseFails) throw options.releaseError; }
  };
  const assetReader = vm.runInNewContext(`(${readerSource})`, {
    URL, Uint8Array, location: { origin: 'https://studio.example.test' },
    fetch: async (path, policy) => {
      assert.equal(path, '../web/test.asset');
      assert.equal(policy.redirect, 'error');
      return { ok: true, url: 'https://studio.example.test/web/test.asset', body: { getReader: () => reader } };
    }
  });
  let failed = false, error, bytes;
  try { bytes = await assetReader('../web/test.asset', options.maximumBytes ?? 8); }
  catch (caught) { failed = true; error = caught; }
  assert.equal(cancelled, 1, 'cleanup must attempt cancellation exactly once');
  assert.equal(released, 1, 'cleanup must release the lock even after cancellation failure');
  return { failed, error, bytes };
}
const cancellationFailure = new Error('injected cancellation failure');
const releaseFailure = new Error('injected release failure');
const readFailure = new Error('injected read failure');
const success = await readerScenario();
assert.equal(success.failed, false);
assert.deepEqual([...success.bytes], [1, 2, 3]);
const cancelOnly = await readerScenario({ cancelFails: true, cancelError: cancellationFailure });
assert.equal(cancelOnly.failed, true);
assert.equal(cancelOnly.error, cancellationFailure);
const readAndCancel = await readerScenario({ readFails: true, readError: readFailure,
  cancelFails: true, cancelError: cancellationFailure });
assert.equal(readAndCancel.failed, true);
assert.equal(readAndCancel.error, readFailure, 'read failure takes precedence over cleanup failure');
const sizeAndCancel = await readerScenario({ maximumBytes: 1,
  cancelFails: true, cancelError: cancellationFailure });
assert.equal(sizeAndCancel.failed, true);
assert.equal(sizeAndCancel.error.message, 'Runner asset exceeds limit: ../web/test.asset');
const readAndRelease = await readerScenario({ readFails: true, readError: readFailure,
  releaseFails: true, releaseError: releaseFailure });
assert.equal(readAndRelease.failed, true);
assert.equal(readAndRelease.error, readFailure);
const bothCleanup = await readerScenario({ cancelFails: true, cancelError: cancellationFailure,
  releaseFails: true, releaseError: releaseFailure });
assert.equal(bothCleanup.failed, true);
assert.equal(bothCleanup.error, cancellationFailure, 'first cleanup failure retains precedence');
const releaseOnly = await readerScenario({ releaseFails: true, releaseError: releaseFailure });
assert.equal(releaseOnly.failed, true);
assert.equal(releaseOnly.error, releaseFailure);
const undefinedRead = await readerScenario({ readFails: true, readError: undefined,
  cancelFails: true, cancelError: cancellationFailure, releaseFails: true, releaseError: releaseFailure });
assert.equal(undefinedRead.failed, true);
assert.equal(undefinedRead.error, undefined, 'undefined is still an original thrown failure');
console.log('Studio asset-reader cleanup and failure precedence: 8 scenarios passed');

// Execute the actual init event bindings with inert renderer dependencies.
const initStart = studioSource.indexOf('  function init() {');
const initEnd = studioSource.indexOf('  // Start application on DOM ready', initStart);
const compileStart = studioSource.indexOf('  function compileManifest(exam) {');
const compileEnd = studioSource.indexOf('  async function readRunnerAsset(', compileStart);
assert.ok(initStart >= 0 && initEnd > initStart && compileStart >= 0 && compileEnd > compileStart);
const fieldNodes = new Map();
function fieldNode(id) {
  if (!fieldNodes.has(id)) fieldNodes.set(id, {
    value: '', children: [], listeners: {},
    classList: { add() {}, remove() {} }, setAttribute() {},
    addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  });
  return fieldNodes.get(id);
}
const fields = vm.runInNewContext(`(() => {
  let currentExam = { id: 'placeholder-test', title: 'Test', durationSeconds: 600, questions: [
    { id: 'short-a', type: 'short_answer', prompt: 'A', points: 1, answerPlaceholder: 'Original A' },
    { id: 'short-b', type: 'short_answer', prompt: 'B', points: 1, answerPlaceholder: 'Original B' },
    { id: 'essay-c', type: 'essay', prompt: 'C', points: 1, answerPlaceholder: 'Original C' }
  ] };
  let activeQuestionIndex = 0, previewIndex = 0;
  const el = getNode;
  const setMode = () => {}, renderEditor = () => {}, initSplitter = () => {},
    renderPreview = () => {}, showToast = () => {}, handleImportFile = () => {},
    saveDraft = () => {}, exportPackage = () => {}, addQuestion = () => {};
  ${studioSource.slice(compileStart, compileEnd)}
  ${studioSource.slice(initStart, initEnd)}
  init();
  return { exam: currentExam, setIndex(index) { activeQuestionIndex = index; },
    manifest() { return compileManifest(currentExam); } };
})()`, { getNode: fieldNode, document: { documentElement: { dataset: {} } },
  localStorage: { getItem() { return null; }, setItem() {} } });
function placeholderInput(value) {
  const node = fieldNode('q-sa-placeholder'); node.value = value;
  for (const callback of node.listeners.input || []) callback({ target: node });
}
const authoredPlaceholder = 'Nhập từ chính xác <b>literal</b> & one number';
placeholderInput(authoredPlaceholder);
assert.equal(fields.exam.questions[0].answerPlaceholder, authoredPlaceholder);
assert.equal(fields.exam.questions[1].answerPlaceholder, 'Original B');
assert.equal(fields.manifest().questions[0].answerPlaceholder, authoredPlaceholder);
fields.setIndex(1); placeholderInput('Independent B');
assert.equal(fields.exam.questions[0].answerPlaceholder, authoredPlaceholder);
assert.equal(fields.exam.questions[1].answerPlaceholder, 'Independent B');
fields.setIndex(0); placeholderInput('');
assert.equal(fields.exam.questions[0].answerPlaceholder, '');
assert.equal(fields.manifest().questions[0].answerPlaceholder, undefined);
fields.setIndex(2); placeholderInput('Must not change essay data');
assert.equal(fields.exam.questions[2].answerPlaceholder, 'Original C');
fields.setIndex(999); placeholderInput('Missing target event');
assert.equal(fields.exam.questions.length, 3);
console.log('Studio short-answer placeholder binding and manifest: 6 scenarios passed');

const renderStart = studioSource.indexOf('  function renderEditor() {');
const renderEnd = studioSource.indexOf('  function renderActiveQuestionForm()', renderStart);
const addStart = studioSource.indexOf('  function addQuestion(type) {');
const addEnd = studioSource.indexOf('  let previewIndex = 0;', addStart);
const syncStart = studioSource.indexOf('  function syncExamMetadata() {');
const syncEnd = studioSource.indexOf('  async function exportPackage()', syncStart);
const originsStart = studioSource.indexOf('  function parseOriginsInput(raw) {');
const originsEnd = studioSource.indexOf('  function validateOrigin(', originsStart);
assert.ok([renderStart, renderEnd, addStart, addEnd, syncStart, syncEnd, originsStart, originsEnd].every(index => index >= 0));
const transitionNodes = new Map();
function transitionNode(id = '') {
  const node = { value: '', children: [], listeners: {}, textContent: '',
    addEventListener(type, callback) { (this.listeners[type] ||= []).push(callback); },
    appendChild(child) { this.children.push(child); } };
  Object.defineProperty(node, 'innerHTML', { set(value) { this.html = value; this.children = []; }, get() { return this.html; } });
  if (id) transitionNodes.set(id, node);
  return node;
}
const transitions = vm.runInNewContext(`(() => {
  let currentExam = { id: 'old-id', title: 'Old title', subject: 'Old subject', durationSeconds: 600,
    questions: [{ id: 'a', type: 'short_answer', prompt: 'A' }, { id: 'b', type: 'short_answer', prompt: 'B' }] };
  let activeQuestionIndex = 0;
  const previewAnswers = {}; // Existing addQuestion now reads retained Preview identities.
  const el = getNode, escapeHtml = value => value;
  const renderActiveQuestionForm = () => {}, showToast = () => {};
  ${studioSource.slice(renderStart, renderEnd)}
  ${studioSource.slice(addStart, addEnd)}
  ${studioSource.slice(originsStart, originsEnd)}
  ${studioSource.slice(syncStart, syncEnd)}
  renderEditor();
  return { exam: currentExam, add: addQuestion, selectedIndex() { return activeQuestionIndex; } };
})()`, { getNode: id => transitionNodes.get(id) || transitionNode(id),
  document: { createElement: () => transitionNode() } });
function editTransitionMetadata(title) {
  const values = { 'exam-id': ' new-id ', 'exam-title': title, 'exam-subject': ' New subject ',
    'exam-duration': '43', 'exam-allowed-origins': ' https://example.test\nhttps://example.test ' };
  for (const [id, value] of Object.entries(values)) transitionNodes.get(id).value = value;
}
function assertTransitionMetadata(title) {
  assert.equal(transitions.exam.id, 'new-id');
  assert.equal(transitions.exam.title, title);
  assert.equal(transitions.exam.subject, 'New subject');
  assert.equal(transitions.exam.durationSeconds, 2580);
  assert.deepEqual([...transitions.exam.allowedOrigins], ['https://example.test']);
  assert.equal(transitionNodes.get('exam-title').value, title);
}
function clickQuestion(index, remove = false) {
  const row = transitionNodes.get('question-nav-list').children[index];
  row.listeners.click[0]({ target: { closest: () => remove ? {} : null } });
}
editTransitionMetadata('Selection metadata'); clickQuestion(1); assertTransitionMetadata('Selection metadata');
assert.equal(transitions.selectedIndex(), 1);
assert.deepEqual([...transitions.exam.questions].map(q => q.id), ['a', 'b']);
editTransitionMetadata('Added metadata'); transitions.add('short_answer'); assertTransitionMetadata('Added metadata');
assert.equal(transitions.exam.questions.length, 3); assert.equal(transitions.selectedIndex(), 2);
const addedQuestion = JSON.stringify(transitions.exam.questions[2]);
editTransitionMetadata('Deleted metadata'); clickQuestion(0, true); assertTransitionMetadata('Deleted metadata');
assert.equal(transitions.exam.questions.length, 2); assert.equal(transitions.selectedIndex(), 1);
assert.equal(JSON.stringify(transitions.exam.questions[1]), addedQuestion);
clickQuestion(0, true); assert.equal(transitions.exam.questions.length, 1);
editTransitionMetadata('Last-question guard metadata'); clickQuestion(0, true);
assertTransitionMetadata('Last-question guard metadata');
assert.equal(transitions.exam.questions.length, 1);
assert.equal(JSON.stringify(transitions.exam.questions[0]), addedQuestion);
console.log('Studio metadata on select/add/delete/last-question guard: 4 scenarios passed');

function essayLimitInput(value, valid = true, number = Number(value)) {
  const node = fieldNode('q-essay-limit');
  node.value = value; node.valueAsNumber = number; node.validity = { valid };
  for (const callback of node.listeners.input || []) callback({ target: node });
}
fields.setIndex(2);
fields.exam.questions[2].wordLimit = 300;
essayLimitInput('420');
assert.equal(fields.exam.questions[2].wordLimit, 420);
assert.equal(fields.manifest().questions[2].wordLimit, 420);
for (const [input, numeric] of [['9', 9], ['2001', 2001], ['21.5', 21.5], ['invalid', NaN]]) {
  essayLimitInput(input, false, numeric);
  assert.equal(fields.exam.questions[2].wordLimit, 420, 'invalid/intermediate input must preserve previous model value');
}
essayLimitInput('10'); assert.equal(fields.exam.questions[2].wordLimit, 10);
essayLimitInput('2000'); assert.equal(fields.exam.questions[2].wordLimit, 2000);
essayLimitInput('21.5', true, 21.5); assert.equal(fields.exam.questions[2].wordLimit, 2000);
essayLimitInput('', false, NaN); assert.equal(fields.exam.questions[2].wordLimit, 2000,
  'invalid number input may expose an empty value without representing optional clearing');
essayLimitInput('');
assert.equal(Object.hasOwn(fields.exam.questions[2], 'wordLimit'), false);
assert.equal(Object.hasOwn(fields.manifest().questions[2], 'wordLimit'), false);
fields.setIndex(0); essayLimitInput('100');
assert.equal(Object.hasOwn(fields.exam.questions[0], 'wordLimit'), false);
fields.setIndex(999); essayLimitInput('100'); assert.equal(fields.exam.questions.length, 3);
assert.equal(fields.exam.questions[2].answerPlaceholder, 'Original C');
console.log('Studio suggested essay limit binding/optional/invalid/type cases: 12 scenarios passed');

function generatedIdScenario(type, existingIds, retiredAnswers = {}) {
  const model = existingIds.map((id, index) => ({ id, type: 'short_answer', prompt: `Existing ${index}`, answerPlaceholder: `Data ${index}` }));
  const before = JSON.stringify(model);
  const result = vm.runInNewContext(`(() => {
    let currentExam = { questions: model }, activeQuestionIndex = 0;
    const previewAnswers = retiredAnswers;
    const syncExamMetadata = () => {}, renderEditor = () => {}, showToast = () => {};
    ${studioSource.slice(addStart, addEnd)}
    addQuestion(type);
    return { questions: currentExam.questions, activeQuestionIndex };
  })()`, { model, retiredAnswers, type });
  assert.equal(JSON.stringify(result.questions.slice(0, -1)), before, 'existing question IDs/data must remain unchanged');
  assert.equal(result.activeQuestionIndex, existingIds.length);
  assert.equal(new Set(result.questions.map(q => q.id)).size, result.questions.length);
  assert.equal(Object.hasOwn(retiredAnswers, result.questions.at(-1).id), false,
    'a new identity cannot bind to retained Preview answers');
  return result.questions.at(-1);
}
const generatedSuffixes = { matching: 'matching', reading: 'reading', multiple_choice: 'mc',
  short_answer: 'short-answer', essay: 'essay', listening: 'listening' };
for (const [type, suffix] of Object.entries(generatedSuffixes)) {
  const defaultQuestion = generatedIdScenario(type, ['old-a', 'old-b']);
  assert.equal(defaultQuestion.id, `q3-${suffix}`, 'non-collision IDs keep their existing form');
  const liveCollisions = generatedIdScenario(type, [`q3-${suffix}`, `q4-${suffix}`], { [`q5-${suffix}`]: 'Retired data' });
  assert.equal(liveCollisions.id, `q6-${suffix}`);
  const retiredOnly = generatedIdScenario(type, ['old-a', 'old-b'], { [`q3-${suffix}`]: '' });
  assert.equal(retiredOnly.id, `q4-${suffix}`, 'even an empty retained Preview value reserves its ID');
  assert.equal(defaultQuestion.type, type === 'matching' ? 'reading' : type);
}
console.log('Studio generated IDs across live and retained Preview collisions: 18 scenarios passed');

const importStart = studioSource.indexOf('  async function handleImportFile(file) {');
const importEnd = studioSource.indexOf('  function saveDraft()', importStart);
const previewStateStart = studioSource.indexOf('  let previewIndex = 0;');
const previewStateEnd = studioSource.indexOf('  function renderPreview()', previewStateStart);
assert.ok(importStart >= 0 && importEnd > importStart && previewStateStart >= 0 && previewStateEnd > previewStateStart);
const replacementNodes = new Map();
function replacementNode(id) {
  if (!replacementNodes.has(id)) replacementNodes.set(id, {
    listeners: {}, style: { display: 'none' }, setAttribute() {}, classList: { add() {}, remove() {} },
    addEventListener(type, callback) { (this.listeners[type] ||= []).push(callback); }
  });
  return replacementNodes.get(id);
}
let zipManifest = { id: 'zip-new', questions: [{ id: 'shared', type: 'short_answer', prompt: 'ZIP prompt' }] };
const replacements = vm.runInNewContext(`(() => {
  const DEFAULT_EXAM = { id: 'sample', questions: [{ id: 'shared', type: 'short_answer', prompt: 'Sample prompt' }] };
  let currentExam = { id: 'original', questions: [{ id: 'shared' }, { id: 'last' }] };
  let activeQuestionIndex = 1, previewAnswers = {}, selectedTokenForPlacement = null;
  let renderCount = 0, previewRenders = 0;
  const el = getNode, renderEditor = () => { ++renderCount; }, showToast = () => {};
  const setMode = () => {}, saveDraft = () => {}, exportPackage = () => {},
    addQuestion = () => {}, renderPreview = () => { ++previewRenders; }, initSplitter = () => {};
  ${studioSource.slice(previewStateStart, previewStateEnd)}
  ${studioSource.slice(importStart, importEnd)}
  ${studioSource.slice(initStart, initEnd)}
  init();
  return { importFile: handleImportFile,
    seedPreview() { previewIndex = 3; previewAnswers = { shared: 'Old answer' }; selectedTokenForPlacement = 2; activeQuestionIndex = 1; },
    inspect() { return { currentExam, previewIndex, previewAnswers, selectedTokenForPlacement, activeQuestionIndex, renderCount, previewRenders }; },
    sample() { el('btn-load-sample').listeners.click[0](); } };
})()`, { getNode: replacementNode, TextDecoder, document: { documentElement: { dataset: {} } },
  localStorage: { getItem() { return null; }, setItem() {} },
  NstuZip: { Reader: class { async readEntries() {
    return [{ path: 'manifest.json', bytes: new TextEncoder().encode(JSON.stringify(zipManifest)) }];
  } } } });
function assertFreshPreview(expectedId) {
  const state = replacements.inspect();
  assert.equal(state.currentExam.id, expectedId);
  assert.equal(state.activeQuestionIndex, 0);
  assert.equal(state.previewIndex, 0, 'replacement resets the old Preview cursor');
  assert.equal(Object.keys(state.previewAnswers).length, 0, 'replacement cannot reuse old answers by question ID');
  assert.equal(state.selectedTokenForPlacement, null);
}
replacements.seedPreview();
await replacements.importFile({ name: 'fixture.json', text: async () => JSON.stringify({ id: 'json-new', questions: [{ id: 'shared', type: 'short_answer', prompt: 'JSON prompt' }] }) });
assertFreshPreview('json-new');
replacements.seedPreview();
await replacements.importFile({ name: 'fixture.nstuexam', arrayBuffer: async () => new ArrayBuffer(0) });
assertFreshPreview('zip-new');
replacements.seedPreview(); replacements.sample(); assertFreshPreview('sample');
assert.equal(replacements.inspect().currentExam.questions[0].prompt, 'Sample prompt');
for (const file of [
  { name: 'broken.json', text: async () => '{not valid' },
  { name: 'invalid.json', text: async () => JSON.stringify({ questions: [] }) },
  { name: 'not-supported.txt' }
]) {
  replacements.seedPreview(); const before = replacements.inspect();
  await replacements.importFile(file);
  const after = replacements.inspect();
  assert.equal(after.currentExam, before.currentExam);
  assert.equal(after.previewAnswers, before.previewAnswers);
  assert.equal(after.previewIndex, 3);
  assert.equal(after.selectedTokenForPlacement, 2);
  assert.equal(after.activeQuestionIndex, 1);
  assert.equal(after.renderCount, before.renderCount);
}
replacementNode('preview-view').style.display = 'flex';
replacements.seedPreview(); const previewRendersBefore = replacements.inspect().previewRenders;
await replacements.importFile({ name: 'active.json', text: async () => JSON.stringify({ id: 'active-replacement', questions: [{ id: 'shared', type: 'short_answer', prompt: 'New active Preview' }] }) });
assertFreshPreview('active-replacement');
assert.equal(replacements.inspect().previewRenders, previewRendersBefore + 1);
const unchangedPreview = replacements.inspect();
await replacements.importFile({ name: 'broken.json', text: async () => '{broken' });
assert.equal(replacements.inspect().previewRenders, unchangedPreview.previewRenders);
assert.equal(replacements.inspect().currentExam, unchangedPreview.currentExam);
console.log('Studio Preview reset/refresh on model replacement: 8 scenarios passed');

const exportStart = studioSource.indexOf('  let exportGeneration =') >= 0
  ? studioSource.indexOf('  let exportGeneration =') : studioSource.indexOf('  async function exportPackage() {');
const exportEnd = studioSource.indexOf('  async function handleImportFile(', exportStart);
assert.ok(exportStart >= 0 && exportEnd > exportStart);
function exportLifecycleHarness() {
  const nodes = new Map(), pending = [], downloads = [];
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, { style: {}, classList: { add() {} }, innerHTML: '', textContent: '', disabled: false });
    return nodes.get(id);
  };
  const model = { id: 'old-export', title: 'Old content', questions: [] };
  const exporter = vm.runInNewContext(`(() => {
    let currentExam = model;
    const el = getNode, syncExamMetadata = () => {},
      validateExam = exam => exam.title ? [] : ['Invalid title'],
      compileManifest = exam => ({ id: exam.id, title: exam.title }),
      verifiedBrandPng = async bytes => bytes, showToast = () => {}, escapeHtml = value => value;
    ${studioSource.slice(exportStart, exportEnd)}
    return { exportPackage };
  })()`, { model, getNode: node, Uint8Array, window: { NSTU_RUNNER_TEMPLATE: {
    indexHtml: 'HTML', stylesCss: 'CSS', appJs: 'JS', brandPngBase64: 'A'.repeat(228760) } },
    atob: () => 'PNG', btoa: () => 'A'.repeat(228760), NstuZip: { Writer: class {
      constructor() { this.entries = new Map(); }
      add(name, bytes) { this.entries.set(name, bytes); }
      build() { return new Promise((resolve, reject) => pending.push({ resolve, reject,
        manifest: JSON.parse(this.entries.get('manifest.json')) })); }
    } }, URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} },
    document: { createElement: () => ({ click() { downloads.push(this.download); } }) } });
  const result = id => ({ fileCount: 5, totalBytes: 100, archiveSha256: id + '-archive', contentSha256: id + '-content', blob: {} });
  async function start() {
    const promise = exporter.exportPackage();
    for (let i = 0; i < 10; ++i) await Promise.resolve();
    return { promise };
  }
  return { model, node, pending, downloads, result, start };
}
{
  const h = exportLifecycleHarness(), first = await h.start();
  assert.equal(h.pending.length, 1);
  h.model.id = 'new-export'; h.model.title = '';
  await (await h.start()).promise;
  const latestStatus = h.node('export-validation-status').innerHTML;
  h.pending[0].resolve(h.result('old')); await first.promise;
  assert.equal(h.node('export-validation-status').innerHTML, latestStatus);
  assert.equal(h.node('btn-download-nstuexam').disabled, true);
}
{
  const h = exportLifecycleHarness(), first = await h.start();
  h.model.id = 'new-export'; h.model.title = 'New content'; const second = await h.start();
  h.pending[1].resolve(h.result('new')); await second.promise;
  h.pending[0].resolve(h.result('old')); await first.promise;
  assert.equal(h.node('lbl-content-hash').textContent, 'new-content');
  h.node('btn-download-nstuexam').onclick(); assert.deepEqual(h.downloads, ['new-export.nstuexam']);
  assert.equal(h.pending[1].manifest.id, 'new-export');
}
{
  const h = exportLifecycleHarness(), first = await h.start();
  h.model.title = ''; await (await h.start()).promise;
  const latestStatus = h.node('export-validation-status').innerHTML;
  h.pending[0].reject(new Error('Obsolete build error')); await first.promise;
  assert.equal(h.node('export-validation-status').innerHTML, latestStatus);
  assert.equal(h.node('btn-download-nstuexam').disabled, true);
}
{
  const h = exportLifecycleHarness(), first = await h.start();
  assert.equal(h.pending[0].manifest.id, 'old-export');
  h.model.id = 'edited-while-building'; h.pending[0].resolve(h.result('old')); await first.promise;
  assert.match(h.node('lbl-stage-command').textContent, /old-export\.nstuexam/);
  h.node('btn-download-nstuexam').onclick(); assert.deepEqual(h.downloads, ['old-export.nstuexam']);
}
{
  const h = exportLifecycleHarness(), first = await h.start();
  h.pending[0].reject(new Error('Current build error')); await first.promise;
  assert.match(h.node('export-validation-status').innerHTML, /Current build error/);
  assert.equal(h.node('btn-download-nstuexam').disabled, true);
}
console.log('Studio asynchronous export ownership and manifest identity: 5 scenarios passed');

// Run the whole actual controller and renderers; only DOM/platform I/O are boundaries.
class ImportDomNode {
  constructor(id = '') { this.id = id; this.children = []; this.style = {}; this.dataset = {}; this.listeners = {}; this.className = ''; this._html = ''; this._value = ''; this.textContent = ''; this.queries = new Map(); }
  get value() { return this._value; }
  set value(value) { this._value = String(value ?? ''); }
  get innerHTML() { return this._html; }
  set innerHTML(value) { this._html = String(value); this.children = []; this.queries.clear(); }
  get classList() { return { add: name => { this.className += ' ' + name; }, remove: name => { this.className = this.className.split(' ').filter(x => x !== name).join(' '); }, contains: name => this.className.split(' ').includes(name) }; }
  appendChild(child) { this.children.push(child); child.parentNode = this; return child; }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  querySelector(selector) { if (!this.queries.has(selector)) this.queries.set(selector, new ImportDomNode(selector)); return this.queries.get(selector); }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  setAttribute(name, value) { this[name] = String(value); }
  getBoundingClientRect() { return { left: 0, width: 1000 }; }
  remove() { if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(child => child !== this); }
  click() { (this.listeners.click || []).forEach(listener => listener({ target: this, preventDefault() {}, stopPropagation() {} })); }
}

const zipSource = await readFile(new URL('exam/studio/zip.js', root), 'utf8');
const importInstrumentation = `
  const importStats = { reset:0, editor:0, preview:0, matching:0, mc:0, forms:[] };
  const originalImportReset=resetPreviewState, originalImportEditor=renderEditor,
    originalImportPreview=renderPreview, originalImportMatching=renderPreviewMatching,
    originalImportMc=renderPreviewMc, originalImportForm=renderActiveQuestionForm;
  resetPreviewState=()=>{importStats.reset++;return originalImportReset();};
  renderEditor=()=>{importStats.editor++;return originalImportEditor();};
  renderPreview=()=>{importStats.preview++;return originalImportPreview();};
  renderPreviewMatching=q=>{importStats.matching++;return originalImportMatching(q);};
  renderPreviewMc=q=>{importStats.mc++;return originalImportMc(q);};
  renderActiveQuestionForm=()=>{importStats.forms.push(currentExam.questions[activeQuestionIndex]?.type);return originalImportForm();};
  window.importProbe={
    load:handleImportFile, stats:importStats,
    inspect:()=>({exam:currentExam,active:activeQuestionIndex,index:previewIndex,answers:previewAnswers,token:selectedTokenForPlacement}),
    seed:(exam,answers,open)=>{currentExam=exam;activeQuestionIndex=1;previewIndex=0;previewAnswers=answers;selectedTokenForPlacement=1;el('preview-view').style.display=open?'flex':'none';renderEditor();if(open)renderPreview();Object.assign(importStats,{reset:0,editor:0,preview:0,matching:0,mc:0,forms:[]});},
    place:(target,token)=>assignTokenToSlot(currentExam.questions[previewIndex],target,token),
    prototypeKeys:()=>Object.getOwnPropertyNames(Object.prototype).join('|')
  };
`;
async function actualImportHarness(reader) {
  const nodes = new Map(), getNode = id => { if (!nodes.has(id)) nodes.set(id, new ImportDomNode(id)); return nodes.get(id); };
  getNode('preview-view').style.display = 'none';
  const globals = { window: { addEventListener() {} }, document: { readyState: 'complete', documentElement: { dataset: {} }, getElementById: getNode, createElement: () => new ImportDomNode(), addEventListener() {} },
    localStorage: { getItem() { return null; }, setItem() {} }, setTimeout() {}, confirm: () => true,
    TextEncoder, TextDecoder, Uint8Array, DataView, ArrayBuffer, Blob, Response, DecompressionStream,
    crypto: (await import('node:crypto')).webcrypto, URL, location: { origin:'http://studio.test' } };
  const context = vm.createContext(globals);
  vm.runInContext(zipSource, context); context.NstuZip = globals.window.NstuZip;
  if (reader) context.NstuZip = { ...context.NstuZip, Reader: reader };
  const marker = studioSource.indexOf('  // Start application on DOM ready');
  assert(marker > 0);
  vm.runInContext(studioSource.slice(0,marker) + importInstrumentation + studioSource.slice(marker), context);
  const old = { id:'old-draft', title:'Stored title', durationSeconds:600, extra:{ keep:'inert' }, questions:[
    {id:'old-match',type:'reading',prompt:'Old matching',isMatching:true,options:['Old A','Old B'],matchingTargets:['A','B']},
    {id:'old-short',type:'short_answer',prompt:'Old short'}] };
  const answers = {'old-match':{0:0},'old-short':'old answer'};
  return { probe:globals.window.importProbe, node:getNode, zip:globals.window.NstuZip,
    seed(open) { globals.window.importProbe.seed(old,answers,open); getNode('exam-title').value='Raw unsynced title'; getNode('exam-id').value='raw-unsynced-id'; getNode('exam-duration').value='17'; getNode('exam-allowed-origins').value='raw.example'; getNode('toast-container').appendChild(new ImportDomNode('prior-feedback')); return globals.window.importProbe.inspect(); } };
}
const importFixture = () => ({ id:'new-draft', title:'New title', durationSeconds:600, questions:[{id:'new-q',type:'short_answer',prompt:'New prompt'}] });
const jsonImport = value => ({ name:'fixture.json', text:async()=>JSON.stringify(value) });
async function storedImport(h, value, literal) {
  const writer = new h.zip.Writer(); writer.add('manifest.json',literal ?? JSON.stringify(value));
  const archive=await writer.build(); return {name:'fixture.nstuexam',arrayBuffer:()=>archive.blob.arrayBuffer()};
}
async function rejectedImport(value, format, open, specialFile) {
  const h=await actualImportHarness(); const before=h.seed(open), oldQuestions=before.exam.questions, oldExtra=before.exam.extra,
    oldQuestion=before.exam.questions[0], oldOptions=oldQuestion.options, oldTargets=oldQuestion.matchingTargets, oldAssignment=before.answers['old-match'];
  const modelContent=JSON.stringify(before.exam), answerContent=JSON.stringify(before.answers);
  const nav=h.node('question-nav-list').children.slice(), raw=['exam-id','exam-title','exam-duration','exam-allowed-origins'].map(id=>h.node(id).value);
  const feedback=h.node('toast-container').children.slice(), proto=h.probe.prototypeKeys();
  const file=specialFile || (format==='json'?jsonImport(value):await storedImport(h,value));
  await h.probe.load(file); const after=h.probe.inspect();
  assert.equal(after.exam,before.exam,'failed admission preserves the model identity');
  assert.equal(after.exam.questions,oldQuestions); assert.equal(after.exam.questions[0],oldQuestion); assert.equal(after.exam.extra,oldExtra);
  assert.equal(after.exam.questions[0].options,oldOptions); assert.equal(after.exam.questions[0].matchingTargets,oldTargets);
  assert.equal(after.answers,before.answers); assert.equal(after.answers['old-match'],oldAssignment);
  assert.equal(JSON.stringify(after.exam),modelContent); assert.equal(JSON.stringify(after.answers),answerContent);
  assert.equal(after.active,1); assert.equal(after.index,0); assert.equal(after.token,1);
  assert.deepEqual(h.node('question-nav-list').children,nav); assert.deepEqual(['exam-id','exam-title','exam-duration','exam-allowed-origins'].map(id=>h.node(id).value),raw);
  assert.equal(h.probe.stats.reset,0); assert.equal(h.probe.stats.editor,0); assert.equal(h.probe.stats.preview,0);
  assert.equal(h.node('toast-container').children.length,feedback.length+1); assert.equal(h.node('toast-container').children[0],feedback[0]);
  assert.match(h.node('toast-container').children.at(-1).className,/error/);
  assert.equal(h.probe.prototypeKeys(),proto); if(open)h.probe.place(0,1); assert.equal(h.probe.prototypeKeys(),proto);
}

const invalidImportCases = [
  () => null, () => [],
  x => {x.questions={truthy:true};return x;}, x => {x.questions=[];return x;},
  x => {x.questions=[null];return x;}, x => {x.questions=[[]];return x;},
  x => {delete x.questions[0].type;return x;}, x => {x.questions[0].type='"><img>';return x;},
  x => {x.id='bad id';return x;}, x => {x.questions[0].id='bad id';return x;},
  x => {x.questions.push({...x.questions[0]});return x;},
  ...['title','subject'].flatMap(key=>[null,{},false,1,[]].map(v=>x=>{x[key]=v;return x;})),
  ...['prompt','instruction','passage','answerPlaceholder','audio'].flatMap(key=>[null,{},false,1,[]].map(v=>x=>{x.questions[0][key]=v;return x;})),
  ...['durationSeconds','points','wordLimit'].flatMap(key=>[null,{},false,'12',[]].map(v=>x=>{(key==='durationSeconds'?x:x.questions[0])[key]=v;return x;})),
  ...['options','matchingTargets'].flatMap(key=>[null,{},'text',[null],[1]].map(v=>x=>{x.questions[0][key]=v;return x;})),
  x=>{x.allowedOrigins='';return x;}, x=>{x.allowedOrigins=[false];return x;},
  x=>{x.questions[0].isMatching='yes';return x;},
  x=>{x.questions[0].isMatching=true;return x;},
  x=>{x.questions.push({id:'later',type:42});return x;},
  ...['__proto__','constructor','toString','valueOf'].flatMap(id=>[false,true].map(matching=>x=>{x.questions[0]={id,type:'reading',isMatching:matching,options:['A','B'],matchingTargets:['T']};return x;}))
];
let rejectedCases=0;
for(const mutate of invalidImportCases)for(const format of ['json','zip'])for(const open of [false,true]) {
  await rejectedImport(mutate(importFixture()),format,open);rejectedCases++;
}
for(const format of ['json','zip'])for(const open of [false,true]) {
  const file=format==='json'?{name:'bad.json',text:async()=>'{bad'}:await storedImport(await actualImportHarness(),null,'{bad');
  await rejectedImport(null,format,open,file);rejectedCases++;
  await rejectedImport(null,format,open,{name:format==='json'?'read.json':'read.zip',text:async()=>{throw new Error('read failure');},arrayBuffer:async()=>{throw new Error('read failure');}});rejectedCases++;
}
for(const open of [false,true]) {
  const h=await actualImportHarness(), writer=new h.zip.Writer();writer.add('unrelated.txt','no manifest');const archive=await writer.build();
  await rejectedImport(null,'zip',open,{name:'missing.zip',arrayBuffer:()=>archive.blob.arrayBuffer()});rejectedCases++;
  await rejectedImport(null,'json',open,{name:'infinite.json',text:async()=>'{"id":"finite-test","durationSeconds":1e400,"questions":[{"id":"q","type":"essay"}]}'});rejectedCases++;
}
let acceptedCases=0;
for(const type of ['multiple_choice','short_answer','essay','listening','reading'])for(const format of ['json','zip'])for(const open of [false,true]) {
  const h=await actualImportHarness();h.seed(open);const x=importFixture();x.questions[0].type=type;x.questions[0].options=['A','B'];
  const file=format==='json'?jsonImport(x):await storedImport(h,x);await h.probe.load(file);const state=h.probe.inspect();
  assert.equal(state.exam.id,x.id);assert.equal(state.active,0);assert.equal(state.index,0);assert.equal(state.token,null);
  assert.equal(h.probe.stats.reset,1);assert.equal(h.probe.stats.editor,1);assert.equal(h.probe.stats.preview,open?1:0);
  assert.equal(h.probe.stats.forms[0],type);assert.equal(h.node('question-nav-list').children.length,1);
  assert.equal(h.node('q-prompt').value,'New prompt');
  if(['multiple_choice','listening','reading'].includes(type)) {
    assert.equal(h.node('mc-option-list').children.length,2);assert.match(h.node('mc-option-list').children[0].innerHTML,/value="A"/);
    if(open){assert.equal(h.probe.stats.mc,1);assert.equal(h.node('p-mc-surface').children.length,2);assert.match(h.node('p-mc-surface').children[1].innerHTML,/B/);}
  }
  if(open){assert.equal(h.node('p-question-prompt').textContent,'New prompt');assert.equal(h.node('p-question-badge').textContent,type.toUpperCase());}
  acceptedCases++;
}
for(const format of ['json','zip'])for(const open of [false,true]) {
  const h=await actualImportHarness();h.seed(open);const x={id:'empty-safe',questions:[{id:'match-safe',type:'reading',isMatching:true,options:[]}]};
  await h.probe.load(format==='json'?jsonImport(x):await storedImport(h,x));const state=h.probe.inspect();assert.equal(state.exam.id,x.id);assert.equal(h.probe.stats.reset,1);
  assert.equal(state.exam.questions[0].matchingTargets.length,0);if(open){assert.equal(Object.keys(state.answers).length,1);assert.equal(Object.keys(state.answers['match-safe']).length,0);}
  acceptedCases++;
}
for(const points of [0,101,1000,1.25])for(const limit of [1,9,2001,5000,9.5]) {
  const h=await actualImportHarness();h.seed(true);const x=importFixture();x.durationSeconds=600.25;x.questions[0].type='essay';x.questions[0].points=points;x.questions[0].wordLimit=limit;x.questions[0].prompt='ế\u0301𝄞';x.unknown={keep:'untouched'};
  await h.probe.load(jsonImport(x));const q=h.probe.inspect().exam.questions[0];assert.equal(q.points,points);assert.equal(q.wordLimit,limit);assert.equal(q.prompt,x.questions[0].prompt);assert.equal(h.probe.inspect().exam.durationSeconds,600.25);assert.equal(h.probe.inspect().exam.unknown.keep,'untouched');acceptedCases++;
}
async function acceptedImport(value,format,open) {
  const h=await actualImportHarness();const before=h.seed(open);
  await h.probe.load(format==='json'?jsonImport(value):await storedImport(h,value));const after=h.probe.inspect();
  assert.notEqual(after.exam,before.exam);assert.equal(after.exam.id,value.id);assert.equal(h.probe.stats.reset,1);assert.equal(h.probe.stats.editor,1);assert.equal(h.probe.stats.preview,open?1:0);
  assert.equal(h.node('question-nav-list').children.length,value.questions.length);
  return {h,after};
}
const textImportBounds=[['title',240,true],['subject',120,true],['prompt',8000,false],['instruction',4000,false],['passage',30000,false],['answerPlaceholder',240,false],['audio',8000,false]];
for(const [key,maximum,top] of textImportBounds)for(const format of ['json','zip'])for(const open of [false,true]) {
  const x=importFixture(), text='𝄞'.repeat(maximum);(top?x:x.questions[0])[key]=text;
  const {after}=await acceptedImport(x,format,open);assert.equal((top?after.exam:after.exam.questions[0])[key],text);acceptedCases++;
  (top?x:x.questions[0])[key]+='𝄞';await rejectedImport(x,format,open);rejectedCases++;
}
for(const format of ['json','zip'])for(const open of [false,true]) {
  const x=importFixture();x.id='a'.repeat(96);x.questions[0].id='b'.repeat(96);await acceptedImport(x,format,open);acceptedCases++;
  x.id+='a';await rejectedImport(x,format,open);rejectedCases++;
  x.id='valid';x.questions[0].id+='b';await rejectedImport(x,format,open);rejectedCases++;
  const many=importFixture();many.questions=Array.from({length:500},(_,index)=>({id:'q'+index,type:'short_answer'}));await acceptedImport(many,format,open);acceptedCases++;
  many.questions.push({id:'over',type:'short_answer'});await rejectedImport(many,format,open);rejectedCases++;
  const origins=importFixture();origins.allowedOrigins=Array.from({length:32},()=> '𝄞'.repeat(253));await acceptedImport(origins,format,open);acceptedCases++;
  origins.allowedOrigins.push('');await rejectedImport(origins,format,open);rejectedCases++;
  origins.allowedOrigins=['𝄞'.repeat(254)];await rejectedImport(origins,format,open);rejectedCases++;
  const matching=importFixture();Object.assign(matching.questions[0],{type:'reading',isMatching:true,options:Array(500).fill('𝄞'.repeat(2000)),matchingTargets:Array(500).fill('𝄞'.repeat(2000))});
  const {h,after}=await acceptedImport(matching,format,open);assert.equal(after.exam.questions[0].options.length,500);assert.equal(after.exam.questions[0].matchingTargets.length,500);if(open){h.probe.place(0,0);assert.equal(after.answers['new-q'][0],0);}acceptedCases++;
  matching.questions[0].options.push('');await rejectedImport(matching,format,open);rejectedCases++;
  matching.questions[0].options=['𝄞'.repeat(2001)];await rejectedImport(matching,format,open);rejectedCases++;
  matching.questions[0].options=['','A'];matching.questions[0].matchingTargets.push('');await rejectedImport(matching,format,open);rejectedCases++;
  matching.questions[0].matchingTargets=['𝄞'.repeat(2001)];await rejectedImport(matching,format,open);rejectedCases++;
  matching.questions[0].matchingTargets=[''];matching.questions[0].prompt='';matching.title='';matching.subject='';matching.allowedOrigins=[''];
  const empty=await acceptedImport(matching,format,open);assert.equal(empty.after.exam.questions[0].options[0],'');acceptedCases++;
  const inert=importFixture();inert.id='__proto__';inert.candidate={ignored:'inert'};inert.documents=[null];inert.questions[0].pdf={ignored:'inert'};inert.questions[0].authoringExtension={keep:'literal'};
  const retained=await acceptedImport(inert,format,open);assert.equal(JSON.stringify(retained.after.exam),JSON.stringify(inert));acceptedCases++;
}
console.log(`Studio actual import/render admission: ${rejectedCases} negative and ${acceptedCases} positive cases passed`);
