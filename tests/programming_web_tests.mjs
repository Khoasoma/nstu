import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import vm from 'node:vm';

const code = await readFile(new URL('../exam/shared/programming.js', import.meta.url), 'utf8');
const sandbox = { window: {}, TextEncoder, TextDecoder, Uint8Array, atob, btoa, setTimeout: () => 1, clearTimeout() {} };
vm.runInNewContext(code, sandbox, { filename: 'exam/shared/programming.js' });
const api = sandbox.window.NstuProgramming;
const manifest = () => ({ id: 'cs-test', title: 'Programming', durationSeconds: 600,
  contentMode: 'computer_science', programmingVersion: 1, sessionMode: 'exam', languages: ['cpp20', 'python3'],
  questions: [{ id: 'a', type: 'programming', revision: 1, prompt: 'Sum', points: 1,
    timeLimitMs: 1000, memoryLimitMiB: 64, samples: [{ id: 's1', input: ' 2 3\r\n\n', output: '5 \n' }, { id: 's2', input: '', output: '' }] }] });
const context = { packageId: 'cs-test', packageDigestHex: '1'.repeat(64), clientIdHex: '2'.repeat(32),
  sessionIdHex: '3'.repeat(32), candidateId: 'candidate' };
const hash = source => createHash('sha256').update(source, 'utf8').digest('hex');
const clone = value => JSON.parse(JSON.stringify(value));

assert.equal(api.validatePublicManifest(manifest()).length, 0);
for (const change of [m => m.secretTests = ['PRIVATE'], m => m.questions[0].answer = 'PRIVATE',
  m => m.questions[0].samples[0].private = true, m => m.programmingVersion = 2,
  m => m.languages = ['python3'], m => m.questions[0].samples = [],
  m => m.questions[0].samples[0].input = 'ế'.repeat(1366), m => m.questions[0].revision = 0,
  m => m.questions[0].timeLimitMs = 99, m => m.questions[0].memoryLimitMiB = 513]) {
  const m = manifest(); change(m);
  assert.ok(api.validatePublicManifest(m).length > 0, 'unsafe or unsupported CS field must be rejected');
}
const authored = manifest(); authored.privateGrading = { secret: 'CANARY' };
authored.questions[0].referenceSource = 'CANARY';
const projected = api.projectPublicManifest(authored);
assert.equal(JSON.stringify(projected).includes('CANARY'), false);
assert.equal(projected.questions[0].samples[0].input, ' 2 3\r\n\n');
assert.equal(projected.questions[0].samples[0].output, '5 \n');
assert.equal(api.sourceError('a'.repeat(32768)), '');
assert.ok(api.sourceError('a'.repeat(32769)));
assert.equal(api.sourceError('ế'.repeat(10922) + 'ab'), '');
assert.ok(api.sourceError('ế'.repeat(10923)));
for (const source of ['\0', '\u0001', '\ud800', '\udc00']) assert.ok(api.sourceError(source));
assert.equal(api.sourceError('\t\r\nprint("xin chào")'), '');

function harness(seed, failWrite = false, sendResult = true) {
  const values = new Map(seed ? [['session', seed]] : []);
  const messages = [];
  const storage = { getItem: key => values.get(key) || null,
    setItem(key, value) { if (typeof failWrite === 'function' ? failWrite(key, value) : failWrite) throw new Error('quota'); values.set(key, value); } };
  const session = api.createSession(manifest(), { context, key: 'session', storage, hash,
    send(message) { messages.push(clone(message)); return typeof sendResult === 'function' ? sendResult(message) : sendResult; } });
  return { session, values, messages };
}
const h = harness();
h.session.select('a', 'cpp20');
h.session.update('a', 'cpp20', 'int main() {}', { start: 4, end: 4 });
h.session.select('a', 'python3'); h.session.update('a', 'python3', 'print(5)');
h.session.select('a', 'cpp20');
assert.equal(h.session.document('a', 'cpp20').source, 'int main() {}');
assert.equal(h.session.document('a', 'python3').source, 'print(5)');
assert.equal(h.session.selectedLanguage('a'), 'cpp20');
assert.equal(h.session.selectedSample('a'), 's1');
const submit = h.session.request('submit', 'a', 'cpp20');
assert.equal(submit.type, 'exam_programming_request');
assert.equal(submit.operation, 'submit');
assert.equal(submit.source, 'int main() {}');
h.session.update('a', 'cpp20', 'int main() { return 0; }');
assert.equal(submit.source, 'int main() {}', 'submitted source remains immutable');
const receipt = { type: 'exam_programming_response', operation: 'submit', requestId: submit.requestId,
  problemId: 'a', problemRevision: 1, language: 'cpp20', draftRevision: submit.draftRevision,
  sourceDigest: hash(submit.source), status: 'received', receiptId: 'receipt-1', serverTime: 123456 };
