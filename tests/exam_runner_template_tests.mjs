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
const canonicalRunner=context.window.NSTU_RUNNER_TEMPLATE;

// Fault-inject the actual private reader helper without exporting production APIs.
const studioSource = await readFile(new URL('exam/studio/studio.js', root), 'utf8');
const dialogStart=studioSource.indexOf('  let exportDialogOpener =');
const dialogEnd=studioSource.indexOf('  function syncExamMetadata() {');
assert.ok(dialogStart>=0&&dialogEnd>dialogStart);
const dialogSource=studioSource.slice(dialogStart,dialogEnd);
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
  ${dialogSource}
  ${studioSource.slice(initStart, initEnd)}
  init();
  return { exam: currentExam, setIndex(index) { activeQuestionIndex = index; },
    manifest() { return compileManifest(currentExam); } };
})()`, { getNode: fieldNode, document: { documentElement: { dataset: {} }, listeners:{}, addEventListener(type,callback){(this.listeners[type] ||= []).push(callback);} },
  window:{listeners:{},addEventListener(type,callback){(this.listeners[type] ||= []).push(callback);}},
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
assert.equal(fields.manifest().questions[0].answerPlaceholder, '', 'Export preserves explicitly empty placeholder');
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
const syncEnd = studioSource.indexOf('  async function exportPackage(', syncStart);
const originsStart = studioSource.indexOf('  function parseOriginsInput(raw) {');
const originsEnd = studioSource.indexOf('  function validateOrigin(', originsStart);
assert.ok([renderStart, renderEnd, addStart, addEnd, syncStart, syncEnd, originsStart, originsEnd].every(index => index >= 0));
const transitionNodes = new Map();
function transitionNode(id = '') {
  const node = { value: '', children: [], listeners: {}, textContent: '',
    setAttribute(name,value) { this[name]=String(value); },
    contains(target) { return target === this || this.children.some(child => child.contains?.(target)); },
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
  ${dialogSource}
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

const nativeRailMarkup=transitionNodes.get('question-nav-list').children[0].innerHTML;
assert.match(nativeRailMarkup,/<button[^>]*type="button"[^>]*aria-current="true"/,
  'Current question selection must be a native button, keyboard reachable without a custom key dispatcher');
assert.equal((nativeRailMarkup.match(/<button\b/g)||[]).length,2,'Selection and deletion are separate native controls');
assert.ok(nativeRailMarkup.indexOf('</button>')<nativeRailMarkup.indexOf('data-delete='),'Delete must be outside selection button');

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
  ${dialogSource}
  ${studioSource.slice(initStart, initEnd)}
  init();
  return { importFile: handleImportFile,
    seedPreview() { previewIndex = 3; previewAnswers = { shared: 'Old answer' }; selectedTokenForPlacement = 2; activeQuestionIndex = 1; },
    inspect() { return { currentExam, previewIndex, previewAnswers, selectedTokenForPlacement, activeQuestionIndex, renderCount, previewRenders }; },
    sample() { el('btn-load-sample').listeners.click[0](); } };
})()`, { getNode: replacementNode, TextDecoder, document: { documentElement: { dataset: {} }, listeners:{}, addEventListener(type,callback){(this.listeners[type] ||= []).push(callback);} },
  window:{listeners:{},addEventListener(type,callback){(this.listeners[type] ||= []).push(callback);}},
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

class ImportDomNode {
  constructor(id = '') { this.id = id; this.children = []; this.style = {}; this.dataset = {}; this.listeners = {}; this.className = ''; this._html = ''; this._value = ''; this.textContent = ''; this.queries = new Map(); }
  get value() { return this._value; }
  set value(value) { this._value = String(value ?? ''); }
  get innerHTML() { return this._html; }
  set innerHTML(value) { this._html = String(value); for(const child of this.children)child.parentNode=null; this.children = []; this.queries.clear(); }
  get classList() { return { add: name => { this.className += ' ' + name; }, remove: name => { this.className = this.className.split(' ').filter(x => x !== name).join(' '); }, contains: name => this.className.split(' ').includes(name), toggle: (name,force) => { const present=this.className.split(' ').includes(name),next=force===undefined?!present:!!force;if(next&&!present)this.className+=' '+name;if(!next)this.className=this.className.split(' ').filter(x=>x!==name).join(' ');return next; } }; }
  appendChild(child) { this.children.push(child); child.parentNode = this; return child; }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  prepend(child) { this.children.unshift(child); child.parentNode = this; }
  querySelector(selector) {
    const find=node=>{for(const child of node.children){if(selector.startsWith('.')&&child.className.split(/\s+/).includes(selector.slice(1)))return child;const nested=find(child);if(nested)return nested;}return null;};
    const actual=find(this);if(actual)return actual;
    if (!this.queries.has(selector)) this.queries.set(selector, new ImportDomNode(selector)); return this.queries.get(selector);
  }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  setAttribute(name, value) { this[name] = String(value); }
  getBoundingClientRect() { return { left: 0, width: 1000 }; }
  remove() { if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(child => child !== this); this.parentNode = null; }
  click() { (this.listeners.click || []).forEach(listener => listener({ target: this, currentTarget: this, preventDefault() {}, stopPropagation() {} })); }
  get isConnected() { return this === this.ownerDocument?.body || !!this.parentNode?.isConnected; }
  getClientRects() { for(let n=this;n;n=n.parentNode)if(n.hidden||n.style.display==='none')return [];return [{width:100,height:30}]; }
  closest() { for(let n=this;n;n=n.parentNode)if(n.hidden||n.inert)return n;return null; }
  matches(selector) { assert.equal(selector, ':disabled');for(let n=this;n;n=n.parentNode)if(n.disabled)return true;return false; }
  contains(node) { for(let n=node;n;n=n.parentNode)if(n===this)return true;return false; }
  querySelectorAll(selector) { assert.equal(selector,'button');const result=[];const walk=n=>{for(const c of n.children){if(c.tagName==='BUTTON')result.push(c);walk(c);}};walk(this);return result; }
  focus() { this.focusCalls=(this.focusCalls||0)+1;if(this.isConnected&&!this.closest()&&!this.matches(':disabled')&&this.getClientRects().length&&this.style.visibility!=='hidden'&&this.style.visibility!=='collapse'&&this.tabIndex>=-1){const previous=this.ownerDocument.activeElement;this.ownerDocument.activeElement=this;if(previous!==this)for(const callback of this.listeners.focus||[])callback({target:this,currentTarget:this});} }
  scrollIntoView(options) { (this.scrolls ||= []).push(options); }
}


const exportStart = studioSource.indexOf('  let exportGeneration =') >= 0
  ? studioSource.indexOf('  let exportGeneration =') : studioSource.indexOf('  async function exportPackage(');
const exportEnd = studioSource.indexOf('  async function handleImportFile(', exportStart);
assert.ok(exportStart >= 0 && exportEnd > exportStart);
function exportLifecycleHarness() {
  const nodes = new Map(), pending = [], downloads = [];
  const doc={documentElement:{},body:new ImportDomNode('body')};doc.body.ownerDocument=doc;doc.activeElement=doc.body;
  const node = id => {
    if (!nodes.has(id)) {const value=new ImportDomNode(id);value.ownerDocument=doc;value.tabIndex=0;doc.body.appendChild(value);nodes.set(id,value);}
    return nodes.get(id);
  };
  for(const id of ['btn-close-export','btn-cancel-export','btn-download-nstuexam']){const value=node(id);value.remove();value.tagName='BUTTON';node('export-modal').appendChild(value);}
  const model = { id: 'old-export', title: 'Old content', durationSeconds:600,
    questions: [{id:'q1',type:'short_answer',prompt:'Original question',points:0}] };
  const exporter = vm.runInNewContext(`(() => {
    let currentExam = model;
    const el = getNode, syncExamMetadata = () => {},
      verifiedBrandPng = async bytes => bytes, showToast = () => {}, escapeHtml = value => value;
    ${studioSource.slice(studioSource.indexOf('  function validateOrigin('),compileEnd)}
    ${dialogSource}
    ${studioSource.slice(exportStart, exportEnd)}
    return { exportPackage };
  })()`, { model, getNode: node, Uint8Array, TextEncoder, window: { NSTU_RUNNER_TEMPLATE: {
    indexHtml: 'HTML', stylesCss: 'CSS', appJs: 'JS', brandPngBase64: 'A'.repeat(228760) } },
    atob: () => 'PNG', btoa: () => 'A'.repeat(228760), NstuZip: { Writer: class {
      constructor() { this.entries = new Map(); }
      add(name, bytes) { this.entries.set(name, bytes); }
      build() { return new Promise((resolve, reject) => pending.push({ resolve, reject,
        manifest: JSON.parse(this.entries.get('manifest.json')) })); }
    } }, URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} },
    getComputedStyle:value=>({visibility:value.style.visibility||'visible'}),
    document: Object.assign(doc,{createElement: () => ({ click() { downloads.push(this.download); } })}) });
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

// A lost schema value or admitted native-incompatible manifest must fail these literal contracts.
const nativeContracts=vm.runInNewContext(`(() => {
  ${studioSource.slice(studioSource.indexOf('  function validateOrigin('),compileEnd)}
  return {compile:compileManifest,guard:validateExportContract,origin:validateOrigin};
})()`,{TextEncoder});
const nativeFixture=()=>({id:'native-v1',title:'Native contract',subject:'',durationSeconds:601,questions:[
  {id:'q1',type:'short_answer',prompt:'Prompt',points:0}]});
