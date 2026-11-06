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