assert.equal(h.session.receive({ ...receipt, problemId: 'foreign' }), false);
assert.equal(h.session.receive({ ...receipt, sourceDigest: '0'.repeat(64) }), false);
assert.equal(h.session.receive({ ...receipt, verdict: 'accepted' }), false);
assert.equal(h.session.receive({ ...receipt, fullContext: { ...context, candidateId: 'foreign' } }), false);
assert.equal(h.session.receive({ ...receipt, programmingVersion: 2 }), false);
assert.equal(h.session.receive(receipt), true);
assert.equal(h.session.document('a', 'cpp20').source, 'int main() { return 0; }');
assert.equal(h.session.delivery('a', 'cpp20'), 'changed');
const run = h.session.request('run', 'a', 'cpp20', 's1');
const cancel = h.session.request('cancel', 'a', 'cpp20');
assert.equal(cancel.targetRunId, run.requestId);
assert.notEqual(cancel.requestId, run.requestId);
assert.equal(Object.hasOwn(cancel, 'draftRevision'), false);
assert.equal(h.session.receive({ type: 'exam_programming_response', operation: 'cancel', requestId: cancel.requestId,
  problemId: 'a', problemRevision: 1, language: 'cpp20', status: 'cancelled' }), true);
assert.equal(h.session.runResult('a').status, 'running', 'cancel acknowledgment does not claim process termination');
assert.equal(h.session.runResult('a').cancelRequested, true);
h.session.update('a', 'cpp20', 'int main() { return 1; }');
assert.equal(h.session.receive({ type: 'exam_programming_response', operation: 'run', requestId: run.requestId,
  problemId: 'a', problemRevision: 1, language: 'cpp20', draftRevision: run.draftRevision,
  sourceDigest: hash(run.source), status: 'completed', stdout: '<script>literal</script>', stderr: '', diagnostics: '', truncated: false }), true);
assert.equal(h.session.runResult('a').stale, true);
assert.equal(h.session.runResult('a').stdout, '<script>literal</script>');
h.session.update('a', 'cpp20', run.source);
assert.equal(h.session.runResult('a').stale, false);
h.session.select('a', 'python3');
assert.equal(h.session.runResult('a').stale, true, 'changing language keeps the old run bound to its original snapshot');
h.session.select('a', 'cpp20'); h.session.selectSample('a', 's2');
assert.equal(h.session.runResult('a').stale, true, 'changing sample cannot attach earlier output to the new input');
h.session.selectSample('a', 's1'); h.session.update('a', 'cpp20', 'int main() { return 1; }');
const restored = harness(h.values.get('session'));
assert.equal(restored.session.document('a', 'python3').source, 'print(5)');
assert.equal(restored.session.document('a', 'cpp20').source, 'int main() { return 1; }');
assert.equal(restored.session.selectedLanguage('a'), 'cpp20');
const failed = harness(null, true);
assert.equal(failed.session.update('a', 'cpp20', 'visible unsaved'), false);
assert.equal(failed.session.document('a', 'cpp20').source, 'visible unsaved');
assert.equal(failed.session.localSaved(), false);

const finish = harness(); finish.session.update('a', 'cpp20', 'int main() {}');
finish.session.finish();
assert.equal(finish.session.frozen(), true);
assert.equal(finish.session.finished(), false);
const finalSubmission = finish.messages.find(message => message.operation === 'submit');
assert.ok(finalSubmission, 'Finish captures the latest selected source before finalization');
assert.equal(finish.messages.some(message => message.operation === 'finalize'), false,
  'finalization waits for real source receipts');