const projected=nativeFixture();Object.assign(projected,{subject:'',candidate:'',allowedOrigins:[],documents:[]});
Object.assign(projected.questions[0],{instruction:'',passage:'',answerPlaceholder:'',wordLimit:1});
assert.deepEqual(JSON.parse(JSON.stringify(nativeContracts.compile(projected))),{
  id:'native-v1',title:'Native contract',subject:'',candidate:'',durationSeconds:601,allowedOrigins:[],documents:[],
  questions:[{id:'q1',type:'short_answer',prompt:'Prompt',points:0,instruction:'',passage:'',answerPlaceholder:'',wordLimit:1}]});
const absent=nativeFixture();delete absent.subject;delete absent.questions[0].points;
assert.equal(nativeContracts.compile(absent).subject,'General');assert.equal(nativeContracts.compile(absent).questions[0].points,1);
const mediaProjection=nativeFixture();Object.assign(mediaProjection.questions[0],{pdf:'',audio:''});
assert.equal(nativeContracts.compile(mediaProjection).questions[0].pdf,'');assert.equal(nativeContracts.compile(mediaProjection).questions[0].audio,'');
let exportContractCases=4;
function admitNative(model,accepted,label){const before=JSON.stringify(model);const errors=nativeContracts.guard(model);
  assert.equal(errors.length===0,accepted,label+': '+errors.join('; '));assert.equal(JSON.stringify(model),before,'admission is read-only');exportContractCases++;}
for(const type of ['multiple_choice','short_answer','essay','listening','reading']){
  const x=nativeFixture();x.questions[0].type=type;x.questions[0].options=['A','B'];admitNative(x,true,type);
}
for(const [key,min,max] of [['points',0,1000],['wordLimit',1,5000]])for(const value of [min,max,min-1,max+1,1.5,'1',null]){
  const x=nativeFixture();x.questions[0][key]=value;admitNative(x,value===min||value===max,key+':'+value);
}
for(const value of [60,601,86400,59,86401,600.25,'600',null]){const x=nativeFixture();x.durationSeconds=value;admitNative(x,Number.isInteger(value)&&value>=60&&value<=86400,'duration');}
for(const mutate of [x=>x.questions[0].isMatching=true,x=>x.questions[0].type='matching',x=>x.questions.push({...x.questions[0]}),
  x=>x.questions=null,x=>x.questions[0]=null,x=>x.questions[0].options=['A'],x=>x.questions[0].options=['','B'],
  x=>x.questions[0].options=Array(13).fill('A'),x=>x.subject=null,x=>x.candidate=7,x=>x.documents=[{id:'doc',url:'file.pdf'}],
  x=>x.allowedOrigins=['exam.test','exam.test'],x=>x.allowedOrigins=null]){
  const x=nativeFixture();mutate(x);admitNative(x,false,'shape/media/type');
}
for(const key of ['pdf','audio'])for(const value of ['',null,'media/file']){const x=nativeFixture();x.questions[0][key]=value;admitNative(x,false,'declared media');}
for(const [field,accepted,rejected] of [['prompt','ế'.repeat(2666)+'ab','ế'.repeat(2667)],
  ['passage','ế'.repeat(21845)+'a','ế'.repeat(21845)+'ab']]){
  for(const [text,ok] of [[accepted,true],[rejected,false]]){const x=nativeFixture();x.questions[0][field]=text;admitNative(x,ok,field+' UTF8 byte boundary');}
}
for(const type of ['short_answer','essay','reading'])for(const text of ['ế'.repeat(666)+'ab','ế'.repeat(667),'']){
  const x=nativeFixture();x.questions[0].type=type;x.questions[0].options=[text,'B'];admitNative(x,text.length>0&&Buffer.byteLength(text)<=2000,'all supplied option bytes');
}
for(const field of ['title','subject','candidate','prompt','instruction','passage','answerPlaceholder'])for(const bad of ['\ud800','\udc00']){
  const x=nativeFixture();(field==='title'||field==='subject'||field==='candidate'?x:x.questions[0])[field]=bad;admitNative(x,false,'lone surrogate:'+field);
}
for(const count of [256,257,500]){const x=nativeFixture();x.questions=Array.from({length:count},(_,i)=>({id:'q'+i,type:'short_answer',prompt:'P'}));admitNative(x,count===256,'native array boundary');}
for(const [origin,ok] of [['//exam.test',true],['HTTPS://Exam.test:65535/path?x=1',true],['exam.test:00001/#x',true],
  ['https://exam.test/đề',false],['https://exam.test/path name',false],['exam.test:0',false],['exam.test:65536',false],
  ['exam.test:000001',false],['https://exam.test/@x',false],['-exam.test',false],['a..test',false],['ftp://exam.test',false]]){
  assert.equal(nativeContracts.origin(origin),ok,origin);exportContractCases++;
}
console.log(`Studio literal projection/native shape/UTF8/origin admission: ${exportContractCases} cases passed`);

// Run the whole actual controller and renderers; only DOM/platform I/O are boundaries.

const zipSource = await readFile(new URL('exam/studio/zip.js', root), 'utf8');
const importInstrumentation = `
  const importStats = { reset:0, editor:0, preview:0, matching:0, mc:0, compile:0, forms:[] };
  const originalImportReset=resetPreviewState, originalImportEditor=renderEditor,
    originalImportPreview=renderPreview, originalImportMatching=renderPreviewMatching,
    originalImportMc=renderPreviewMc, originalImportForm=renderActiveQuestionForm, originalImportCompiler=compileManifest;
  resetPreviewState=()=>{importStats.reset++;return originalImportReset();};
  renderEditor=()=>{importStats.editor++;return originalImportEditor();};
  renderPreview=()=>{importStats.preview++;return originalImportPreview();};
  renderPreviewMatching=q=>{importStats.matching++;return originalImportMatching(q);};
  renderPreviewMc=q=>{importStats.mc++;return originalImportMc(q);};
  renderActiveQuestionForm=()=>{importStats.forms.push(currentExam.questions[activeQuestionIndex]?.type);return originalImportForm();};
  compileManifest=exam=>{importStats.compile++;return originalImportCompiler(exam);};
  window.importProbe={
    load:handleImportFile, save:saveDraft, sync:syncExamMetadata, stats:importStats,
    export:exportPackage,
    inspect:()=>({exam:currentExam,active:activeQuestionIndex,index:previewIndex,answers:previewAnswers,token:selectedTokenForPlacement}),
    seed:(exam,answers,open)=>{currentExam=exam;activeQuestionIndex=1;previewIndex=0;previewAnswers=answers;selectedTokenForPlacement=1;el('preview-view').style.display=open?'flex':'none';renderEditor();if(open)renderPreview();Object.assign(importStats,{reset:0,editor:0,preview:0,matching:0,mc:0,forms:[]});},
    place:(target,token)=>assignTokenToSlot(currentExam.questions[previewIndex],target,token),
    prototypeKeys:()=>Object.getOwnPropertyNames(Object.prototype).join('|')
  };
`;
async function actualImportHarness(reader) {
  const nodes = new Map();
  const doc={readyState:'complete',documentElement:{dataset:{}},listeners:{},addEventListener(type,callback){(this.listeners[type] ||= []).push(callback);}};
  const win={NSTU_RUNNER_TEMPLATE:canonicalRunner,listeners:{},frames:new Map(),cancelled:[],nextFrame:0,
    addEventListener(type,callback){(this.listeners[type] ||= []).push(callback);},
    requestAnimationFrame(callback){const id=this.nextFrame++;this.frames.set(id,callback);return id;},
    cancelAnimationFrame(id){this.cancelled.push(id);this.frames.delete(id);}};
  doc.body=new ImportDomNode('body');doc.body.ownerDocument=doc;doc.activeElement=doc.body;
  const getNode = id => { if (!nodes.has(id)){const node=new ImportDomNode(id);node.ownerDocument=doc;node.tabIndex=0;doc.body.appendChild(node);nodes.set(id,node);}return nodes.get(id); };
  // Model the real initial modal attributes and its existing button order.
  const html=await readFile(new URL('exam/studio/index.html',root),'utf8');
  const modalTag=html.match(/<div[^>]*id="export-modal"[^>]*>/)[0];
  const modal=getNode('export-modal');modal.inert=/\binert\b/.test(modalTag);modal.tabIndex=/tabindex="-1"/.test(modalTag)?-1:0;
  for(const id of ['btn-close-export','btn-cancel-export','btn-download-nstuexam']){const node=getNode(id);node.remove();node.tagName='BUTTON';modal.appendChild(node);}
  getNode('btn-download-nstuexam').disabled=true;
  const downloads = [], blobs = new Map();
  class DownloadURL extends URL { static createObjectURL(blob) { const url='blob:test-'+blobs.size;blobs.set(url,blob);return url; } static revokeObjectURL() {} }
  getNode('preview-view').style.display = 'none';
  doc.getElementById=getNode;doc.createElement=tag=>{const node=new ImportDomNode();node.tagName=tag.toUpperCase();node.tabIndex=['button','input'].includes(tag)?0:-1;node.ownerDocument=doc;if(tag==='a')node.click=()=>downloads.push({filename:node.download,blob:blobs.get(node.href)});return node;};
  const globals = { window:win, document: doc, atob, btoa,
    getComputedStyle:node=>({visibility:node.style.visibility||'visible',display:node.style.display||'block'}),
    localStorage: { getItem() { return null; }, setItem() {} }, setTimeout() {}, confirm: () => true,
    TextEncoder, TextDecoder, Uint8Array, DataView, ArrayBuffer, Blob, Response, DecompressionStream,
    crypto: (await import('node:crypto')).webcrypto, URL:DownloadURL, location: { origin:'http://studio.test' } };
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
  return { probe:globals.window.importProbe, node:getNode, zip:globals.window.NstuZip, downloads, document:doc,
    window:win,resize(){for(const callback of win.listeners.resize||[])callback({type:'resize'});},
    flushFrames(){const callbacks=[...win.frames.values()];win.frames.clear();callbacks.forEach(callback=>callback());},
    key(key,options={}){const event={key,shiftKey:false,ctrlKey:false,altKey:false,metaKey:false,prevented:0,preventDefault(){this.prevented++;},...options};for(const callback of doc.listeners.keydown||[])callback(event);return event;},
    seed(open) { globals.window.importProbe.seed(old,answers,open); getNode('exam-title').value='Raw unsynced title'; getNode('exam-id').value='raw-unsynced-id'; getNode('exam-duration').value='17'; getNode('exam-allowed-origins').value='raw.example'; getNode('toast-container').appendChild(new ImportDomNode('prior-feedback')); return globals.window.importProbe.inspect(); } };
}
const importFixture = () => ({ id:'new-draft', title:'New title', durationSeconds:600, questions:[{id:'new-q',type:'short_answer',prompt:'New prompt'}] });