finish.session.receive({ ...receipt, requestId: finalSubmission.requestId,
  draftRevision: finalSubmission.draftRevision, sourceDigest: hash(finalSubmission.source) });
const finalRequest = finish.messages.find(message => message.operation === 'finalize');
assert.ok(finalRequest);
assert.deepEqual(clone(finalRequest.receiptIds), ['receipt-1']);
assert.deepEqual(clone(finalRequest.noSourceProblemIds), []);
assert.equal(finish.session.finished(), false);
finish.session.receive({ type: 'exam_programming_response', operation: 'finalize', requestId: finalRequest.requestId,
  status: 'finalized', receiptId: 'finish-1', serverTime: 123456 });
assert.equal(finish.session.finished(), true);

const recoveredMessage = (intent, source, revision, extra = {}) => ({
  type: 'exam_programming_response', operation: 'recover', requestId: intent.requestId,
  fullContext: context, programmingVersion: 1, problemId: intent.problemId, problemRevision: 1,
  language: intent.language, status: 'recovered', draftRevision: revision, sourceDigest: hash(source),
  sourceBase64: Buffer.from(source, 'utf8').toString('base64'), latestReceiptId: '', receiptDigest: '',
  finalized: false, serverTime: 123456, ...extra });
const blankRecovery = harness(); blankRecovery.session.recover();
const blankIntent = blankRecovery.messages.find(message => message.operation === 'recover');
assert.equal(blankRecovery.session.receive(recoveredMessage(blankIntent, 'invalid revision zero source', 0)), false);
assert.equal(blankRecovery.session.receive(recoveredMessage(blankIntent, '', 0)), true,
  'untouched documents have a valid no-source recovery revision');
assert.equal(blankRecovery.session.document('a', 'cpp20').revision, 1, 'editable drafts retain a valid revision');
assert.equal(JSON.parse(blankRecovery.values.get('session')).pending.some(message => message.requestId === blankIntent.requestId), false);
const closedBlankIntent = blankRecovery.messages.find(message => message.operation === 'recover' && message.language === 'python3');
assert.equal(blankRecovery.session.receive(recoveredMessage(closedBlankIntent, '', 0, { finalized: true })), true);
assert.equal(blankRecovery.session.finished(), true, 'closed empty attempts recover their real finalization state');

const lateFinish = harness(); lateFinish.session.update('a', 'cpp20', 'int main() {}'); lateFinish.session.finish();
const lateSubmission = lateFinish.messages.find(message => message.operation === 'submit');
const lateReceipt = { ...receipt, requestId: lateSubmission.requestId, draftRevision: lateSubmission.draftRevision,
  sourceDigest: hash(lateSubmission.source), status: 'late', receiptId: 'receipt-late' };
assert.equal(lateFinish.session.receive({ ...lateReceipt, sourceDigest: '0'.repeat(64) }), false);
assert.equal(lateFinish.session.receive(lateReceipt), true);
assert.equal(lateFinish.session.delivery('a', 'cpp20'), 'late', 'durable late receipt does not imply grading eligibility');
assert.match(api.copy('en', 'late'), /not eligible/i);
const lateFinalRequest = lateFinish.messages.find(message => message.operation === 'finalize');
assert.ok(lateFinalRequest, 'a real late source receipt still permits final response-set delivery');
assert.deepEqual(clone(lateFinalRequest.receiptIds), ['receipt-late']);
assert.equal(JSON.parse(lateFinish.values.get('session')).pending.some(message => message.requestId === lateSubmission.requestId), false);

for (const failurePoint of ['lock', 'source']) {
  let fail = false;
  const transient = harness(null, (_, value) => fail && (failurePoint === 'lock' || JSON.parse(value).pending.some(message => message.operation === 'submit')));
  transient.session.update('a', 'cpp20', 'int main() {}'); fail = true; transient.session.finish();
  assert.equal(transient.session.frozen(), true, 'Finish keeps its snapshot locked while storage is unavailable');
  assert.equal(transient.messages.some(message => message.operation === 'submit'), false);
  fail = false; transient.session.retry(); transient.session.retry(); transient.session.finish();
  const submissions = transient.messages.filter(message => message.operation === 'submit');
  assert.ok(submissions.length > 0, `Finish resumes source delivery after ${failurePoint} persistence recovers`);
  assert.equal(new Set(submissions.map(message => message.requestId)).size, 1, 'retry reuses one immutable final source intent');
  assert.equal(submissions[0].source, 'int main() {}');
  transient.session.receive({ ...receipt, requestId: submissions[0].requestId,
    draftRevision: submissions[0].draftRevision, sourceDigest: hash(submissions[0].source) });
  assert.ok(transient.messages.some(message => message.operation === 'finalize'));
}

const coalesced = harness(); coalesced.session.update('a', 'cpp20', 'immutable submission');
const immutable = coalesced.session.request('submit', 'a', 'cpp20');
for (let index = 0; index < 256; ++index) {
  coalesced.session.update('a', 'cpp20', `// latest ${index}`);
  assert.ok(coalesced.session.request('draft', 'a', 'cpp20'), 'obsolete offline drafts do not exhaust recovery');
}
let storedPending = JSON.parse(coalesced.values.get('session')).pending;
assert.equal(storedPending.filter(message => message.operation === 'draft').length, 1);
assert.equal(storedPending.find(message => message.requestId === immutable.requestId).source, 'immutable submission');
assert.equal(storedPending.find(message => message.operation === 'draft').source, '// latest 255');
coalesced.session.finish();
assert.ok(coalesced.messages.some(message => message.operation === 'submit' && message.source === '// latest 255'));
let rollbackFailure = false;
const draftRollback = harness(null, () => rollbackFailure);
draftRollback.session.update('a', 'cpp20', 'previous draft'); const previousDraft = draftRollback.session.request('draft', 'a', 'cpp20');
draftRollback.session.update('a', 'cpp20', 'new draft'); rollbackFailure = true;
assert.equal(draftRollback.session.request('draft', 'a', 'cpp20'), null); rollbackFailure = false;
draftRollback.session.retry();
assert.equal(draftRollback.messages.at(-1).requestId, previousDraft.requestId, 'failed coalescing retains the preceding durable intent');

const newerRecovery = harness(); newerRecovery.session.update('a', 'cpp20', 'older local draft'); newerRecovery.session.recover();
const newerIntent = newerRecovery.messages.find(message => message.operation === 'recover');
assert.equal(newerRecovery.session.receive(recoveredMessage(newerIntent, 'newer durable draft', 10)), true);
assert.equal(newerRecovery.session.document('a', 'cpp20').source, 'newer durable draft');
assert.equal(newerRecovery.session.document('a', 'cpp20').revision, 10);
assert.equal(newerRecovery.session.undo('a', 'cpp20').source, 'older local draft', 'replacement preserves the previous local source in history');
const dirtyRecovery = harness(); dirtyRecovery.session.update('a', 'cpp20', 'first local draft'); dirtyRecovery.session.update('a', 'cpp20', 'newer dirty draft'); dirtyRecovery.session.recover();
const dirtyIntent = dirtyRecovery.messages.find(message => message.operation === 'recover');
assert.equal(dirtyRecovery.session.receive(recoveredMessage(dirtyIntent, 'older durable draft', 2)), true);
assert.equal(dirtyRecovery.session.document('a', 'cpp20').source, 'newer dirty draft');
assert.equal(dirtyRecovery.session.document('a', 'cpp20').revision, 3);
const emptyDirtyRecovery = harness(JSON.stringify({ context: JSON.stringify(context), documents: {
  'a:cpp20': { source: '', revision: 3, start: 0, end: 0, undo: [], redo: [] } }, pending: [] }));