// Exercise the real init-installed resize listener and frame-time focus guards.
async function resizeHarness(){const h=await actualImportHarness();h.seed(false);const editor=h.node('editor-view');editor.style.display='grid';const field=h.node('q-points');field.remove();editor.appendChild(field);field.value='12.';field.selectionStart=1;field.selectionEnd=2;field.focus();return{h,editor,field};}
let resizeCases=0;
{
  const {h,field}=await resizeHarness();assert.equal(h.window.listeners.resize?.filter(callback=>callback.name==='queueAuthoringFocusReveal').length,1,'one actual authoring resize listener is installed');
  assert.equal(h.window.listeners.resize?.filter(callback=>callback.name==='queueMatchingFocusReveal').length,1,'one matching resize listener is installed');
  assert.equal(h.window.listeners.resize?.filter(callback=>callback.name==='queueChoiceFocusReveal').length,1,'one choice resize listener is installed');
  assert.equal(h.window.listeners.resize?.filter(callback=>callback.name==='queuePreviewTextFocusReveal').length,1,'one text Preview resize listener is installed');assert.equal(h.window.listeners.resize.length,4,'no duplicate resize registrations');
  const before=h.probe.inspect(),model=JSON.stringify(before.exam),answers=before.answers,stats=JSON.stringify(h.probe.stats),focusCalls=field.focusCalls;
  h.resize();h.resize();assert.equal(h.window.frames.size,1);assert.deepEqual(h.window.cancelled,[0],'opaque RAF id0 must be cancelled');h.flushFrames();
  assert.equal(field.scrolls.length,1);assert.equal(field.scrolls[0].block,'nearest');assert.equal(field.scrolls[0].inline,'nearest');assert.equal(field.scrolls[0].behavior,'instant');
  assert.equal(field.focusCalls,focusCalls,'reveal never refocuses');assert.equal(h.document.activeElement,field);assert.equal(field.value,'12.');assert.equal(field.selectionStart,1);assert.equal(field.selectionEnd,2);
  assert.equal(JSON.stringify(h.probe.inspect().exam),model);assert.equal(h.probe.inspect().answers,answers);assert.equal(JSON.stringify(h.probe.stats),stats);
  h.resize();assert.deepEqual(h.window.cancelled,[0],'executed frame ownership was cleared');h.flushFrames();assert.equal(field.scrolls.length,2);resizeCases++;
}
for(const invalidate of [({field})=>field.hidden=true,({field})=>field.inert=true,({field})=>field.disabled=true,({field})=>field.remove(),({field})=>field.ownerDocument={},({field})=>field.style.visibility='hidden',({editor})=>editor.style.display='none',({h})=>h.node('export-modal').classList.add('open'),({h})=>h.node('btn-save-draft').focus()]){
  const context=await resizeHarness();context.h.resize();invalidate(context);const active=context.h.document.activeElement,model=JSON.stringify(context.h.probe.inspect().exam);context.h.flushFrames();assert.equal(context.field.scrolls?.length||0,0);assert.equal(context.h.document.activeElement,active);assert.equal(JSON.stringify(context.h.probe.inspect().exam),model);resizeCases++;
}
{
  const {h,editor,field}=await resizeHarness();h.resize();const next=h.node('q-sa-placeholder');next.remove();editor.appendChild(next);next.focus();h.flushFrames();assert.equal(field.scrolls?.length||0,0);assert.equal(next.scrolls.length,1);assert.equal(h.document.activeElement,next,'frame resolves current active control');resizeCases++;
}
{
  const {h,field}=await resizeHarness();h.resize();h.node('btn-mode-preview').click();const state=h.probe.inspect(),active=h.document.activeElement;h.flushFrames();assert.equal(field.scrolls?.length||0,0);assert.equal(h.probe.inspect().exam,state.exam);assert.equal(h.document.activeElement,active);resizeCases++;
}
{
  const {h,field}=await resizeHarness();h.resize();await h.probe.export();const state=h.probe.inspect(),active=h.document.activeElement,status=h.node('export-validation-status').innerHTML;h.flushFrames();assert.equal(field.scrolls?.length||0,0);assert.equal(h.probe.inspect().exam,state.exam);assert.equal(h.document.activeElement,active);assert.equal(h.node('export-validation-status').innerHTML,status);resizeCases++;
}
{
  const {h,field}=await resizeHarness();h.resize();await h.probe.load({name:'new.json',text:async()=>JSON.stringify({id:'resize-new',title:'New',durationSeconds:600,questions:[{id:'new',type:'essay',prompt:'New',points:7,wordLimit:300}]})});const state=h.probe.inspect(),raw=field.value;h.flushFrames();assert.equal(field.scrolls.length,1);assert.equal(field.value,raw);assert.equal(h.probe.inspect().exam,state.exam);assert.equal(h.probe.inspect().exam.questions[0].points,7);resizeCases++;
}
console.log(`Studio actual authoring resize reveal lifecycle/guards: ${resizeCases} cases passed`);

// A numeric renderer coercion, lost raw/rejection draft, or missing Export guard must fail these actual-controller tests.
const numericFixture=()=>({id:'numeric-draft',title:'Numeric draft',subject:'',durationSeconds:600,questions:[
  {id:'essay-a',type:'essay',prompt:'A',points:4,wordLimit:300},{id:'essay-b',type:'essay',prompt:'B',points:5,wordLimit:400}]});