emptyDirtyRecovery.session.recover();
const emptyDirtyIntent = emptyDirtyRecovery.messages.find(message => message.operation === 'recover');
assert.equal(emptyDirtyRecovery.session.receive(recoveredMessage(emptyDirtyIntent, 'older durable draft', 2)), true);
assert.equal(emptyDirtyRecovery.session.document('a', 'cpp20').source, '', 'a newer deliberate empty draft survives history trimming/recovery');
assert.equal(emptyDirtyRecovery.session.document('a', 'cpp20').revision, 3);
const conflictRecovery = harness(); conflictRecovery.session.update('a', 'cpp20', 'local conflicting draft'); conflictRecovery.session.recover();
const conflictIntent = conflictRecovery.messages.find(message => message.operation === 'recover');
assert.equal(conflictRecovery.session.receive(recoveredMessage(conflictIntent, 'other durable draft', 2)), false);
assert.equal(conflictRecovery.session.document('a', 'cpp20').source, 'local conflicting draft');
assert.match(conflictRecovery.session.error(), /conflict/i);
assert.equal(conflictRecovery.session.request('submit', 'a', 'cpp20'), null, 'an unresolved revision conflict cannot silently choose a final source');
conflictRecovery.session.update('a', 'cpp20', 'resolved local draft');
assert.equal(conflictRecovery.session.receive(recoveredMessage(conflictIntent, 'other durable draft', 2)), true);
assert.ok(conflictRecovery.session.request('submit', 'a', 'cpp20'));
const persistedConflict = harness(); persistedConflict.session.update('a', 'cpp20', 'original conflicting source'); persistedConflict.session.recover();
const persistedConflictIntent = persistedConflict.messages.find(message => message.operation === 'recover');
assert.equal(persistedConflict.session.receive(recoveredMessage(persistedConflictIntent, 'different durable source', 2)), false);
const editableConflictSnapshot = persistedConflict.values.get('session');
persistedConflict.session.finish();
assert.equal(persistedConflict.session.localSaved(), true);
assert.equal(persistedConflict.messages.some(message => ['submit', 'finalize'].includes(message.operation)), false);
const frozenConflictReload = harness(persistedConflict.values.get('session'));
assert.equal(frozenConflictReload.session.frozen(), true);
assert.equal(frozenConflictReload.session.document('a', 'cpp20').source, 'original conflicting source');
frozenConflictReload.session.retry();
assert.equal(frozenConflictReload.messages.some(message => ['submit', 'finalize'].includes(message.operation)), false,
  'Finish reload retry cannot transmit unresolved conflicting source');
assert.equal(frozenConflictReload.session.request('submit', 'a', 'cpp20'), null, 'Finish reload retains the unresolved document conflict');
assert.match(frozenConflictReload.session.error(), /conflict/i);
frozenConflictReload.session.retry(); frozenConflictReload.session.finish();
for (const [source, revision] of [['original conflicting source', 2], ['newer durable source', 10]]) {
  const recovery = frozenConflictReload.session.request('recover', 'a', 'cpp20');
  assert.equal(frozenConflictReload.session.receive(recoveredMessage(recovery, source, revision)), true);
  assert.equal(frozenConflictReload.session.document('a', 'cpp20').source, 'original conflicting source');
  assert.equal(frozenConflictReload.session.request('submit', 'a', 'cpp20'), null, 'recovery cannot resolve a frozen conflict');
}
assert.equal(frozenConflictReload.session.update('a', 'cpp20', 'unapproved resolution'), false);
assert.equal(frozenConflictReload.session.undo('a', 'cpp20'), null);
assert.equal(frozenConflictReload.messages.some(message => ['submit', 'finalize'].includes(message.operation)), false,
  'reload and retry never submit or finalize the unresolved Finish snapshot');
assert.equal(frozenConflictReload.session.finished(), false);
const editableConflictReload = harness(editableConflictSnapshot);
assert.equal(editableConflictReload.session.request('submit', 'a', 'cpp20'), null,
  'a recovery conflict survives reload before Finish');
editableConflictReload.session.update('a', 'cpp20', 'original conflicting source', { start: 1, end: 1 });
assert.equal(editableConflictReload.session.request('submit', 'a', 'cpp20'), null, 'selection-only updates cannot resolve conflicts');
editableConflictReload.session.recover();
const editableConflictIntent = editableConflictReload.messages.find(message => message.operation === 'recover');
assert.equal(editableConflictReload.session.receive(recoveredMessage(editableConflictIntent, 'older durable source', 1)), true);
assert.equal(editableConflictReload.session.request('submit', 'a', 'cpp20'), null, 'older recovery cannot resolve conflicts');
assert.equal(editableConflictReload.session.receive(recoveredMessage(editableConflictReload.session.request('recover', 'a', 'cpp20'), 'original conflicting source', 2)), true);
assert.ok(editableConflictReload.session.request('submit', 'a', 'cpp20'), 'accepted consistent recovery resolves an editable conflict');
for (const resolve of ['edit', 'undo', 'newer']) {
  const session = harness(editableConflictSnapshot).session;
  if (resolve === 'edit') session.update('a', 'cpp20', 'explicit resolution');
  else if (resolve === 'undo') {
    session.undo('a', 'cpp20');
    assert.ok(session.request('draft', 'a', 'cpp20'), 'undo resolves the conflict even when restoring empty source');
    session.undo('a', 'cpp20', true);
  }
  else assert.equal(session.receive(recoveredMessage(session.request('recover', 'a', 'cpp20'), 'newer accepted source', 3)), true);
  assert.ok(session.request('submit', 'a', 'cpp20'), `${resolve} explicitly resolves an editable conflict`);
}
for (const recoveryConflicts of [null, {}, 'a:cpp20', ['foreign:cpp20'], ['a:cpp20', 'a:cpp20'], ['a:cpp20', 1], Array(129).fill('a:cpp20')]) {
  const malformed = JSON.parse(persistedConflict.values.get('session')); malformed.recoveryConflicts = recoveryConflicts;
  const restoredConflict = harness(JSON.stringify(malformed));
  assert.equal(restoredConflict.session.document('a', 'cpp20').source, 'original conflicting source', 'malformed markers cannot discard local source');
  assert.equal(restoredConflict.session.frozen(), true);
  restoredConflict.session.retry(); restoredConflict.session.finish();
  assert.equal(restoredConflict.session.request('submit', 'a', 'cpp20'), null, 'malformed markers fail closed');
  assert.equal(restoredConflict.messages.some(message => ['submit', 'finalize'].includes(message.operation)), false);
  assert.equal(restoredConflict.session.finished(), false);
}
const blockedIntents = JSON.parse(persistedConflict.values.get('session'));
blockedIntents.pending.push({ ...clone(submit), requestId: 'queued-conflicting-submit', source: 'original conflicting source', draftRevision: 2 },
  { type: 'exam_programming_request', operation: 'finalize', requestId: 'queued-conflicting-finalize', context, receiptIds: [], noSourceProblemIds: [] });
const blockedRetry = harness(JSON.stringify(blockedIntents)); blockedRetry.session.retry();
assert.equal(blockedRetry.session.request('finalize'), null, 'direct finalization cannot bypass a restored source conflict');
assert.equal(blockedRetry.messages.some(message => ['submit', 'finalize'].includes(message.operation)), false,
  'restored source/finalization intents remain blocked by conflicts');
assert.deepEqual(JSON.parse(blockedRetry.values.get('session')).pending.filter(intent => blockedIntents.pending.some(original => original.requestId === intent.requestId)),
  blockedIntents.pending, 'blocked immutable intents are retained for explicit resolution');

for (const failurePoint of ['persist', 'capacity', 'send', 'throw']) {
  let failWrite = false, failSend = false;
  const admission = harness(null, () => failWrite, () => { if (failSend && failurePoint === 'throw') throw new Error('offline'); return !failSend; });
  admission.session.update('a', 'cpp20', 'source retained after failed run');
  const readyIntent = admission.session.request('ready');
  assert.equal(admission.session.receive({ type: 'exam_programming_response', operation: 'ready', requestId: readyIntent.requestId, status: 'ready', runnerReady: true }), true);
  if (failurePoint === 'capacity') for (let index = 0; index < 256; ++index) assert.ok(admission.session.request('recover', 'a', 'cpp20'));
  const before = admission.messages.length;
  failWrite = failurePoint === 'persist'; failSend = ['send', 'throw'].includes(failurePoint);
  const failedRun = admission.session.request('run', 'a', 'cpp20', 's1');
  assert.notEqual(admission.session.runResult('a')?.status, 'running', `${failurePoint} failure cannot invent an active run`);
  assert.equal(admission.session.document('a', 'cpp20').source, 'source retained after failed run');
  if (failSend) {
    assert.ok(failedRun, 'transport failure retains the admitted immutable request');
    assert.equal(admission.session.runResult('a').status, 'unavailable');
    assert.equal(admission.session.runnerReady(), false, 'failed transport invalidates runner readiness');
  } else {
    assert.equal(failedRun, null);
    assert.equal(admission.messages.length, before, 'failed admission never transmits a run');
  }
  failWrite = failSend = false;
  const attemptedRuns = admission.messages.filter(message => message.operation === 'run').length;
  admission.session.retry();
  assert.equal(admission.messages.filter(message => message.operation === 'run').length, attemptedRuns, 'runs never auto-retry after failed admission');
}
const frozenRecovery = harness(); frozenRecovery.session.update('a', 'cpp20', 'pinned final source'); frozenRecovery.session.recover();
const frozenIntent = frozenRecovery.messages.find(message => message.operation === 'recover');
frozenRecovery.session.finish();
const pinnedSubmission = frozenRecovery.messages.find(message => message.operation === 'submit');
assert.equal(frozenRecovery.session.receive(recoveredMessage(frozenIntent, 'later recovered source', 10)), true);
assert.equal(frozenRecovery.session.document('a', 'cpp20').source, 'pinned final source', 'recovery after Finish cannot replace the locked final snapshot');
assert.equal(frozenRecovery.session.document('a', 'cpp20').revision, pinnedSubmission.draftRevision);
assert.equal(new Set(frozenRecovery.messages.filter(message => message.operation === 'submit').map(message => message.source)).size, 1);
frozenRecovery.session.receive({ ...receipt, requestId: pinnedSubmission.requestId,
  draftRevision: pinnedSubmission.draftRevision, sourceDigest: hash(pinnedSubmission.source) });