async function numericHarness(model=numericFixture()) { const h=await actualImportHarness();await h.probe.load({name:'numeric.json',text:async()=>JSON.stringify(model)});return h; }
function numericInput(h,id,raw,valid=true,badInput=false){const node=h.node(id);node.value=raw;node.valueAsNumber=Number(raw);node.validity={valid,badInput};for(const callback of node.listeners.input||[])callback({target:node,currentTarget:node});}
function numericNavigate(h){h.node('question-nav-list').children[1].click();h.node('question-nav-list').children[0].click();}
let numericCases=0;
for(const id of ['exam-duration','q-points','q-essay-limit']){
  const h=await numericHarness(),before=JSON.stringify(h.probe.inspect().exam);numericInput(h,id,'1e');numericNavigate(h);
  assert.equal(h.node(id).value,'1e','navigation retains incomplete numeric text');assert.equal(JSON.stringify(h.probe.inspect().exam),before,'invalid raw text preserves prior model');
  assert.equal(h.node(id)['aria-invalid'],'true');assert.equal(h.node(id+'-error').hidden,false);assert.match(h.node(id+'-error').textContent,id==='exam-duration'?/whole seconds/:/whole number/);
  await h.probe.export();assert.equal(h.probe.stats.compile,0);assert.equal(h.node('btn-download-nstuexam').disabled,true);numericCases++;
}
for(const [id,key,scale,values] of [['exam-duration','durationSeconds',60,['1','1440',' 10 ','001','1.5']],['q-points','points',1,['0','1000',' 101 ','001']],['q-essay-limit','wordLimit',1,['1','5000',' 2001 ','0010']]]){
  for(const raw of values){const h=await numericHarness();numericInput(h,id,raw);numericNavigate(h);assert.equal(h.node(id).value,raw);assert.equal((id==='exam-duration'?h.probe.inspect().exam:h.probe.inspect().exam.questions[0])[key],Number(raw)*scale);assert.equal(h.node(id)['aria-invalid'],'false');assert.equal(h.node(id+'-error').hidden,true);numericCases++;}
}
for(const [id,invalid] of [['exam-duration',['','0','1441','1.501','-1','+1','1e2','garbage','   ']],['q-points',['','-1','1001','1.5','-2','+1','1e2','garbage','   ']],['q-essay-limit',['0','5001','21.5','-10','+10','1e2','garbage','   ']]])for(const raw of invalid){
  const h=await numericHarness(),before=JSON.stringify(h.probe.inspect().exam);numericInput(h,id,raw);numericNavigate(h);assert.equal(h.node(id).value,raw);assert.equal(JSON.stringify(h.probe.inspect().exam),before);assert.equal(h.node(id)['aria-invalid'],'true');numericCases++;
}
{
  const h=await numericHarness();numericInput(h,'q-essay-limit','',false,true);numericNavigate(h);
  assert.equal(h.probe.inspect().exam.questions[0].wordLimit,300);assert.equal(h.node('q-essay-limit').value,'');assert.equal(h.node('q-essay-limit')['aria-invalid'],'true');
  await h.probe.export();assert.equal(h.probe.stats.compile,0);numericInput(h,'q-essay-limit','');assert.equal(Object.hasOwn(h.probe.inspect().exam.questions[0],'wordLimit'),false);assert.equal(h.node('q-essay-limit')['aria-invalid'],'false');numericNavigate(h);assert.equal(h.node('q-essay-limit').value,'');numericCases++;
}
for(const [key,value] of [['points',-1],['wordLimit',9.5]]){
  const model=numericFixture();model.questions[1][key]=value;const h=await numericHarness(model);await h.probe.export();
  assert.equal(h.probe.stats.compile,0,'unvisited imported numeric fields block Export');assert.equal(h.probe.inspect().exam.questions[1][key],value);assert.match(h.node('export-validation-status').innerHTML,/Question #2/);numericCases++;
}
{
  const model=numericFixture();model.durationSeconds=600.25;const h=await numericHarness(model);assert.equal(h.node('exam-duration').value,String(600.25/60));
  h.probe.sync();h.node('btn-mode-preview').click();h.node('btn-mode-editor').click();h.probe.save();const saved=JSON.parse(await h.downloads[0].blob.text());
  assert.equal(saved.durationSeconds,600.25);assert.equal(h.probe.inspect().exam.durationSeconds,600.25);assert.equal(h.node('exam-duration').value,String(600.25/60));assert.equal(h.node('exam-duration')['aria-invalid'],'true');numericCases++;
}
{
  const model=numericFixture();delete model.durationSeconds;delete model.questions[0].points;delete model.questions[0].wordLimit;const h=await numericHarness(model);
  assert.equal(h.node('exam-duration').value,'60');assert.equal(h.node('q-points').value,'1');assert.equal(h.node('q-essay-limit').value,'');h.probe.sync();assert.equal(h.probe.inspect().exam.durationSeconds,3600);numericCases++;
}
{
  const model=numericFixture();model.questions[1]={id:'short',type:'short_answer',prompt:'B',points:2,wordLimit:9.5};const h=await numericHarness(model);
  h.node('question-nav-list').children[1].click();numericInput(h,'q-essay-limit','100');assert.equal(h.probe.inspect().exam.questions[1].wordLimit,9.5);await h.probe.export();assert.equal(h.probe.stats.compile,0,'inert nonessay wordLimit is retained but fails native integer contract');numericCases++;
}
{
  const h=await numericHarness();numericInput(h,'q-points','-1');h.probe.inspect().answers['essay-a']='Retained answer';const answers=h.probe.inspect().answers;
  h.node('btn-mode-preview').click();h.node('btn-mode-editor').click();h.node('btn-studio-theme').click();assert.equal(h.node('q-points').value,'-1');assert.equal(h.probe.inspect().answers,answers);
  h.probe.save();const saved=JSON.parse(await h.downloads[0].blob.text());assert.equal(saved.questions[0].points,4);assert.equal(h.node('q-points').value,'-1');
  await h.probe.load({name:'bad.json',text:async()=>'{bad'});numericNavigate(h);assert.equal(h.node('q-points').value,'-1');
  const replacement=numericFixture();replacement.questions[0].points=7;await h.probe.load({name:'new.json',text:async()=>JSON.stringify(replacement)});assert.equal(h.node('q-points').value,'7');assert.equal(h.node('q-points')['aria-invalid'],'false');
  numericInput(h,'exam-duration','1e');h.node('btn-load-sample').click();assert.equal(h.node('exam-duration').value,'60');assert.equal(h.node('exam-duration')['aria-invalid'],'false');numericCases++;
}
{
  const h=await numericHarness();numericInput(h,'q-points','-1');h.node('question-nav-list').children[0].listeners.click[0]({target:{closest:()=>true}});
  h.node('select-add-type').value='essay';h.node('btn-add-question').click();assert.equal(h.node('q-points')['aria-invalid'],'false');assert.equal(h.node('q-points').value,'8');await h.probe.export();assert.equal(h.probe.stats.compile,1,'retired drafts do not poison a newly added question');numericCases++;
}
for(const retry of [false,true]){
  const h=await numericHarness();let resolveBuild;h.zip.Writer.prototype.build=function(){return new Promise(resolve=>{resolveBuild=resolve;});};
  const first=h.probe.export();for(let n=0;n<100&&!resolveBuild;n++)await new Promise(resolve=>setTimeout(resolve,10));assert.equal(typeof resolveBuild,'function');
  numericInput(h,'q-points','-1');if(retry)await h.probe.export();const status=h.node('export-validation-status').innerHTML;
  resolveBuild({fileCount:5,totalBytes:100,archiveSha256:'captured-archive',contentSha256:'captured-content',blob:new Blob()});await first;
  if(retry){assert.equal(h.node('export-validation-status').innerHTML,status);assert.equal(h.node('btn-download-nstuexam').disabled,true);}else{assert.match(h.node('export-validation-status').innerHTML,/Successfully/);assert.equal(h.node('btn-download-nstuexam').disabled,false);assert.equal(h.node('lbl-content-hash').textContent,'captured-content');}
  assert.equal(h.downloads.length,0);numericCases++;
}
console.log(`Studio actual numeric draft retention, bounds, model and Export ownership: ${numericCases} cases passed`);

let exportControllerCases=0;
for(const seconds of [62,601,86400]){
  const x=numericFixture();x.durationSeconds=seconds;const h=await numericHarness(x);
  const raw=String(seconds/60);assert.equal(h.node('exam-duration').value,raw);
  h.probe.sync();h.node('btn-mode-preview').click();h.node('btn-mode-editor').click();h.probe.save();
  assert.equal(JSON.parse(await h.downloads[0].blob.text()).durationSeconds,seconds,'unchanged imported integer seconds stay exact');
  await h.probe.export();assert.equal(h.node('btn-download-nstuexam').disabled,false);h.node('btn-download-nstuexam').onclick();
  const entries=await new h.zip.Reader(await h.downloads[1].blob.arrayBuffer()).readEntries();const manifest=JSON.parse(new TextDecoder().decode(entries.find(e=>e.path==='manifest.json').bytes));
  assert.equal(manifest.durationSeconds,seconds);exportControllerCases++;
}
{
  const x=numericFixture();x.durationSeconds=62;const h=await numericHarness(x);numericInput(h,'exam-duration','1.0333333333333333');
  assert.equal(h.node('exam-duration')['aria-invalid'],'true');assert.equal(h.probe.inspect().exam.durationSeconds,62);
  await h.probe.export();assert.equal(h.probe.stats.compile,0,'changed text does not get imported-seed tolerance');exportControllerCases++;
}
for(const mutate of [x=>x.questions[0].isMatching=true,x=>x.questions[0].audio='',x=>x.questions[0].pdf='',
  x=>x.documents=[{id:'doc',url:'file.pdf'}],x=>x.questions=Array.from({length:257},(_,i)=>({id:'q'+i,type:'short_answer',prompt:'P',points:0}))]){
  const x=nativeFixture();mutate(x);const h=await numericHarness(x),state=h.probe.inspect(),before=JSON.stringify(state.exam),answers=state.answers;
  let constructed=0;const Writer=h.zip.Writer;h.zip.Writer=class extends Writer{constructor(){super();constructed++;}};
  await h.probe.export();assert.equal(h.probe.stats.compile,0);assert.equal(constructed,0,'rejection precedes ZIP construction');
  assert.equal(h.node('btn-download-nstuexam').disabled,true);assert.equal(h.node('btn-download-nstuexam').onclick,null);
  assert.equal(h.node('lbl-archive-hash').textContent,'-');assert.equal(h.downloads.length,0);assert.equal(h.probe.inspect().answers,answers);
  assert.equal(h.probe.inspect().exam,state.exam);assert.equal(JSON.stringify(state.exam),before);
  h.probe.save();assert.equal(JSON.stringify(JSON.parse(await h.downloads[0].blob.text())),before,'rejected data remains fully saveable');exportControllerCases++;
}
for(const byteLimit of [131072,131073]){
  // Independent literal manifest; tune only an authored ASCII passage, with no compiler-derived expectation.
  const x={id:'byte-boundary',title:'Boundary',subject:'',durationSeconds:62,documents:[],questions:Array.from({length:5},(_,i)=>({
    id:'q'+i,type:'essay',prompt:'P',points:0,instruction:'',passage:i<4?'x'.repeat(30000):''}))};
  x.questions[4].passage='x'.repeat(byteLimit-Buffer.byteLength(JSON.stringify(x,null,2)));
  assert.equal(Buffer.byteLength(JSON.stringify(x,null,2)),byteLimit);assert.ok(x.questions[4].passage.length<30000);
  const h=await numericHarness(x);let constructed=0;const Writer=h.zip.Writer;h.zip.Writer=class extends Writer{constructor(){super();constructed++;}};
  const before=JSON.stringify(h.probe.inspect().exam);await h.probe.export();assert.equal(h.probe.stats.compile,1);
  assert.equal(constructed,byteLimit===131072?1:0,'actual serialized byte admission precedes ZIP');assert.equal(JSON.stringify(h.probe.inspect().exam),before);
  assert.equal(h.node('btn-download-nstuexam').disabled,byteLimit!==131072);
  if(byteLimit===131072){h.node('btn-download-nstuexam').onclick();const entries=await new h.zip.Reader(await h.downloads[0].blob.arrayBuffer()).readEntries();
    const bytes=entries.find(e=>e.path==='manifest.json').bytes;assert.equal(bytes.byteLength,131072);assert.deepEqual(JSON.parse(new TextDecoder().decode(bytes)),x);
  }else{assert.match(h.node('export-validation-status').innerHTML,/131072/);assert.equal(h.downloads.length,0);}
  exportControllerCases++;
}
{
  const h=await numericHarness(nativeFixture());let resolveBuild;h.zip.Writer.prototype.build=function(){return new Promise(resolve=>resolveBuild=resolve);};
  const first=h.probe.export();for(let i=0;i<100&&!resolveBuild;i++)await new Promise(resolve=>setTimeout(resolve,10));assert.equal(typeof resolveBuild,'function');
  h.probe.inspect().exam.questions[0].pdf='';await h.probe.export();const status=h.node('export-validation-status').innerHTML;
  resolveBuild({fileCount:5,totalBytes:100,archiveSha256:'stale',contentSha256:'stale',blob:new Blob()});await first;
  assert.equal(h.node('export-validation-status').innerHTML,status);assert.equal(h.node('btn-download-nstuexam').disabled,true);
  assert.equal(h.node('lbl-archive-hash').textContent,'-');exportControllerCases++;
}
console.log(`Studio actual Export limits, whole-second seeds, rejection/custody/Save: ${exportControllerCases} cases passed`);

let nativeRailCases=1;
for(const [type,name,matching] of [['essay','Essay',false],['short_answer','Short answer',false],['multiple_choice','Multiple choice',false],
  ['listening','Listening',false],['reading','Reading',false],['reading','Matching',true]]){
  const x=nativeFixture();Object.assign(x.questions[0],{type,isMatching:matching,prompt:'Name "ế" <b>keep</b> & 𝄞',options:['A','B'],matchingTargets:['Target']});
  const h=await numericHarness(x),before=JSON.stringify(h.probe.inspect().exam),markup=h.node('question-nav-list').children[0].innerHTML;
  assert.match(markup,/<button[^>]*type="button"[^>]*aria-current="true"/);assert.equal((markup.match(/<button\b/g)||[]).length,2);
  assert.ok(markup.includes(`aria-label="Question 1: ${name}. Name &quot;ế&quot; &lt;b&gt;keep&lt;/b&gt; &amp; 𝄞"`));
  assert.ok(markup.includes('aria-label="Delete question 1"'));assert.ok(markup.indexOf('</button>')<markup.indexOf('data-delete='));
  assert.equal(JSON.stringify(h.probe.inspect().exam),before,'Native naming/rendering does not mutate author data');nativeRailCases++;
}
for(const text of ['', 'Edited "𝄞" <img onerror=alert(1)> e\u0302\u0301']){
  const h=await numericHarness(),state=h.probe.inspect(),other=JSON.stringify(state.exam.questions[1]),renders=h.probe.stats.editor;
  state.answers['essay-a']='Retained Preview answer';const input=h.node('q-prompt');input.value=text;input.selectionStart=2;input.selectionEnd=5;input.focus();
  for(const callback of input.listeners.input)callback({target:input});
  assert.equal(h.probe.inspect().exam,state.exam);assert.equal(state.exam.questions[0].prompt,text);assert.equal(JSON.stringify(state.exam.questions[1]),other);
  assert.equal(h.node('question-nav-list').children[0].querySelector('.question-select')['aria-label'],`Question 1: Essay. ${text||'Untitled Question'}`);
  assert.equal(h.probe.stats.editor,renders,'Live accessible name update does not rerender or reset rail focus');
  assert.equal(h.document.activeElement,input);assert.deepEqual([input.selectionStart,input.selectionEnd],[2,5]);assert.equal(h.probe.inspect().answers,state.answers);
  assert.equal(state.answers['essay-a'],'Retained Preview answer');nativeRailCases++;
}
console.log(`Studio actual native question rail names/markup/live binding/identity: ${nativeRailCases} cases passed`);

// A truthy fallback in Preview must not substitute one point for a stored zero.
let previewPointCases=0;
for(const [type,matching] of [['essay',false],['short_answer',false],['multiple_choice',false],['listening',false],['reading',false],['reading',true]]){
  for(const points of [0,1,101,1000,undefined,1.25,-1]){
    const x=nativeFixture();Object.assign(x.questions[0],{type,isMatching:matching,options:['A','B'],matchingTargets:['Target']});
    if(points===undefined)delete x.questions[0].points;else x.questions[0].points=points;
    const h=await numericHarness(x),state=h.probe.inspect(),before=JSON.stringify(state.exam),q=state.exam.questions[0];
    state.answers[q.id]=matching?{0:0}:type==='essay'||type==='short_answer'?'Retained response':1;
    const answers=state.answers,answer=answers[q.id];h.node('btn-mode-preview').click();
    assert.equal(h.node('p-question-points').textContent,`${points===undefined?1:points} points`,'Preview shows literal stored points or absent default');
    assert.equal(h.probe.inspect().exam,state.exam);assert.equal(JSON.stringify(state.exam),before);assert.equal(h.probe.inspect().answers,answers);assert.equal(answers[q.id],answer);
    assert.equal(h.probe.stats.compile,0);h.node('btn-mode-editor').click();h.probe.save();
    assert.equal(JSON.stringify(JSON.parse(await h.downloads[0].blob.text())),before,'Preview and Save retain complete authoring data');
    assert.equal(Object.hasOwn(state.exam.questions[0],'points'),points!==undefined,'Absent points remain absent in Save');previewPointCases++;
  }
}
{
  const x=numericFixture();x.questions[0].points=0;const h=await numericHarness(x);numericInput(h,'q-points','1e');
  h.node('btn-mode-preview').click();assert.equal(h.node('p-question-points').textContent,'0 points');h.node('btn-mode-editor').click();
  assert.equal(h.node('q-points').value,'1e');assert.equal(h.node('q-points')['aria-invalid'],'true');assert.equal(h.probe.inspect().exam.questions[0].points,0);previewPointCases++;
}
console.log(`Studio actual Preview point captions/defaults/model/Save/answer/raw identity: ${previewPointCases} cases passed`);

const matchingKeyboardFixture=()=>({id:'matching-keyboard',title:'Matching keyboard',subject:'',durationSeconds:62,questions:[
  {id:'match-a',type:'reading',isMatching:true,prompt:'Match',points:0,options:['Heading A','Heading B','Heading C'],matchingTargets:['Target A','Target B','Target C'],passage:'Passage'},
  {id:'match-b',type:'reading',isMatching:true,prompt:'Other',points:1,options:['Other A','Other B'],matchingTargets:['Other target'],passage:'Other passage'}]});
let matchingKeyboardCases=0;
{
  const h=await numericHarness(matchingKeyboardFixture());h.node('btn-mode-preview').click();
  const state=h.probe.inspect(),model=JSON.stringify(state.exam),answers=state.answers,assignment=answers['match-a'];
  const token=i=>h.node('p-token-tray').children.find(n=>Number(n.dataset.tokenIdx)===i),slot=i=>h.node('p-target-list').children[i].children[1],target=i=>slot(i).querySelector('.target-select');
  assert.equal(token(0).tagName,'BUTTON','Matching headings must be native keyboard buttons');assert.equal(token(0).type,'button');
  assert.equal(target(0).tagName,'BUTTON','Matching targets must have separate native activation');assert.equal(target(0).type,'button');matchingKeyboardCases++;
  token(0).focus();token(0).click();assert.equal(token(0)['aria-pressed'],'true');assert.equal(h.document.activeElement,token(0));matchingKeyboardCases++;
  token(0).click();assert.equal(token(0)['aria-pressed'],'false');assert.equal(h.document.activeElement,token(0));matchingKeyboardCases++;
  token(0).click();target(0).focus();slot(0).click();assert.equal(assignment[0],0);assert.equal(h.document.activeElement,target(0));assert.equal(h.probe.inspect().token,null);matchingKeyboardCases++;
  token(1).focus();token(1).click();target(0).focus();slot(0).click();assert.equal(assignment[0],1);assert(token(0));assert.equal(h.document.activeElement,target(0));matchingKeyboardCases++;
  const removal=slot(0).querySelector('.chip-remove-btn');assert.equal(removal.tagName,'BUTTON');assert.equal(removal['aria-label'],'Remove heading from target 1: Target A');removal.focus();removal.click();assert.equal(Object.hasOwn(assignment,'0'),false);assert.equal(h.document.activeElement,target(0));matchingKeyboardCases++;
  const same=target(0);same.focus();slot(0).click();assert.equal(target(0),same);assert.equal(h.document.activeElement,same);matchingKeyboardCases++;
  for(let i=0;i<3;i++){token(i).focus();token(i).click();target(i).focus();slot(i).click();assert.equal(assignment[i],i);assert.equal(h.document.activeElement,target(i));matchingKeyboardCases++;}
  assert.equal(h.node('p-token-tray').children.length,0);assert.match(h.node('p-token-tray').innerHTML,/All headings have been placed/);matchingKeyboardCases++;
  h.probe.place(2,0);assert.equal(assignment[2],0);assert.equal(Object.hasOwn(assignment,'0'),false);assert(token(2));matchingKeyboardCases++;
  h.node('btn-p-next').click();assert.equal(Object.keys(answers['match-b']).length,0);h.node('btn-p-prev').click();assert.equal(answers['match-a'],assignment);assert.equal(assignment[2],0);matchingKeyboardCases++;
  h.node('btn-mode-editor').click();numericInput(h,'q-points','1e');h.node('btn-mode-preview').click();assert.equal(answers['match-a'],assignment);h.node('btn-mode-editor').click();assert.equal(h.node('q-points').value,'1e');
  assert.equal(h.probe.inspect().exam,state.exam);assert.equal(JSON.stringify(state.exam),model);assert.equal(h.probe.inspect().answers,answers);h.probe.save();assert.equal(JSON.stringify(JSON.parse(await h.downloads[0].blob.text())),model);matchingKeyboardCases++;
  await h.probe.export();assert.equal(h.probe.stats.compile,0);assert.equal(h.node('btn-download-nstuexam').disabled,true);matchingKeyboardCases++;
}
console.log(`Studio actual matching native controls/placement/removal/focus/data: ${matchingKeyboardCases} cases passed`);

async function matchingResizeHarness(){const h=await numericHarness(matchingKeyboardFixture());h.node('btn-mode-preview').click();const surface=h.node('p-matching-surface');for(const id of ['p-token-tray','p-target-list']){const node=h.node(id);node.remove();surface.appendChild(node);}const field=h.node('p-target-list').children[2].children[1].querySelector('.target-select');field.focus();return{h,field,surface,queue:()=>h.window.listeners.resize.find(callback=>callback.name==='queueMatchingFocusReveal')({type:'resize'})};}
let matchingResizeCases=0;
{
  const {h,field,queue}=await matchingResizeHarness(),state=h.probe.inspect(),model=JSON.stringify(state.exam),stats=JSON.stringify(h.probe.stats),focus=field.focusCalls;
  queue();queue();assert.equal(h.window.frames.size,1);assert.deepEqual(h.window.cancelled,[0],'matching RAF id0 is cancellable');h.flushFrames();
  assert.equal(field.scrolls.length,1);assert.deepEqual(JSON.parse(JSON.stringify(field.scrolls[0])),{block:'nearest',inline:'nearest',behavior:'instant'});assert.equal(h.document.activeElement,field);assert.equal(field.focusCalls,focus);
  assert.equal(JSON.stringify(state.exam),model);assert.equal(h.probe.inspect().answers,state.answers);assert.equal(JSON.stringify(h.probe.stats),stats);queue();h.flushFrames();assert.equal(field.scrolls.length,2);assert.deepEqual(h.window.cancelled,[0]);matchingResizeCases++;
}
for(const invalidate of [({field})=>field.hidden=true,({field})=>field.inert=true,({field})=>field.disabled=true,({field})=>field.remove(),({field})=>field.ownerDocument={},({field})=>field.style.visibility='hidden',({surface})=>surface.style.display='none',({h})=>h.node('preview-view').style.display='none',({h})=>h.node('export-modal').classList.add('open'),({h})=>h.node('btn-save-draft').focus(),({h})=>h.probe.inspect().exam.questions[0].isMatching=false]){
  const ctx=await matchingResizeHarness();ctx.queue();invalidate(ctx);const state=ctx.h.probe.inspect(),model=JSON.stringify(state.exam),active=ctx.h.document.activeElement;ctx.h.flushFrames();assert.equal(ctx.field.scrolls?.length||0,0);assert.equal(ctx.h.document.activeElement,active);assert.equal(JSON.stringify(state.exam),model);assert.equal(ctx.h.probe.inspect().answers,state.answers);matchingResizeCases++;
}
{
  const {h,field,surface,queue}=await matchingResizeHarness();queue();const next=h.node('p-target-list').children[0].children[1].querySelector('.target-select');next.focus();h.flushFrames();assert.equal(field.scrolls?.length||0,0);assert.equal(next.scrolls.length,1);assert.equal(h.document.activeElement,next);matchingResizeCases++;
}
console.log(`Studio actual matching resize lifecycle/guards/current focus: ${matchingResizeCases} cases passed`);

// Native single-choice controls must project answers without replacing the focused radio.
const choiceFixture=type=>({id:'choice-preview',title:'Choices',subject:'',durationSeconds:600,questions:[
  {id:'choice-a',type,prompt:'Choose ế',options:['A "ế" <b>one</b>','B e\u0302\u0301','C']},
  {id:'choice-b',type:'multiple_choice',prompt:'Other',options:['Other A','Other B']}]});
const choiceRadio=(h,index)=>h.node('p-mc-surface').children[index].children[0];
function choiceChange(radio){radio.checked=true;for(const callback of radio.listeners.change||[])callback({target:radio,currentTarget:radio});}
let choiceKeyboardCases=0;
for(const type of ['multiple_choice','reading','listening']){
  const h=await numericHarness(choiceFixture(type)),state=h.probe.inspect(),before=JSON.stringify(state.exam);
  h.node('btn-mode-preview').click();const surface=h.node('p-mc-surface');
  assert.equal(surface.role,'radiogroup');assert.equal(surface['aria-labelledby'],'p-question-prompt');
  assert.equal(Object.hasOwn(state.answers,'choice-a'),false,'Rendering does not initialize an answer');
  for(let index=0;index<3;index++){
    const radio=choiceRadio(h,index);assert.equal(radio.tagName,'INPUT');assert.equal(radio.type,'radio');assert.equal(radio.name,'nstu-preview-choice-choice-a');
    assert.equal(radio.value,String(index));assert.equal(radio['aria-label'],`Option ${index+1}: ${state.exam.questions[0].options[index]}`);assert.equal(radio.checked,false);assert.equal(radio.listeners.focus.length,1,'one focus listener per rendered radio');
    assert.equal(surface.children[index].listeners.click?.length||0,0,'Native change owns selection; no legacy label-click path');
  }
  const labels=[...surface.children],radio=choiceRadio(h,1);radio.focus();const stats=JSON.stringify(h.probe.stats);choiceChange(radio);
  assert.equal(state.answers['choice-a'],1);assert.equal(h.document.activeElement,radio);assert.equal(radio.isConnected,true);assert.deepEqual(surface.children,labels);
  assert.equal(JSON.stringify(h.probe.stats),stats,'Selecting does not render');assert.equal(JSON.stringify(state.exam),before);assert.equal(h.probe.inspect().answers,state.answers);
  assert.equal(choiceRadio(h,0).checked,false);assert.equal(radio.checked,true);assert.equal(surface.children[1].classList.contains('checked'),true);
  choiceChange(choiceRadio(h,0));assert.equal(state.answers['choice-a'],0);assert.equal(choiceRadio(h,0).checked,true);assert.equal(radio.checked,false);
  choiceChange(choiceRadio(h,0));assert.equal(state.answers['choice-a'],0);assert.deepEqual(surface.children,labels);
  h.node('btn-p-next').click();assert.equal(Object.hasOwn(state.answers,'choice-b'),false);choiceChange(choiceRadio(h,1));assert.equal(state.answers['choice-b'],1);
  h.node('btn-p-prev').click();assert.equal(choiceRadio(h,0).checked,true);assert.equal(state.answers['choice-b'],1);
  h.node('btn-mode-editor').click();h.node('btn-mode-preview').click();assert.equal(choiceRadio(h,0).checked,true);assert.equal(JSON.stringify(state.exam),before);choiceKeyboardCases++;
}
{
  const h=await numericHarness(choiceFixture('multiple_choice')),state=h.probe.inspect();state.answers['choice-a']=0;
  const unrelated=h.node('btn-save-draft');unrelated.focus();h.node('btn-mode-preview').click();assert.equal(h.document.activeElement,unrelated,'Rendering does not focus a radio');
  assert.equal(choiceRadio(h,0).checked,true);assert.equal(choiceRadio(h,1).checked,false);assert.match(h.node('p-mc-surface').children[0].innerHTML,/&lt;b&gt;one&lt;\/b&gt;/);
  const input=choiceRadio(h,1);input.checked=false;for(const callback of input.listeners.change)callback({target:input});assert.equal(state.answers['choice-a'],0,'Unchecked change cannot clear or select a response');choiceKeyboardCases++;
}
{
  const h=await numericHarness(choiceFixture('multiple_choice')),state=h.probe.inspect();state.exam.questions[0].options=[];
  h.node('btn-mode-preview').click();assert.equal(h.node('p-mc-surface').children.length,0);assert.equal(Object.hasOwn(state.answers,'choice-a'),false);choiceKeyboardCases++;
}
{
  const h=await numericHarness(choiceFixture('multiple_choice'));h.node('btn-mode-preview').click();choiceChange(choiceRadio(h,0));const state=h.probe.inspect(),before=JSON.stringify(state.exam);h.probe.save();
  assert.equal(JSON.stringify(JSON.parse(await h.downloads[0].blob.text())),before,'Native choice responses do not alter the whole authoring Save');
  const fresh={...choiceFixture('reading'),id:'fresh-choice'};await h.probe.load({name:'fresh.json',text:async()=>JSON.stringify(fresh)});
  assert.equal(Object.hasOwn(h.probe.inspect().answers,'choice-a'),false);assert.equal(choiceRadio(h,0).checked,false);assert.equal(choiceRadio(h,1).checked,false);choiceKeyboardCases++;
}
console.log(`Studio actual single-choice native semantics/stable focus/response indices: ${choiceKeyboardCases} cases passed`);

async function choiceResizeHarness(){const h=await numericHarness(choiceFixture('multiple_choice'));h.node('btn-mode-preview').click();const surface=h.node('p-mc-surface'),field=choiceRadio(h,2);field.focus();return{h,field,surface,label:field.parentNode,queue:()=>h.window.listeners.resize.find(callback=>callback.name==='queueChoiceFocusReveal')({type:'resize'})};}
let choiceResizeCases=0;
{
  const {h,field,label,queue}=await choiceResizeHarness(),state=h.probe.inspect(),model=JSON.stringify(state.exam),stats=JSON.stringify(h.probe.stats),focus=field.focusCalls;
  assert.equal(label.scrolls.length,1,'ordinary radio focus reveals full proxy before resize');queue();queue();assert.equal(h.window.frames.size,1);assert.deepEqual(h.window.cancelled,[0]);h.flushFrames();
  assert.equal(label.scrolls.length,2);assert.deepEqual({...label.scrolls[1]},{block:'nearest',inline:'nearest',behavior:'instant'});assert.equal(field.focusCalls,focus);assert.equal(h.document.activeElement,field);
  assert.equal(JSON.stringify(state.exam),model);assert.equal(h.probe.inspect().answers,state.answers);assert.equal(JSON.stringify(h.probe.stats),stats);choiceResizeCases++;
}
for(const invalidate of [({field})=>field.hidden=true,({field})=>field.inert=true,({field})=>field.disabled=true,({field})=>field.remove(),({field})=>field.ownerDocument={},({field})=>field.style.visibility='hidden',({surface})=>surface.style.display='none',({h})=>h.node('preview-view').style.display='none',({h})=>h.node('export-modal').classList.add('open'),({h})=>h.node('btn-save-draft').focus(),({h})=>h.probe.inspect().exam.questions[0].isMatching=true,({h})=>h.probe.inspect().exam.questions[0].type='essay',({field})=>field.type='text']){
  const ctx=await choiceResizeHarness();assert.equal(ctx.label.scrolls.length,1);ctx.queue();invalidate(ctx);const state=ctx.h.probe.inspect(),model=JSON.stringify(state.exam),active=ctx.h.document.activeElement;ctx.h.flushFrames();assert.equal(ctx.label.scrolls.length,1,'ineligible queued resize adds no reveal');assert.equal(ctx.h.document.activeElement,active);assert.equal(JSON.stringify(state.exam),model);assert.equal(ctx.h.probe.inspect().answers,state.answers);choiceResizeCases++;
}
{
  const {h,field,label,queue}=await choiceResizeHarness();queue();const next=choiceRadio(h,0);next.focus();assert.equal(next.parentNode.scrolls.length,1,'new current radio has separate native focus reveal');h.flushFrames();assert.equal(label.scrolls.length,1);assert.equal(next.parentNode.scrolls.length,2);assert.equal(h.document.activeElement,next);choiceResizeCases++;
}
console.log(`Studio actual single-choice resize ownership/lifecycle/guards: ${choiceResizeCases} cases passed`);

const previewTextFixture=()=>({id:'text-preview',title:'Text Preview',subject:'',durationSeconds:600,questions:[
  {id:'essay',type:'essay',prompt:'Essay',points:0,wordLimit:250},{id:'short',type:'short_answer',prompt:'Short',points:1}]});
async function previewTextResizeHarness(short=false){const h=await numericHarness(previewTextFixture());h.node('btn-mode-preview').click();if(short)h.node('btn-p-next').click();const field=h.node(short?'p-sa-input':'p-essay-input'),view=h.node('preview-view');field.remove();view.appendChild(field);field.value='A e\u0302\u0301 Việt';field.selectionStart=2;field.selectionEnd=5;field.oninput({target:field});field.focus();return{h,field,view,queue:()=>h.window.listeners.resize.find(callback=>callback.name==='queuePreviewTextFocusReveal')({type:'resize'})};}
let previewTextResizeCases=0;
for(const short of [false,true]){
  const {h,field,queue}=await previewTextResizeHarness(short),state=h.probe.inspect(),before=JSON.stringify(state.exam),answers=JSON.stringify(state.answers),stats=JSON.stringify(h.probe.stats),focus=field.focusCalls,value=field.value;
  assert.equal(field.listeners.focus.length,1,'one native focus listener per persistent text control');assert.equal(field.scrolls.length,1,'ordinary native focus reveals independently of resize');
  queue();queue();assert.equal(h.window.frames.size,1);assert.deepEqual(h.window.cancelled,[0]);h.flushFrames();assert.equal(field.scrolls.length,2);assert.deepEqual({...field.scrolls[1]},{block:'nearest',inline:'nearest',behavior:'instant'});
  assert.equal(field.focusCalls,focus);assert.equal(h.document.activeElement,field);assert.equal(field.value,value);assert.equal(field.selectionStart,2);assert.equal(field.selectionEnd,5);assert.equal(JSON.stringify(state.exam),before);assert.equal(JSON.stringify(state.answers),answers);assert.equal(h.probe.inspect().answers,state.answers);assert.equal(JSON.stringify(h.probe.stats),stats);previewTextResizeCases++;
}
for(const invalidate of [({field})=>field.hidden=true,({field})=>field.inert=true,({field})=>field.disabled=true,({field})=>field.remove(),({field})=>field.ownerDocument={},({field})=>field.style.visibility='hidden',({view})=>view.style.display='none',({h})=>h.node('export-modal').classList.add('open'),({h})=>h.node('btn-save-draft').focus(),({h})=>h.probe.inspect().exam.questions[0].type='reading',({h,field})=>{field.remove();h.document.body.appendChild(field);},({h})=>h.probe.inspect().exam.questions[0].isMatching=true]){
  const ctx=await previewTextResizeHarness();assert.equal(ctx.field.scrolls.length,1);ctx.queue();invalidate(ctx);const state=ctx.h.probe.inspect(),before=JSON.stringify(state.exam),answers=JSON.stringify(state.answers),active=ctx.h.document.activeElement;ctx.h.flushFrames();assert.equal(ctx.field.scrolls.length,1,'ineligible queued resize adds no reveal');assert.equal(ctx.h.document.activeElement,active);assert.equal(JSON.stringify(state.exam),before);assert.equal(JSON.stringify(state.answers),answers);previewTextResizeCases++;
}
{
  const {h,field,view,queue}=await previewTextResizeHarness();queue();h.node('btn-p-next').click();const next=h.node('p-sa-input');next.remove();view.appendChild(next);next.focus();assert.equal(next.scrolls.length,1,'new current input receives ordinary native reveal');const state=h.probe.inspect(),before=JSON.stringify(state.exam),answers=JSON.stringify(state.answers);h.flushFrames();assert.equal(field.scrolls.length,1);assert.equal(next.scrolls.length,2);assert.equal(h.document.activeElement,next);assert.equal(JSON.stringify(state.exam),before);assert.equal(JSON.stringify(state.answers),answers);previewTextResizeCases++;
}
console.log(`Studio actual text Preview resize ownership/caret/guards: ${previewTextResizeCases} cases passed`);

// Missing open/close helpers, stale Tab candidates or an unguarded listener must fail these real-controller cases.
let dialogCases=0;
for(const preview of [false,true]){
  const h=await actualImportHarness();h.seed(preview);h.node('exam-id').value='invalid id';
  const modal=h.node('export-modal'),exportButton=h.node('btn-export-pkg');
  assert.equal(modal.inert,true,'initial closed modal excludes keyboard/AX focus');
  exportButton.focus();h.node('btn-close-export').focus();assert.equal(h.document.activeElement,exportButton);
  assert.equal(h.key('Tab').prevented,0);assert.equal(h.key('Escape').prevented,0);
  const snapshot=h.probe.inspect(),modelContent=JSON.stringify(snapshot.exam),mode=h.node('preview-view').style.display;
  exportButton.click();assert.equal(h.document.activeElement,h.node('btn-cancel-export'),'Export starts on safe Cancel');
  assert.equal(modal.inert,false);assert.equal(modal.classList.contains('open'),true);assert.equal(h.document.listeners.keydown.length,1);
  const postSync=JSON.stringify(h.probe.inspect().exam);
  for(const [key,want] of [['Tab','btn-close-export'],['Tab','btn-cancel-export'],['Tab','btn-close-export']]){assert.equal(h.key(key).prevented,1);assert.equal(h.document.activeElement,h.node(want));}
  h.key('Tab',{shiftKey:true});assert.equal(h.document.activeElement,h.node('btn-cancel-export'));
  h.node('btn-download-nstuexam').disabled=false;h.node('btn-cancel-export').focus();h.key('Tab');assert.equal(h.document.activeElement,h.node('btn-download-nstuexam'));
  h.node('btn-download-nstuexam').disabled=true;h.key('Tab',{shiftKey:true});assert.equal(h.document.activeElement,h.node('btn-cancel-export'));
  exportButton.focus();h.key('Tab');assert.equal(h.document.activeElement,h.node('btn-close-export'));exportButton.focus();h.key('Tab',{shiftKey:true});assert.equal(h.document.activeElement,h.node('btn-cancel-export'));
  for(const options of [{ctrlKey:true},{altKey:true},{metaKey:true}])for(const key of ['Tab','Escape']){const before=h.document.activeElement;assert.equal(h.key(key,options).prevented,0);assert.equal(h.document.activeElement,before);assert(modal.classList.contains('open'));}
  assert.equal(h.key('Escape',{shiftKey:true}).prevented,0);
  assert.equal(h.key('Escape').prevented,1);assert.equal(h.document.activeElement,exportButton);assert.equal(modal.inert,true);
  assert.equal(JSON.stringify(h.probe.inspect().exam),postSync);assert.equal(h.probe.inspect().answers,snapshot.answers);assert.equal(h.node('preview-view').style.display,mode);assert.notEqual(postSync,modelContent,'existing Export metadata sync still runs');
  for(const id of ['btn-close-export','btn-cancel-export']){exportButton.click();h.node(id).click();assert.equal(h.document.activeElement,exportButton);assert.equal(modal.inert,true);}
  h.node('exam-title').focus();h.node('btn-close-export').click();assert.equal(h.document.activeElement,h.node('exam-title'),'repeated close is harmless');
  assert.equal(h.downloads.length,0);dialogCases++;
}
// Eligibility, degenerate candidates and opener lifetime through actual init callbacks.
for(const disqualify of [n=>n.hidden=true,n=>n.inert=true,n=>n.disabled=true,n=>n.style.display='none',n=>n.style.visibility='hidden',n=>n.style.visibility='collapse',n=>n.tabIndex=-1,n=>n.remove(),n=>n.ownerDocument={}]){
  const h=await actualImportHarness();h.node('exam-id').value='invalid id';const cancel=h.node('btn-cancel-export');disqualify(cancel);
  h.node('btn-export-pkg').click();assert.equal(h.document.activeElement,h.node('btn-close-export'),'unusable Cancel falls back inside');h.key('Tab');assert.equal(h.document.activeElement,h.node('btn-close-export'),'one eligible candidate cycles');h.key('Tab',{shiftKey:true});assert.equal(h.document.activeElement,h.node('btn-close-export'));dialogCases++;
}
{
  const h=await actualImportHarness();h.node('exam-id').value='invalid id';h.node('btn-close-export').disabled=true;h.node('btn-cancel-export').disabled=true;
  h.node('btn-export-pkg').click();assert.equal(h.document.activeElement,h.node('export-modal'));h.key('Tab');assert.equal(h.document.activeElement,h.node('export-modal'));h.key('Tab',{shiftKey:true});assert.equal(h.document.activeElement,h.node('export-modal'));dialogCases++;
}
{
  const h=await actualImportHarness();h.node('exam-id').value='invalid id';const group=h.document.createElement('fieldset');group.disabled=true;h.node('export-modal').appendChild(group);const cancel=h.node('btn-cancel-export');cancel.remove();group.appendChild(cancel);
  h.node('btn-export-pkg').click();assert.equal(h.document.activeElement,h.node('btn-close-export'));h.key('Tab');assert.equal(h.document.activeElement,h.node('btn-close-export'));dialogCases++;
}
for(const invalidOpener of [n=>n.remove(),n=>n.hidden=true,n=>n.inert=true,n=>n.disabled=true,n=>n.tabIndex=-1,n=>n.ownerDocument={}]){
  const h=await actualImportHarness();h.node('exam-id').value='invalid id';const opener=h.node('exam-title');opener.focus();await h.probe.export({currentTarget:opener});invalidOpener(opener);h.key('Escape');assert.equal(h.document.activeElement,h.node('btn-export-pkg'));dialogCases++;
}
for(const currentTarget of ['valid','body','foreign','disabled','missing']){
  const h=await actualImportHarness();h.node('exam-id').value='invalid id';const active=h.node('exam-title'),trigger=h.node('btn-open-file');active.focus();let target=trigger;
  if(currentTarget==='body')target=h.document.body;if(currentTarget==='foreign')trigger.ownerDocument={};if(currentTarget==='disabled')trigger.disabled=true;if(currentTarget==='missing')target=undefined;
  await h.probe.export({currentTarget:target});await h.probe.export({currentTarget:h.node('btn-cancel-export')});h.key('Escape');assert.equal(h.document.activeElement,currentTarget==='valid'?trigger:active,'reopen preserves original outside opener');dialogCases++;
}
{
  const h=await actualImportHarness();h.node('exam-id').value='invalid id';h.document.activeElement=h.document.body;await h.probe.export();h.key('Escape');assert.equal(h.document.activeElement,h.node('btn-export-pkg'));dialogCases++;
}
for(const fail of [false,true]){
  const h=await actualImportHarness();await h.probe.load({name:'valid.json',text:async()=>JSON.stringify(importFixture())});
  let resolveBuild,rejectBuild;h.zip.Writer.prototype.build=function(){return new Promise((resolve,reject)=>{resolveBuild=resolve;rejectBuild=reject;});};
  // Keep the real exporter and asset checks; only the asynchronous build boundary is controlled.
  const completion=h.probe.export({currentTarget:h.node('btn-export-pkg')});
  for(let attempt=0;attempt<100&&!resolveBuild;attempt++)await new Promise(resolve=>setTimeout(resolve,10));
  assert.equal(typeof resolveBuild,'function','real asset checks reached the pending build boundary');
  h.node('btn-cancel-export').click();assert.equal(h.document.activeElement,h.node('btn-export-pkg'));
  if(fail)rejectBuild(new Error('Dismissed build failure'));else resolveBuild({fileCount:5,totalBytes:100,archiveSha256:'archive',contentSha256:'content',blob:new Blob()});
  await completion;assert.equal(h.node('export-modal').inert,true);assert.equal(h.node('export-modal').classList.contains('open'),false);assert.equal(h.document.activeElement,h.node('btn-export-pkg'));assert.equal(h.downloads.length,0);assert.match(h.node('export-validation-status').innerHTML,fail?/Dismissed build failure/:/Successfully/);dialogCases++;
}
console.log(`Studio actual Export dialog keyboard, filters, lifetime and pending dismissal: ${dialogCases} scenarios passed`);
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

// Save's actual handler must preserve authoring data after its existing metadata sync.
let authoringSaveCases=0;
for(const matching of [false,true])for(const points of [0,101,1.25]) {
  const h=await actualImportHarness();h.seed(false);const model={id:'authoring-save',title:'Original',subject:'Subject',durationSeconds:600,
    allowedOrigins:['old.example'],candidate:'Inert candidate',documents:[{id:'doc',url:'paper.pdf',title:'Paper'}],extra:{kept:['literal']},questions:[{
      id:'question-one',type:'reading',prompt:'NFC ế / NFD e\u0302\u0301 / 𝄞',instruction:'',points,wordLimit:9.5,
      options:['A',''],isMatching:matching,matchingTargets:[''],passage:'',answerPlaceholder:'',pdf:'paper.pdf',authoringExtension:{kept:true}
    }]};
  await h.probe.load(jsonImport(model));
  h.node('exam-title').value='  Saved authoring title  ';h.node('exam-subject').value='  Saved subject  ';h.node('exam-duration').value='17';
  h.node('exam-allowed-origins').value='alpha.example\nalpha.example\nbeta.example';
  h.probe.save();assert.equal(h.downloads.length,1);assert.equal(h.downloads[0].filename,'authoring-save-draft.json');
  assert.equal(h.node('toast-container').children.at(-1).textContent,'Authoring draft downloaded');
  assert.equal(h.downloads[0].blob.type,'application/json');assert.equal(h.probe.stats.compile,0,'Save must not invoke the runtime compiler');
  const text=await h.downloads[0].blob.text(), saved=JSON.parse(text), post=h.probe.inspect().exam;
  assert.equal(JSON.stringify(saved),JSON.stringify(post));assert.equal(saved.questions[0].points,points);assert.equal(saved.questions[0].isMatching,matching);
  assert.deepEqual(saved.questions[0].matchingTargets,['']);assert.equal(saved.questions[0].wordLimit,9.5);assert.equal(saved.questions[0].passage,'');assert.equal(saved.questions[0].answerPlaceholder,'');
  assert.equal(saved.title,'Saved authoring title');assert.equal(saved.subject,'Saved subject');assert.equal(saved.durationSeconds,1020);assert.deepEqual(saved.allowedOrigins,['alpha.example','beta.example']);
  assert.equal(saved.questions[0].prompt,model.questions[0].prompt);assert.deepEqual(saved.extra,model.extra);assert.equal(saved.questions[0].pdf,'paper.pdf');
  await h.probe.load({name:'saved-draft.json',text:async()=>text});assert.equal(h.probe.inspect().exam.questions[0].isMatching,matching);
  assert.equal(h.probe.inspect().exam.questions[0].matchingTargets[0],'');authoringSaveCases++;
}
const studioMarkup=await readFile(new URL('exam/studio/index.html',root),'utf8');
assert.match(studioMarkup,/<button class="btn btn-primary" id="btn-save-draft" type="button" title="Save authoring draft JSON">/);
console.log(`Studio actual authoring Save payload and roundtrip: ${authoringSaveCases} cases passed`);