assert.ok(frozenRecovery.messages.some(message => message.operation === 'finalize'));

class Node {
  constructor(tag, document) { this.tagName = tag; this.ownerDocument = document; this.children = []; this.listeners = {}; this.attributes = {}; this.value = ''; this.selectionStart = 0; this.selectionEnd = 0; }
  appendChild(child) { this.children.push(child); child.parentNode = this; return child; }
  append(...children) { children.forEach(child => this.appendChild(child)); }
  setAttribute(name, value) { this.attributes[name] = value; }
  addEventListener(name, callback) { (this.listeners[name] ||= []).push(callback); }
  emit(name, event = {}) { (this.listeners[name] || []).forEach(callback => callback({ target: this, preventDefault() {}, ...event })); }
  setRangeText(value, start, end) { this.value = this.value.slice(0, start) + value + this.value.slice(end); this.selectionStart = this.selectionEnd = start + value.length; }
  setSelectionRange(start, end) { this.selectionStart = start; this.selectionEnd = end; }
  focus() { this.ownerDocument.activeElement = this; }
}
const document = { createElement: tag => new Node(tag, document) };
const root = new Node('div', document), editorSession = harness().session;
const view = api.mountWorkspace(root, editorSession, manifest().questions[0], { language: () => 'en', started: () => true, preview: true });
const descendants = node => [node, ...node.children.flatMap(descendants)];
const all = descendants(root), editors = all.filter(node => node.className === 'cs-source');
const selector = all.find(node => node.className === 'cs-language');
const publicInput = all.find(node => node.tagName === 'pre');
assert.equal(publicInput.textContent, ' 2 3\r\n\n', 'sample loads automatically, exactly');
editors[0].value = 'xin chào'; editors[0].emit('compositionstart'); editors[0].emit('input');
assert.equal(editorSession.document('a', 'cpp20').source, '', 'composition is one eventual edit');
editors[0].emit('compositionend');
assert.equal(editorSession.document('a', 'cpp20').source, 'xin chào');
selector.value = 'python3'; selector.emit('change');
assert.equal(editors[0].hidden, true); assert.equal(editors[1].hidden, false);
editors[1].value = 'print(5)'; editors[1].emit('input');
selector.value = 'cpp20'; selector.emit('change');
assert.equal(editors[0].value, 'xin chào', 'language switch retains the original editor node/source');
view.refresh(); assert.equal(descendants(root).filter(node => node.className === 'cs-source')[0], editors[0]);
editors[0].emit('keydown', { key: 'z', ctrlKey: true });
assert.equal(editors[0].value, '', 'composition undo restores the preceding source as one edit');
assert.equal(all.find(node => node.textContent === 'Run sample').disabled, true, 'preview never invents execution readiness');
console.log('Programming public projection, byte limits, language drafts, snapshots, receipts and finalization: PASS');
