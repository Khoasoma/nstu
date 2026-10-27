"use strict";

// Runs the browser recovery state machine without a browser dependency. The
// shim intentionally exposes only the DOM and WebView2 calls used by app.js.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const APP_SOURCE = fs.readFileSync(
  path.join(__dirname, "..", "exam", "web", "app.js"), "utf8");
const MANIFEST = {
  id: "exam-web-recovery-test",
  title: "Recovery test",
  subject: "Test",
  durationSeconds: 600,
  documents: [],
  questions: [{
    id: "q1",
    type: "short_answer",
    points: 1,
    prompt: "Answer",
    answerPlaceholder: "Type an answer"
  }]
};

const HEX16 = "11".repeat(16);
const HEX16_B = "22".repeat(16);
const HEX32_A = "aa".repeat(32);
const HEX32_B = "bb".repeat(32);
const HEX32_C = "cc".repeat(32);
const HEX32_D = "dd".repeat(32);

class FakeClassList {
  constructor() { this.values = new Set(); }
  toggle(name, force) {
    const enabled = force === undefined ? !this.values.has(name) : Boolean(force);
    if (enabled) this.values.add(name); else this.values.delete(name);
    return enabled;
  }
  add(...names) { names.forEach((name) => this.values.add(name)); }
  remove(...names) { names.forEach((name) => this.values.delete(name)); }
  contains(name) { return this.values.has(name); }
}

class FakeNode {
  constructor(tagName = "div", id = "") {
    this.tagName = String(tagName).toUpperCase();
    this.id = id;
    this.dataset = Object.create(null);
    this.children = [];
    this.listeners = Object.create(null);
    this.classList = new FakeClassList();
    this.className = "";
    this.style = { setProperty: (name, value) => { this.style[name] = value; } };
    this.attributes = Object.create(null);
    this.value = "";
    this.checked = false;
    this.disabled = false;
    this.hidden = false;
    this.placeholder = "";
    this.open = false;
    this._textContent = "";
  }

  get textContent() { return this._textContent; }
  set textContent(value) {
    this._textContent = String(value ?? "");
    if (this._textContent === "") this.children = [];
  }

  append(...nodes) { nodes.forEach((node) => this.appendChild(node)); }
  appendChild(node) { this.children.push(node); return node; }
  addEventListener(type, listener) {
    (this.listeners[type] ||= []).push(listener);
  }
  dispatchEvent(event) {
    const current = event || {};
    if (!current.target) current.target = this;
    for (const listener of this.listeners[current.type] || []) {
      listener.call(this, current);
    }
    return !current.defaultPrevented;
  }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  showModal() { this.open = true; }
  matches(selector) {
    return selector.split(",").map((item) => item.trim()).some((item) => {
      return item === this.tagName.toLowerCase() || item === this.tagName;
    });
  }
}

class FakeDocument {
  constructor() {
    this.nodes = [];
    this.byId = new Map();
    this.listeners = Object.create(null);
    this.documentElement = this.addNode("html");
    this.documentElement.style = {
      setProperty: (name, value) => { this.documentElement.style[name] = value; }
    };
    this.body = this.addNode("body");
    this.body.classList = new FakeClassList();
    const ids = [
      "exam-shell", "loading-screen", "loading-status",
      "exam-title", "exam-subject", "candidate-label", "timer",
      "connection-status", "save-status", "question-count", "question-list",
      "question-position", "question-type", "question-points", "question-content",
      "pdf-pane", "notes-pane", "pdf-frame", "pdf-empty", "notes",
      "language-setting", "text-size-setting", "contrast-setting", "motion-setting",
      "settings-dialog", "submit-dialog", "submit-form", "submit-summary"
    ];
    ids.forEach((id) => this.addNode(
      id.endsWith("-dialog") ? "dialog" : id === "submit-form" ? "form" :
        id === "notes" ? "textarea" : id.endsWith("-setting") ? "select" : "div", id));
    this.getElementById("timer").className = "timer";
    this.getElementById("pdf-frame").hidden = true;
    this.getElementById("pdf-empty").hidden = false;
    this.getElementById("notes-pane").hidden = true;
    this.hidden = false;
    this.hasFocus = () => true;
    this.addActionButton("previous");
    this.addActionButton("next");
    this.addActionButton("settings");
    this.addActionButton("submit");
    this.addActionButton("download");
  }

  dispatchEvent(event) {
    const current = event || {};
    if (!current.target) current.target = this.body;
    for (const listener of this.listeners[current.type] || []) {
      listener.call(this, current);
    }
    return true;
  }

  addNode(tagName, id = "") {
    const node = new FakeNode(tagName, id);
    this.nodes.push(node);
    if (id) this.byId.set(id, node);
    return node;
  }
  addActionButton(action) {
    const node = this.addNode("button");
    node.dataset.action = action;
    return node;
  }
  getElementById(id) { return this.byId.get(id) || null; }
  createElement(tagName) { return this.addNode(tagName); }
  createTextNode(value) {
    const node = this.addNode("#text");
    node.textContent = value;
    return node;
  }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  querySelector(selector) {
    const action = /^\[data-action="([^"]+)"\]$/.exec(selector);
    if (action) return this.nodes.find((node) => node.dataset.action === action[1]) || null;
    if (selector === ".timer") return this.getElementById("timer");
    return null;
  }
  querySelectorAll(selector) {
    if (selector === "[data-action]") {
      return this.nodes.filter((node) => node.dataset.action);
    }
    if (selector === "[data-reference]") {
      return this.nodes.filter((node) => node.dataset.reference);
    }
    if (selector === "[data-i18n]") return [];
    if (selector === "[data-i18n-placeholder]") return [];
    if (selector === "button, input, textarea, select") {
      return this.nodes.filter((node) => ["BUTTON", "INPUT", "TEXTAREA", "SELECT"].includes(node.tagName));
    }
    return [];
  }
}

class FakeStorage {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.has(String(key)) ? this.values.get(String(key)) : null; }
  setItem(key, value) { this.values.set(String(key), String(value)); }
  removeItem(key) { this.values.delete(String(key)); }
}

class FailingAnswerStorage extends FakeStorage {
  constructor() {
    super();
    this.failAnswers = false;
  }

  setItem(key, value) {
    if (this.failAnswers && String(key).endsWith(":answers")) {
      throw new Error("simulated answer-map storage failure");
    }
    super.setItem(key, value);
  }
}

class FakeWebView {
  constructor() {
    this.messages = [];
    this.listeners = Object.create(null);
  }
  postMessage(message) { this.messages.push(JSON.parse(JSON.stringify(message))); }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  emit(data) {
    for (const listener of this.listeners.message || []) listener({ data });
  }
}

class BrowserURL extends globalThis.URL {
  static createObjectURL() { return "blob:test"; }
  static revokeObjectURL() {}
}

function contextFor(candidateId, clientIdHex = HEX16, sessionIdHex = HEX16_B,
                    synchronizedStart = false) {
  return {
    packageId: MANIFEST.id,
    packageDigestHex: HEX32_A,
    clientIdHex,
    sessionIdHex,
    candidateId,
    nextSequence: 1,
    synchronizedStart
  };
}

function contextKey(context) {
  return JSON.stringify([
    context.packageId, context.packageDigestHex, context.clientIdHex,
    context.sessionIdHex, context.candidateId
  ]);
}

function storageKey(context) {
  return `nstu-exam:${encodeURIComponent(MANIFEST.id)}:context:${encodeURIComponent(contextKey(context))}`;
}

function seedDurable(storage, context, values = {}) {
  const durable = {
    nextSequence: 1,
    previousEventHashHex: "",
    serverHighestSequence: 0,
    serverLastEventHashHex: "",
    serverStateHashHex: "",
    serverFinalized: false,
    pending: [],
    drafts: {},
    contextKey: contextKey(context),
    quarantined: [],
    finalizeRequested: false,
    ...values
  };
  storage.setItem(`${storageKey(context)}:durable-v1`, JSON.stringify(durable));
  return durable;
}

function seedAnswers(storage, context, answer, notes = "") {
  storage.setItem(`${storageKey(context)}:answers`, JSON.stringify({ q1: answer }));
  storage.setItem(`${storageKey(context)}:notes`, notes);
}

function createHarness(initialContext, prepare, localStorage = new FakeStorage(), options = {}) {
  const document = new FakeDocument();
  if (prepare) prepare(localStorage);
  const webview = new FakeWebView();
  let timerId = 0;
  const timers = new Map();
  let now = Number.isSafeInteger(options.now) ? options.now : 1_700_000_000_000;
  let perfNow = Object.prototype.hasOwnProperty.call(options, "perfNow") ? options.perfNow : 1000;
  const intervalCallbacks = [];
  const windowListeners = Object.create(null);
  const window = {
    NSTU_EXAM_MANIFEST: options.manifest || MANIFEST,
    NSTU_EXAM_CONTEXT: initialContext,
    chrome: options.hosted === false ? undefined : { webview },
    crypto: { randomUUID: () => "00000000-0000-4000-8000-000000000000" },
    performance: { now: () => perfNow },
    addEventListener: (type, listener) => {
      (windowListeners[type] ||= []).push(listener);
    },
    dispatchEvent: (event) => {
      const current = event || {};
      for (const listener of windowListeners[current.type] || []) {
        listener.call(window, current);
      }
      return true;
    },
    setTimeout: (callback, delay) => {
      const id = ++timerId;
      timers.set(id, { callback, delay });
      return id;
    },
    clearTimeout: (id) => { timers.delete(id); },
    clearInterval: (id) => { if (intervalCallbacks[id - 1]) intervalCallbacks[id - 1].callback = () => {}; },
    setInterval: (callback, delay) => {
      intervalCallbacks.push({ callback, delay });
      return intervalCallbacks.length;
    }
  };
  if (Object.prototype.hasOwnProperty.call(options, "assetBase")) {
    window.NSTU_EXAM_ASSET_BASE = options.assetBase;
  }
  const context = vm.createContext({
    window,
    document,
    localStorage,
    TextEncoder,
    Uint8Array,
    Blob: class Blob {},
    URL: options.urlConstructor ? BrowserURL :
      { createObjectURL: () => "blob:test", revokeObjectURL: () => {} },
    Date: class extends Date {
      constructor(...args) { super(...(args.length ? args : [now])); }
      static now() { return now; }
    },
    console
  });
  // Expose owning-layer state only inside this VM, never in the shipped page.
  const source = options.captureInspection ? APP_SOURCE.replace(/\}\(\)\);\s*$/, `
    window.captureInspection = { telemetry, emitTelemetrySample, flushTelemetry,
      resetTelemetryState, saveAnswer, activateQuestion };
  }());`) : APP_SOURCE;
  vm.runInContext(source, context, { filename: "exam/web/app.js" });
  return {
    document, localStorage, webview, window, timers, intervalCallbacks,
    now: () => now,
    perfNow: () => perfNow,
    setNow: (value) => { now = value; },
    setPerfNow: (value) => { perfNow = value; },
    advancePerf: (ms) => { perfNow += ms; now += ms; }
  };
}

function stateResponse(context, highest, lastEventHashHex, stateHashHex, finalized = false, answers = []) {
  return {
    type: "exam_state_response",
    response: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      highestContiguousSequence: highest,
      lastEventHashHex: highest ? lastEventHashHex : "",
      stateHashHex: highest ? stateHashHex : "",
      finalized,
      chunkIndex: 0,
      chunkCount: 1,
      answers
    }
  };
}

function chunkResponse(context, chunkIndex, chunkCount, answers) {
  return {
    type: "exam_state_response",
    response: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      highestContiguousSequence: 1,
      lastEventHashHex: HEX32_B,
      stateHashHex: HEX32_C,
      finalized: false,
      chunkIndex,
      chunkCount,
      answers
    }
  };
}

function runTimer(harness, predicate) {
  const entry = [...harness.timers.entries()].find(([, value]) => predicate(value));
  assert.ok(entry, "the expected browser timer should be scheduled");
  harness.timers.delete(entry[0]);
  entry[1].callback();
}

function latestAnswerInput(document) {
  const inputs = document.nodes.filter((node) => node.className === "answer-input");
  assert.ok(inputs.length > 0, "the exam should render an answer input");
  return inputs[inputs.length - 1];
}

function testStaleWatermarkIsIgnored() {
  const context = contextFor("stale-candidate");
  const harness = createHarness(context, (storage) => {
    seedDurable(storage, context, {
      nextSequence: 6,
      previousEventHashHex: HEX32_B,
      serverHighestSequence: 5,
      serverLastEventHashHex: HEX32_B,
      serverStateHashHex: HEX32_C
    });
  });
  const before = JSON.parse(harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  harness.webview.emit(stateResponse(context, 4, HEX32_D, HEX32_A));
  const after = JSON.parse(harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.deepEqual(after, before, "a lower server watermark must not rewind durable state");
  assert.equal(harness.document.getElementById("connection-status").textContent,
    "Waiting for server recovery state");
}

function testFinalizedResponseDoesNotRequeueFinalize() {
  const context = contextFor("finalize-candidate");
  const harness = createHarness(context);
  // Complete the initial authoritative-state handshake before submitting.
  harness.webview.emit(stateResponse(context, 0, "", ""));
  harness.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  const events = () => harness.webview.messages.filter((message) => message.type === "exam_answer_event");
  assert.equal(events().length, 1, "submission should enqueue one finalization event");
  const finalEvent = events()[0].event;
  assert.equal(finalEvent.kind, "finalize");
  harness.webview.emit(stateResponse(context, 1, finalEvent.eventHashHex, HEX32_D, true));
  const durable = JSON.parse(harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.pending.length, 0, "the acknowledged finalization must leave no pending event");
  assert.equal(durable.serverFinalized, true);
  assert.equal(harness.document.getElementById("save-status").textContent, "Response package prepared.");
  harness.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  assert.equal(events().length, 1, "a finalized response must not enqueue another finalization");
}

function testContextSwitchPreservesTargetNamespace() {
  const contextA = contextFor("candidate-a");
  const contextB = contextFor("candidate-b", HEX16_B, HEX16);
  const harness = createHarness(contextA, (storage) => {
    seedAnswers(storage, contextA, "answer-a", "notes-a");
    seedAnswers(storage, contextB, "answer-b", "notes-b");
    seedDurable(storage, contextA);
    seedDurable(storage, contextB);
  });
  assert.equal(latestAnswerInput(harness.document).value, "answer-a");
  harness.webview.emit({ type: "exam_context", context: contextB });
  assert.equal(latestAnswerInput(harness.document).value, "answer-b",
    "switching context must load the target namespace");
  assert.equal(harness.localStorage.getItem(`${storageKey(contextB)}:answers`),
    JSON.stringify({ q1: "answer-b" }), "the target namespace must remain intact");
  const targetDurable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(contextB)}:durable-v1`));
  assert.equal(targetDurable.contextKey, contextKey(contextB));
  assert.equal(JSON.stringify(targetDurable).includes("answer-a"), false,
    "the active namespace must not contain prior-context answer data");
  assert.equal(harness.localStorage.getItem(`${storageKey(contextA)}:answers`),
    JSON.stringify({ q1: "answer-a" }),
    "the prior namespace must remain isolated and recoverable");
  harness.webview.emit({ type: "exam_context", context: contextA });
  assert.equal(latestAnswerInput(harness.document).value, "answer-a",
    "returning to a context must recover only that context's answer");
}

function testContextSwitchRejectsForeignPackage() {
  const context = contextFor("package-bound-candidate");
  const foreignContext = { ...context, packageId: "different-exam-package" };
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "local-answer", "local-notes");
    seedDurable(storage, context);
  });
  assert.equal(latestAnswerInput(harness.document).value, "local-answer");
  harness.webview.emit({ type: "exam_context", context: foreignContext });
  assert.equal(latestAnswerInput(harness.document).value, "local-answer",
    "a context message for another package must not replace the active context");
  assert.equal(harness.localStorage.getItem(`${storageKey(foreignContext)}:answers`),
    null, "a rejected package context must not load a foreign storage namespace");
  assert.equal(harness.document.getElementById("connection-status").textContent,
    "Exam context does not match this package");
}

function testInitialForeignPackageContextCannotLoadStorage() {
  const validContext = contextFor("initial-package-boundary");
  const foreignContext = { ...validContext, packageId: "foreign-initial-package" };
  const foreignStorageKey = storageKey(foreignContext);
  const storage = new FakeStorage();
  seedAnswers(storage, foreignContext, "must-not-load");
  seedDurable(storage, foreignContext, {
    contextKey: contextKey(foreignContext),
    finalizeRequested: true
  });
  const harness = createHarness(foreignContext, null, storage);
  assert.equal(latestAnswerInput(harness.document).value, "",
    "an initial foreign package context must not expose its answer map");
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, false,
    "an unbound initial context must not inherit a foreign finalization marker");
  assert.equal(harness.localStorage.getItem(`${foreignStorageKey}:answers`),
    JSON.stringify({ q1: "must-not-load" }),
    "foreign storage must remain untouched when the initial context is rejected");
}

function testLocalFinalizationLocksDynamicControls() {
  const context = contextFor("locally-finalized");
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "answer", "notes");
    seedDurable(storage, context, { finalizeRequested: true });
  });
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, true,
    "a durable finalization marker must lock the submit control after reload");
  assert.equal(latestAnswerInput(harness.document).disabled, true,
    "a durable finalization marker must lock dynamically rendered answers");
}

function testContextSwitchRestoresEditableControls() {
  const contextA = contextFor("locked-context");
  const contextB = contextFor("editable-context", HEX16_B, HEX16);
  const harness = createHarness(contextA, (storage) => {
    seedDurable(storage, contextA, { finalizeRequested: true });
    seedDurable(storage, contextB);
  });
  assert.equal(latestAnswerInput(harness.document).disabled, true,
    "the initially finalized context must be locked");
  harness.webview.emit({ type: "exam_context", context: contextB });
  assert.equal(latestAnswerInput(harness.document).disabled, false,
    "switching to an editable context must re-enable answer controls");
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, false,
    "switching to an editable context must re-enable static controls");
  assert.equal(harness.document.querySelector('[data-action="previous"]').disabled, true,
    "the first question must still disable previous navigation");
}

function testMismatchedPendingFinalizationDoesNotLockContext() {
  const staleContext = contextFor("stale-finalized");
  const activeContext = contextFor("active-editable", HEX16_B, HEX16);
  const source = createHarness(staleContext);
  source.webview.emit(stateResponse(staleContext, 0, "", ""));
  source.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  const staleFinalization = source.webview.messages.find((message) =>
    message.type === "exam_answer_event" && message.event.kind === "finalize");
  assert.ok(staleFinalization, "the fixture must contain a valid finalization event");

  const harness = createHarness(activeContext, (storage) => {
    seedDurable(storage, activeContext, {
      contextKey: contextKey(staleContext),
      pending: [staleFinalization.event],
      finalizeRequested: true,
      serverFinalized: true,
      serverHighestSequence: staleFinalization.event.sequence,
      serverLastEventHashHex: staleFinalization.event.eventHashHex,
      serverStateHashHex: HEX32_B
    });
  });
  assert.equal(latestAnswerInput(harness.document).disabled, false,
    "a mismatched finalization must not lock the active context");
  assert.equal(harness.document.querySelector('[data-action="submit"]').disabled, false,
    "the active context must remain submittable after quarantine");
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(activeContext)}:durable-v1`));
  assert.equal(durable.contextKey, contextKey(activeContext));
  assert.equal(durable.pending.length, 0,
    "mismatched finalization events must be quarantined");
  assert.equal(durable.finalizeRequested, false);
  assert.equal(durable.serverFinalized, false);
  assert.ok(durable.quarantined.some((entry) => entry.reason === "context-mismatch"),
    "quarantine evidence should retain bounded context-mismatch metadata");
}

function testContextSwitchQuarantinesStaleTargetNamespace() {
  const contextA = contextFor("switch-source");
  const contextB = contextFor("switch-target", HEX16_B, HEX16);
  const staleContext = contextFor("stale-target", HEX16, HEX16_B);
  const source = createHarness(staleContext);
  source.webview.emit(stateResponse(staleContext, 0, "", ""));
  source.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });
  const staleFinalization = source.webview.messages.find((message) =>
    message.type === "exam_answer_event" && message.event.kind === "finalize");
  assert.ok(staleFinalization, "the stale namespace fixture must contain a finalization event");

  const harness = createHarness(contextA, (storage) => {
    seedDurable(storage, contextA);
    seedDurable(storage, contextB, {
      contextKey: contextKey(staleContext),
      pending: [staleFinalization.event],
      finalizeRequested: true,
      serverFinalized: true,
      serverHighestSequence: staleFinalization.event.sequence,
      serverLastEventHashHex: staleFinalization.event.eventHashHex,
      serverStateHashHex: HEX32_C
    });
  });
  harness.webview.emit({ type: "exam_context", context: contextB });
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(contextB)}:durable-v1`));
  assert.equal(durable.pending.length, 0,
    "a stale target namespace must be quarantined before it can be activated");
  assert.equal(durable.finalizeRequested, false);
  assert.equal(durable.serverFinalized, false);
  assert.ok(durable.quarantined.some((entry) => entry.reason === "context-mismatch"),
    "switching into a stale namespace must retain quarantine evidence");
  assert.equal(latestAnswerInput(harness.document).disabled, false,
    "a stale target finalization must not lock the new context");
}

function testMalformedStoredClearIsQuarantined() {
  const context = contextFor("malformed-clear");
  const malformed = {
    packageId: MANIFEST.id,
    packageDigestHex: context.packageDigestHex,
    clientIdHex: context.clientIdHex,
    sessionIdHex: context.sessionIdHex,
    candidateId: context.candidateId,
    questionId: "q1",
    questionRevision: 1,
    sequence: 1,
    clientTimeUnixMilliseconds: 1,
    kind: "clear",
    answer: "must-be-empty",
    eventHashHex: HEX32_A,
    previousEventHashHex: ""
  };
  const harness = createHarness(context, (storage) => {
    seedDurable(storage, context, { pending: [malformed] });
  });
  // A state checkpoint persists the in-memory quarantine result back to the
  // browser namespace; loading alone intentionally does not rewrite storage.
  harness.webview.emit(stateResponse(context, 0, "", ""));
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.pending.length, 0,
    "a stored clear event with answer bytes must never enter the retry queue");
  assert.ok(durable.quarantined.some((entry) => entry.reason === "invalid"),
    "malformed clear data should produce bounded quarantine evidence");
}

function testZeroWatermarkRejectsStateHash() {
  const context = contextFor("zero-watermark-state-hash");
  const harness = createHarness(context);
  const malformed = stateResponse(context, 0, "", "");
  malformed.response.stateHashHex = HEX32_A;
  harness.webview.emit(malformed);
  assert.notEqual(
    harness.document.getElementById("connection-status").textContent,
    "Server recovery state does not match pending answers.",
    "a zero watermark with a nonzero state hash must be rejected before reconciliation");
  harness.webview.emit(stateResponse(context, 0, "", ""));
  assert.equal(
    harness.document.getElementById("connection-status").textContent,
    "Server recovery ready",
    "a valid zero-watermark response must still complete reconciliation");
}

function testRejectedAckSchedulesRetry() {
  const context = contextFor("rejected-retry");
  const harness = createHarness(context);
  harness.webview.emit(stateResponse(context, 0, "", ""));
  const input = latestAnswerInput(harness.document);
  input.value = "answer";
  input.dispatchEvent({ type: "input" });
  runTimer(harness, (timer) => timer.delay === 350);
  const eventMessage = harness.webview.messages.find((message) =>
    message.type === "exam_answer_event");
  assert.ok(eventMessage, "the draft should be sent after the debounce timer");
  const event = eventMessage.event;
  harness.webview.emit({
    type: "exam_answer_ack",
    ack: {
      status: "rejected",
      sessionIdHex: context.sessionIdHex,
      sequence: event.sequence,
      eventHashHex: event.eventHashHex
    }
  });
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.pending.length, 1,
    "a rejected event must remain durable for retry");
  assert.ok([...harness.timers.values()].some((timer) => timer.delay >= 1000),
    "a rejected ACK must schedule a bounded retry");
}

function testEquivalentChunkEncodingsAreNotConflicts() {
  const context = contextFor("chunk-normalization");
  const harness = createHarness(context);
  const answerCamel = {
    questionId: "q1",
    questionRevision: 1,
    sequence: 1,
    kind: "upsert",
    answer: "server-answer",
    eventHashHex: HEX32_A
  };
  const answerSnake = {
    question_id: "q1",
    question_revision: 1,
    sequence: 1,
    kind: "upsert",
    answer: "server-answer",
    event_hash_hex: HEX32_A
  };
  harness.webview.emit(chunkResponse(context, 0, 2, [answerCamel]));
  harness.webview.emit(chunkResponse(context, 0, 2, [answerSnake]));
  harness.webview.emit(chunkResponse(context, 1, 2, []));
  assert.equal(latestAnswerInput(harness.document).value, "server-answer",
    "equivalent camelCase and snake_case chunks must reassemble identically");
  assert.notEqual(harness.document.getElementById("connection-status").textContent,
    "Stale or conflicting server recovery state.");
}

function testPendingDraftWinsOverAuthoritativeCheckpoint() {
  const context = contextFor("pending-precedence");
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "local-newer");
    seedDurable(storage, context, {
      nextSequence: 2,
      drafts: {
        q1: {
          value: "local-newer",
          questionRevision: 1,
          contextKey: contextKey(context)
        }
      }
    });
  });
  const serverAnswer = {
    questionId: "q1",
    questionRevision: 1,
    sequence: 1,
    kind: "upsert",
    answer: "server-checkpoint",
    eventHashHex: HEX32_B
  };
  harness.webview.emit(stateResponse(
    context, 1, HEX32_B, HEX32_C, false, [serverAnswer]));
  assert.equal(latestAnswerInput(harness.document).value, "local-newer",
    "a newer local draft must remain visible over an older server checkpoint");
}

function testAnswerMapFailureLeavesDurableDraft() {
  const context = contextFor("answer-map-failure");
  const storage = new FailingAnswerStorage();
  const harness = createHarness(context, null, storage);
  harness.webview.emit(stateResponse(context, 0, "", ""));
  storage.failAnswers = true;
  const input = latestAnswerInput(harness.document);
  input.value = "durable-draft";
  input.dispatchEvent({ type: "input" });
  const durable = JSON.parse(
    harness.localStorage.getItem(`${storageKey(context)}:durable-v1`));
  assert.equal(durable.drafts.q1.value, "durable-draft",
    "a failed answer-map write must retain a durable recovery draft");
  assert.equal(harness.document.getElementById("save-status").textContent,
    "Not saved", "the UI must not claim that a failed write was saved");
}

const ASSET_MANIFEST = {
  id: "asset-resolution-test",
  title: "Asset resolution test",
  subject: "Test",
  durationSeconds: 600,
  documents: [],
  questions: [
    {
      id: "audio",
      type: "listening",
      points: 1,
      prompt: "Listen",
      audio: "media/listening-01.mp3",
      options: ["A", "B"]
    },
    {
      id: "pdf",
      type: "reading",
      points: 1,
      prompt: "Read",
      pdf: "documents/reference.pdf",
      options: ["A", "B"]
    },
    {
      id: "external",
      type: "listening",
      points: 1,
      prompt: "Reject external asset",
      audio: "https://example.invalid/outside.mp3",
      options: ["A", "B"]
    },
    {
      id: "traversal",
      type: "listening",
      points: 1,
      prompt: "Reject traversal",
      audio: "../outside.mp3",
      options: ["A", "B"]
    },
    {
      id: "query",
      type: "listening",
      points: 1,
      prompt: "Reject query suffix",
      audio: "media/listening-01.mp3?download=1",
      options: ["A", "B"]
    },
    {
      id: "fragment",
      type: "listening",
      points: 1,
      prompt: "Reject fragment suffix",
      audio: "media/listening-01.mp3#fragment",
      options: ["A", "B"]
    },
    {
      id: "encoded-fragment",
      type: "listening",
      points: 1,
      prompt: "Reject encoded fragment suffix",
      audio: "media/listening-01.mp3%23fragment",
      options: ["A", "B"]
    }
  ]
};

function moveToQuestion(harness, index) {
  const next = harness.document.querySelector('[data-action="next"]');
  for (let step = 0; step < index; step += 1) next.dispatchEvent({ type: "click" });
}

function latestNode(document, tagName) {
  const nodes = document.nodes.filter((node) => node.tagName === tagName.toUpperCase());
  assert.ok(nodes.length > 0, `expected a ${tagName} element`);
  return nodes[nodes.length - 1];
}

function testInjectedAssetBaseResolvesLocalMedia() {
  const context = contextFor("asset-base");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/"
  });
  assert.equal(latestNode(harness.document, "audio").src,
    "https://nstu.exam/media/listening-01.mp3",
    "relative audio must resolve through the injected virtual host");
  moveToQuestion(harness, 1);
  assert.equal(harness.document.getElementById("pdf-frame").src,
    "https://nstu.exam/documents/reference.pdf",
    "relative PDF paths must resolve through the injected virtual host");
}

function testInjectedAssetBaseUsesBrowserUrlResolution() {
  const context = contextFor("asset-url-constructor");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/",
    urlConstructor: true
  });
  assert.equal(latestNode(harness.document, "audio").src,
    "https://nstu.exam/media/listening-01.mp3",
    "the browser URL branch must resolve package-relative media");
}

function testInjectedAssetBaseRejectsExternalAndTraversal() {
  const context = contextFor("asset-rejection");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/"
  });
  moveToQuestion(harness, 2);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "external audio must not be loaded");
  moveToQuestion(harness, 1);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "package traversal audio must not be loaded");
}

function testInjectedAssetBaseRejectsQueryAndFragmentSuffixes() {
  const context = contextFor("asset-suffix-rejection");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST,
    assetBase: "https://nstu.exam/"
  });
  moveToQuestion(harness, 4);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "query-bearing package assets must not be loaded");
  moveToQuestion(harness, 1);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "fragment-bearing package assets must not be loaded");
  moveToQuestion(harness, 1);
  assert.equal(latestNode(harness.document, "audio").src, "",
    "encoded fragment package assets must not be loaded");
}

function testStaticPreviewKeepsRelativeAssetPaths() {
  const context = contextFor("asset-static-preview");
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: ASSET_MANIFEST
  });
  assert.equal(latestNode(harness.document, "audio").src, "media/listening-01.mp3",
    "static preview should retain package-relative media paths");
}

function testExpiredTimerDoesNotResetAfterReload() {
  const context = contextFor("expired-timer");
  const storage = new FakeStorage();
  storage.setItem(`${storageKey(context)}:remaining`, "0");
  const harness = createHarness(context, null, storage, { hosted: false });
  assert.equal(harness.document.getElementById("timer").textContent, "00:00",
    "an expired persisted timer must remain expired after reload");
}

function testHostedExamWaitsForAuthenticatedBegin() {
  const context = contextFor("barrier-candidate", HEX16, HEX16_B, true);
  const startedAt = 1_700_000_000_000;
  const harness = createHarness(context, null, new FakeStorage(), {
    now: startedAt
  });
  assert.equal(harness.document.getElementById("loading-screen").hidden, false,
    "the hosted exam must show a loading screen before the cohort begins");
  assert.equal(harness.document.getElementById("exam-shell").hidden, true,
    "questions must stay hidden before an authenticated begin");
  assert.equal(harness.document.getElementById("timer").textContent, "10:00",
    "waiting must not consume exam time");
  const ready = harness.webview.messages.find((message) => message.type === "exam_ready");
  assert.ok(ready, "the loaded package must report ready to the native host");

  harness.setNow(startedAt + 5_000);
  for (const timer of harness.intervalCallbacks) timer.callback();
  assert.equal(harness.document.getElementById("timer").textContent, "10:00",
    "the countdown must remain frozen while waiting for the cohort");

  harness.webview.emit({
    type: "exam_begin",
    begin: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: "ff".repeat(16),
      candidateId: context.candidateId,
      serverStartUnixMilliseconds: startedAt,
      durationSeconds: 300
    }
  });
  assert.equal(harness.document.getElementById("exam-shell").hidden, true,
    "a begin for another session must not reveal the questions");

  harness.webview.emit({
    type: "exam_begin",
    begin: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      serverStartUnixMilliseconds: startedAt,
      durationSeconds: 300
    }
  });
  assert.equal(harness.document.getElementById("loading-screen").hidden, true);
  assert.equal(harness.document.getElementById("exam-shell").hidden, false,
    "a context-matching begin must reveal the questions");
  assert.equal(harness.document.getElementById("timer").textContent, "04:55",
    "remaining time must be anchored to the authoritative server start");
}

function testMalformedManifestDoesNotCrashInitialRender() {
  const context = contextFor("malformed-manifest");
  const malformed = {
    ...MANIFEST,
    questions: null,
    durationSeconds: "not-a-duration"
  };
  const harness = createHarness(context, null, new FakeStorage(), {
    manifest: malformed
  });
  assert.equal(harness.document.getElementById("timer").textContent, "45:00",
    "an invalid duration must fall back to the safe default");
  assert.equal(harness.document.getElementById("question-list").children.length, 0,
    "a malformed question list must render as an empty, usable page");
  assert.equal(harness.document.getElementById("question-content").textContent, "",
    "malformed question data must not produce a partial question render");
}

const MULTI_MANIFEST = {
  id: "exam-web-recovery-test",
  title: "Multi question test",
  subject: "Test",
  durationSeconds: 1800,
  documents: [],
  questions: [
    { id: "q1", type: "short_answer", points: 1, prompt: "Q1" },
    { id: "q2", type: "multiple_choice", points: 1, prompt: "Q2", options: ["A", "B"] },
    { id: "q3", type: "essay", points: 5, prompt: "Q3" }
  ]
};

function assertTelemetryPrivacy(samples) {
  const allowedKeys = new Set([
    "kind", "source", "collectorId", "questionId", "questionRevision",
    "monotonicElapsedMilliseconds", "durationMilliseconds",
    "clientTimeUnixMilliseconds", "numericValue", "count", "reason"
  ]);
  const forbiddenSubstrings = ["must-be-empty", "secret", "private note", "essay draft", "answer text"];
  for (const sample of samples) {
    for (const key of Object.keys(sample)) {
      assert.ok(allowedKeys.has(key), `forbidden key in sample: ${key}`);
    }
    assert.equal(sample.source, "browser");
    const json = JSON.stringify(sample);
    for (const bad of forbiddenSubstrings) {
      assert.ok(!json.includes(bad), `sample must not leak forbidden data: ${bad}`);
    }
  }
}

function testTelemetryDisabledByDefaultAndEnableLifecycle() {
  const context = contextFor("telem-candidate");
  const harness = createHarness(context, null, new FakeStorage(), { manifest: MULTI_MANIFEST });

  // 1. Telemetry is disabled by default: actions produce no telemetry messages
  const q1Input = latestAnswerInput(harness.document);
  q1Input.value = "initial";
  q1Input.dispatchEvent({ type: "input" });
  harness.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const initialTelem = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  assert.equal(initialTelem.length, 0, "telemetry must be disabled by default");

  // 2. Enable message arrives from host
  harness.webview.emit({
    type: "exam_telemetry_enable",
    enable: {
      packageId: MULTI_MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      enabled: true,
      collectorId: 42, collectorElapsedMilliseconds: 0,
      samplingIntervalMs: 30000,
      maxBatchSamples: 32
    }
  });

  const enabledTelem = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  assert.ok(enabledTelem.length > 0, "enabling telemetry must produce telemetry samples");
  const firstBatch = enabledTelem[0].samples;
  assert.ok(Array.isArray(firstBatch) && firstBatch.length > 0, "must emit initial samples");
  assert.equal(firstBatch[0].kind, "collector_started", "first sample must be collector_started");
  assert.equal(firstBatch[0].collectorId, 42);
  assertTelemetryPrivacy(firstBatch);
}

function testTelemetryNavigationAndDwell() {
  const context = contextFor("telem-nav");
  const harness = createHarness(context, null, new FakeStorage(), { manifest: MULTI_MANIFEST });

  harness.webview.emit({
    type: "exam_telemetry_enable",
    enable: {
      packageId: MULTI_MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      enabled: true,
      collectorId: 101, collectorElapsedMilliseconds: 0,
      samplingIntervalMs: 30000,
      maxBatchSamples: 32
    }
  });

  // Clear startup messages
  harness.webview.messages.length = 0;

  // Dwell on q1 for 3000ms
  harness.advancePerf(3000);

  // Navigate to q2 via Next button
  const nextBtn = harness.document.querySelector('[data-action="next"]');
  nextBtn.dispatchEvent({ type: "click" });

  const telemMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  assert.ok(telemMsgs.length > 0, "navigation must flush telemetry");
  const allSamples = telemMsgs.flatMap((m) => m.samples);

  // Must have closed dwell for q1 and first view for q2
  const q1Dwell = allSamples.find((s) => s.kind === "question_dwell" && s.questionId === "q1");
  assert.ok(q1Dwell, "must emit question_dwell for q1");
  assert.ok(q1Dwell.durationMilliseconds >= 3000, "dwell duration must be >= 3000ms");

  const q2FirstView = allSamples.find((s) => s.kind === "question_first_view" && s.questionId === "q2");
  assert.ok(q2FirstView, "must emit question_first_view for q2");
  assert.equal(q2FirstView.reason, "navigation_button");

  // Rerendering question list or clicking active question button should NOT emit another first_view
  harness.webview.messages.length = 0;
  const qButtons = harness.document.getElementById("question-list").children;
  // Button at index 1 is q2 (currently active)
  qButtons[1].dispatchEvent({ type: "click" });
  const rerenderTelem = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  assert.equal(rerenderTelem.length, 0, "rerendering active question must not emit new view or dwell");

  // Navigate back to q1 via ArrowLeft key
  harness.advancePerf(2000);
  harness.document.dispatchEvent({ type: "keydown", key: "ArrowLeft" });

  const arrowMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const arrowSamples = arrowMsgs.flatMap((m) => m.samples);
  const q2Dwell = arrowSamples.find((s) => s.kind === "question_dwell" && s.questionId === "q2");
  assert.ok(q2Dwell, "must emit question_dwell for q2");
  assert.ok(q2Dwell.durationMilliseconds >= 2000);

  // q1 was already viewed, so returning to q1 must NOT emit question_first_view
  const q1ReView = arrowSamples.find((s) => s.kind === "question_first_view" && s.questionId === "q1");
  assert.equal(q1ReView, undefined, "returning to previously viewed question must not emit question_first_view");

  assertTelemetryPrivacy(allSamples.concat(arrowSamples));
}

function testTelemetryLateEnableAndSilentlyBaselines() {
  const context = contextFor("telem-late");
  const harness = createHarness(context, (storage) => {
    seedAnswers(storage, context, "pre-existing answer", "pre-existing notes");
  }, new FakeStorage(), { manifest: MULTI_MANIFEST });

  // Move to question 2 before enable
  harness.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });

  // Late enable
  harness.webview.emit({
    type: "exam_telemetry_enable",
    enable: {
      packageId: MULTI_MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      enabled: true,
      collectorId: 55, collectorElapsedMilliseconds: 0,
      samplingIntervalMs: 30000,
      maxBatchSamples: 32
    }
  });

  const telemMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  assert.ok(telemMsgs.length > 0);
  const samples = telemMsgs.flatMap((m) => m.samples);

  const startSample = samples.find((s) => s.kind === "collector_started");
  assert.ok(startSample);
  assert.equal(startSample.reason, "collector_late_enable", "must be collector_late_enable");

  // Pre-existing answer for q1 must NOT generate a question_first_answer or change event
  const retroactiveAnswer = samples.find((s) => s.kind === "question_first_answer" && s.questionId === "q1");
  assert.equal(retroactiveAnswer, undefined, "late enable must silently baseline pre-existing answers");

  assertTelemetryPrivacy(samples);
}

function testTelemetryFocusAndVisibility() {
  const context = contextFor("telem-focus");
  const harness = createHarness(context, null, new FakeStorage(), { manifest: MULTI_MANIFEST });

  harness.webview.emit({
    type: "exam_telemetry_enable",
    enable: {
      packageId: MULTI_MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      enabled: true,
      collectorId: 77, collectorElapsedMilliseconds: 0,
      samplingIntervalMs: 30000,
      maxBatchSamples: 32
    }
  });

  harness.webview.messages.length = 0;
  harness.advancePerf(1000);

  // Window loses focus (blur)
  harness.document.hasFocus = () => false;
  harness.window.dispatchEvent({ type: "blur" });

  const blurMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const blurSamples = blurMsgs.flatMap((m) => m.samples);

  const blurFocus = blurSamples.find((s) => s.kind === "focus_changed" && s.reason === "focus_blur");
  assert.ok(blurFocus, "must emit focus_changed blur");
  assert.equal(blurFocus.numericValue, 0);

  const dwellBeforeBlur = blurSamples.find((s) => s.kind === "question_dwell");
  assert.ok(dwellBeforeBlur, "blur must close active dwell segment");
  assert.ok(dwellBeforeBlur.durationMilliseconds >= 1000);

  // Time elapses while blurred: 5000ms
  harness.webview.messages.length = 0;
  harness.advancePerf(5000);

  // Window regains focus
  harness.document.hasFocus = () => true;
  harness.window.dispatchEvent({ type: "focus" });

  const focusMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const focusSamples = focusMsgs.flatMap((m) => m.samples);
  const gainFocus = focusSamples.find((s) => s.kind === "focus_changed" && s.reason === "focus_gain");
  assert.ok(gainFocus, "must emit focus_changed gain");
  assert.equal(gainFocus.numericValue, 1);

  // Dwell while focused for 2000ms
  harness.webview.messages.length = 0;
  harness.advancePerf(2000);

  // Visibility hidden
  harness.document.hidden = true;
  harness.document.dispatchEvent({ type: "visibilitychange" });

  const hiddenMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const hiddenSamples = hiddenMsgs.flatMap((m) => m.samples);
  const visHidden = hiddenSamples.find((s) => s.kind === "visibility_changed" && s.reason === "visibility_hidden");
  assert.ok(visHidden, "must emit visibility_changed hidden");
  assert.equal(visHidden.numericValue, 0);

  const dwellBeforeHide = hiddenSamples.find((s) => s.kind === "question_dwell");
  assert.ok(dwellBeforeHide, "hidden must close active dwell");
  assert.ok(dwellBeforeHide.durationMilliseconds >= 2000);
  // Must NOT include the 5000ms blurred time
  assert.ok(dwellBeforeHide.durationMilliseconds < 4500, "dwell must exclude blurred interval");

  assertTelemetryPrivacy(blurSamples.concat(focusSamples, hiddenSamples));
}

function testTelemetryImeAndInputNormalization() {
  const context = contextFor("telem-input");
  const harness = createHarness(context, null, new FakeStorage(), { manifest: MULTI_MANIFEST });

  harness.webview.emit({
    type: "exam_telemetry_enable",
    enable: {
      packageId: MULTI_MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      enabled: true,
      collectorId: 88, collectorElapsedMilliseconds: 0,
      samplingIntervalMs: 30000,
      maxBatchSamples: 32
    }
  });

  harness.webview.messages.length = 0;
  const input = latestAnswerInput(harness.document);

  // 1. IME composition start -> typing during composition must NOT emit telemetry changes
  input.dispatchEvent({ type: "compositionstart" });
  input.value = "comp";
  input.dispatchEvent({ type: "input" });

  const imeDuring = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  assert.equal(imeDuring.length, 0, "typing during IME composition must not emit telemetry samples");

  // 2. IME composition end -> commits final normalized value
  input.value = "completed word";
  input.dispatchEvent({ type: "compositionend" });

  // Flush on next navigation or tick
  harness.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const imeEndMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const samples = imeEndMsgs.flatMap((m) => m.samples);

  const firstAnswer = samples.find((s) => s.kind === "question_first_answer" && s.questionId === "q1");
  assert.ok(firstAnswer, "must emit question_first_answer on first answer commit");

  const changeSummary = samples.find((s) => s.kind === "answer_change_summary" && s.questionId === "q1");
  assert.ok(changeSummary, "must emit answer_change_summary");
  assert.equal(changeSummary.count, 1, "first commit counts as 1 change");

  // 3. Identical input does not increase change count
  harness.webview.messages.length = 0;
  // Return to q1
  harness.document.querySelector('[data-action="previous"]').dispatchEvent({ type: "click" });
  harness.webview.messages.length = 0;

  const inputQ1Again = latestAnswerInput(harness.document);
  inputQ1Again.value = "completed word"; // exact same value
  inputQ1Again.dispatchEvent({ type: "input" });

  harness.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const sameInputMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const sameInputSamples = sameInputMsgs.flatMap((m) => m.samples);
  const redundantChange = sameInputSamples.find((s) => s.kind === "answer_change_summary" && s.questionId === "q1");
  assert.equal(redundantChange, undefined, "identical answer input must not produce change sample");

  assertTelemetryPrivacy(samples);
}

function testTelemetryClockDiscontinuityAndSubmission() {
  const context = contextFor("telem-disc");
  const harness = createHarness(context, null, new FakeStorage(), { manifest: MULTI_MANIFEST });

  harness.webview.emit({
    type: "exam_telemetry_enable",
    enable: {
      packageId: MANIFEST.id,
      packageDigestHex: context.packageDigestHex,
      clientIdHex: context.clientIdHex,
      sessionIdHex: context.sessionIdHex,
      candidateId: context.candidateId,
      enabled: true,
      collectorId: 99, collectorElapsedMilliseconds: 0,
      samplingIntervalMs: 30000,
      maxBatchSamples: 32
    }
  });

  harness.webview.messages.length = 0;

  // Jump wall clock forward by 60 seconds while monotonic only advances by 1000ms
  harness.setNow(harness.now() + 60000);
  harness.setPerfNow(harness.perfNow() + 1000);

  // Trigger check via navigation
  harness.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });

  const discMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const discSamples = discMsgs.flatMap((m) => m.samples);
  const clockDisc = discSamples.find((s) => s.kind === "clock_discontinuity" && s.reason === "discontinuity_wall_clock");
  assert.ok(clockDisc, "must emit clock_discontinuity for wall clock jump > 15s");

  // Now submit assessment
  harness.webview.messages.length = 0;
  harness.document.getElementById("submit-form").dispatchEvent({
    type: "submit", submitter: { value: "submit" }
  });

  const submitMsgs = harness.webview.messages.filter((m) => m.type === "exam_telemetry_samples");
  const submitSamples = submitMsgs.flatMap((m) => m.samples);
  const submittedSample = submitSamples.find((s) => s.kind === "exam_submitted" && s.reason === "submit_manual");
  assert.ok(submittedSample, "submission must emit exam_submitted sample");

  assertTelemetryPrivacy(discSamples.concat(submitSamples));
}

function testTelemetryEnableBeforeBeginStartsKnownDwell() {
  const c = contextFor("begin-dwell", HEX16, HEX16_B, true);
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42, collectorElapsedMilliseconds: 0 } });
  h.webview.emit({ type: "exam_begin", begin: { ...c, serverStartUnixMilliseconds: h.now(), durationSeconds: 1800 } });
  h.webview.messages.length = 0;
  h.advancePerf(5000);
  h.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const samples = h.webview.messages.filter(m => m.type === "exam_telemetry_samples").flatMap(m => m.samples);
  assert.ok(samples.some(s => s.kind === "question_dwell" && s.durationMilliseconds === 5000));
}

function testTelemetryRecoveryAfterEnableIsSilentlyBaselined() {
  const c = contextFor("recovery-baseline", HEX16, HEX16_B, true);
  const h = createHarness(c, storage => seedDurable(storage, c, {
    serverHighestSequence: 1, nextSequence: 2, previousEventHashHex: HEX32_A,
    serverLastEventHashHex: HEX32_A, serverStateHashHex: HEX32_B
  }), new FakeStorage(), { manifest: MULTI_MANIFEST });
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42, collectorElapsedMilliseconds: 0 } });
  h.webview.emit(stateResponse(c, 1, HEX32_A, HEX32_B, false, [{
    questionId: "q1", questionRevision: 1, sequence: 1,
    clientTimeUnixMilliseconds: 1700000000000, kind: "upsert", answer: "restored", eventHashHex: HEX32_A
  }]));
  const input = latestAnswerInput(h.document);
  assert.strictEqual(input.value, "restored");
  h.webview.messages.length = 0;
  input.dispatchEvent({ type: "input", target: { value: "restored" } });
  h.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const samples = h.webview.messages.filter(m => m.type === "exam_telemetry_samples").flatMap(m => m.samples);
  assert.ok(!samples.some(s => s.kind === "question_first_answer" || s.kind === "answer_change_summary"));
}

function testTelemetryPeriodicDiscontinuityResumesKnownDwell() {
  const c = contextFor("periodic-dwell", HEX16, HEX16_B);
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42, collectorElapsedMilliseconds: 0 } });
  h.setNow(h.now() + 60000);
  h.setPerfNow(h.perfNow() + 1000);
  h.intervalCallbacks.find(timer => timer.delay === 30000).callback();
  h.webview.messages.length = 0;
  h.advancePerf(5000);
  h.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const samples = h.webview.messages.filter(m => m.type === "exam_telemetry_samples").flatMap(m => m.samples);
  assert.ok(samples.some(s => s.kind === "question_dwell" && s.durationMilliseconds === 5000));
}


function clockEnable(h, c, anchor = 0, collectorId = 42) {
  h.webview.emit({ type: "exam_telemetry_enable", enable: {
    ...c, enabled: true, collectorId, collectorElapsedMilliseconds: anchor
  } });
}
function clockSamples(h) {
  return h.webview.messages.filter(m => m.type === "exam_telemetry_samples").flatMap(m => m.samples);
}
function clockNavigate(h) {
  h.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
}
function testTelemetrySlowLoadUsesGenerationAnchor() {
  const c = contextFor("slow-load-clock");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST, perfNow: 12000 });
  clockEnable(h, c);
  assert.equal(clockSamples(h)[0].monotonicElapsedMilliseconds, 0, "slow page load must not become collector elapsed");
  h.advancePerf(3000);
  clockNavigate(h);
  const samples = clockSamples(h);
  assert.ok(samples.some(s => s.kind === "question_dwell" && s.durationMilliseconds === 3000 && s.monotonicElapsedMilliseconds === 3000));
}
function testTelemetryLateEnableUsesNativeAnchorAndDuplicatePreservesBaseline() {
  const c = contextFor("late-clock");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
  clockNavigate(h);
  h.advancePerf(11000);
  clockEnable(h, c, 40);
  assert.equal(clockSamples(h)[0].monotonicElapsedMilliseconds, 40);
  h.advancePerf(500);
  clockEnable(h, c, 999);
  clockEnable(h, c, 0, 41);
  h.advancePerf(500);
  h.document.querySelector('[data-action="previous"]').dispatchEvent({ type: "click" });
  assert.equal(clockSamples(h).filter(s => s.kind === "collector_started").length, 1);
  assert.ok(clockSamples(h).some(s => s.kind === "question_dwell" && s.monotonicElapsedMilliseconds === 1040 && s.durationMilliseconds === 1000));
}
function testTelemetryReloadAndZeroRawClockHaveIndependentOrigins() {
  const c = contextFor("reload-clock");
  const storage = new FakeStorage();
  for (const [raw, id] of [[12000, 42], [0, 43]]) {
    const h = createHarness(c, null, storage, { manifest: MULTI_MANIFEST, perfNow: raw });
    clockEnable(h, c, 0, id);
    assert.equal(clockSamples(h)[0].monotonicElapsedMilliseconds, 0);
    h.advancePerf(3000);
    clockNavigate(h);
    assert.ok(clockSamples(h).some(s => s.kind === "question_dwell" && s.monotonicElapsedMilliseconds === 3000));
    assert.ok(clockSamples(h).every(s => s.collectorId === id));
  }
}
function testTelemetryFractionalIntervalsRoundAfterSubtracting() {
  const c = contextFor("fractional-clock");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST, perfNow: 12000.25 });
  clockEnable(h, c);
  h.advancePerf(0.6);
  clockNavigate(h);
  assert.ok(clockSamples(h).some(s => s.kind === "question_first_view" && s.monotonicElapsedMilliseconds === 1));
  assert.ok(clockSamples(h).some(s => s.kind === "question_dwell" && s.durationMilliseconds === 1));
}
function testTelemetryInvalidAnchorsAndMissingClockLeaveAnswersUsable() {
  const invalid = [undefined, null, -1, 0.5, "0", NaN, Infinity, Number.MAX_SAFE_INTEGER + 1];
  for (const anchor of invalid) {
    const c = contextFor("invalid-anchor");
    const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
    h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42, collectorElapsedMilliseconds: anchor } });
    // Explicitly omit the field too; JSON-style undefined is not a valid anchor.
    if (anchor === undefined) h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42 } });
    clockNavigate(h);
    assert.equal(clockSamples(h).length, 0, `invalid anchor ${String(anchor)} refuses collection`);
    const input = latestAnswerInput(h.document);
    input.value = "answer-clock-canary";
    input.dispatchEvent({ type: "input" });
    assert.equal(input.value, "answer-clock-canary");
  }
  for (const raw of [-1, NaN, Infinity]) {
    const c = contextFor("invalid-clock");
    const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST, perfNow: raw });
    clockEnable(h, c);
    assert.equal(clockSamples(h).length, 0);
  }
  const c = contextFor("absent-clock");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
  h.window.performance = undefined;
  clockEnable(h, c);
  assert.equal(clockSamples(h).length, 0, "wall time cannot substitute for absent performance clock");
}
function testTelemetryRegressionFromZeroAndUnsafeAdditionEndCollection() {
  for (const [raw, anchor, next] of [[0, 0, -0.1], [12000.25, 0, 12000.2], [0, Number.MAX_SAFE_INTEGER, 1], [0, 0, NaN]]) {
    const c = contextFor("clock-failure");
    const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST, perfNow: raw });
    clockEnable(h, c, anchor);
    h.setPerfNow(next);
    clockNavigate(h);
    const after = clockSamples(h).slice(1);
    assert.ok(!after.some(s => s.kind === "question_dwell" || s.kind === "question_first_view"), "unknown clock cannot produce dwell or subsequent timing");
    assert.ok(after.every(s => Number.isSafeInteger(s.monotonicElapsedMilliseconds) && s.monotonicElapsedMilliseconds >= 0));
    h.setPerfNow(20000);
    clockNavigate(h);
    assert.equal(clockSamples(h).length, after.length + 1, "clock failure stays disabled until a new generation");
  }
}


function testTelemetryRecoveryFinalizeMatchesCollectionOff() {
  const c = contextFor("answer-equivalence");
  const run = (collect) => {
    const storage = new FakeStorage();
    const h = createHarness(c, target => seedDurable(target, c, {
      serverHighestSequence: 1, nextSequence: 2, previousEventHashHex: HEX32_A,
      serverLastEventHashHex: HEX32_A, serverStateHashHex: HEX32_B
    }), storage, { manifest: MULTI_MANIFEST, perfNow: 12000 });
    if (collect) clockEnable(h, c, 40);
    h.webview.emit(stateResponse(c, 1, HEX32_A, HEX32_B, false, [{
      questionId: "q1", questionRevision: 1, sequence: 1,
      clientTimeUnixMilliseconds: h.now(), kind: "upsert",
      answer: "secret answer text clock canary", eventHashHex: HEX32_A
    }]));
    h.document.getElementById("notes").value = "private note clock canary";
    h.document.getElementById("notes").dispatchEvent({ type: "input" });
    h.advancePerf(3000);
    clockNavigate(h);
    h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    // Answer events retain their existing priority and checkpoint sequencing. Retire
    // the recovered-answer upsert before expecting the queued finalize event.
    const first = h.webview.messages.find(m => m.type === "exam_answer_event");
    assert.ok(first, "submission sends the existing answer sequence first");
    if (first.event.kind !== "finalize") {
      h.webview.emit(stateResponse(c, first.event.sequence, first.event.eventHashHex,
        HEX32_D, false, [{ questionId: first.event.questionId,
          questionRevision: first.event.questionRevision, sequence: first.event.sequence,
          clientTimeUnixMilliseconds: first.event.clientTimeUnixMilliseconds,
          kind: first.event.kind, answer: first.event.answer, eventHashHex: first.event.eventHashHex }]));
    }
    const final = h.webview.messages.find(m => m.type === "exam_answer_event" && m.event.kind === "finalize");
    assert.ok(final, "same recovered exam must finalize with telemetry on or off");
    h.webview.emit(stateResponse(c, final.event.sequence, final.event.eventHashHex, HEX32_C, true));
    h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    assertTelemetryPrivacy(clockSamples(h));
    return {
      messages: h.webview.messages.filter(m => m.type !== "exam_telemetry_samples"),
      storage: [...storage.values],
      response: JSON.parse(JSON.stringify(h.window.NSTU_EXAM_RESPONSE))
    };
  };
  assert.deepEqual(run(true), run(false), "collection must preserve all non-telemetry messages, storage and finalize response");
}
function testTelemetryRawClockFailuresAndZeroWallHistory() {
  const c = contextFor("raw-history");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST, perfNow: 0 });
  clockEnable(h, c);
  h.setNow(h.now() + 60000);
  clockNavigate(h);
  assert.ok(clockSamples(h).some(s => s.kind === "clock_discontinuity" && s.reason === "discontinuity_wall_clock"), "raw zero is real clock history");
  assert.ok(!clockSamples(h).some(s => s.kind === "question_dwell"));
  for (const failure of [undefined, { now() { throw new Error("clock unavailable"); } }, { now: () => Infinity }]) {
    const live = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
    clockEnable(live, c);
    live.window.performance = failure;
    clockNavigate(live);
    assert.equal(clockSamples(live).filter(s => s.kind === "clock_discontinuity").length, 1);
    assert.ok(!clockSamples(live).some(s => s.kind === "question_dwell" || s.kind === "question_first_view"));
    live.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    assert.ok(live.webview.messages.some(m => m.type === "exam_submit"), "broken telemetry clock cannot prevent submission");
  }
  for (const id of [0, -1, 0.1, "42", Number.MAX_SAFE_INTEGER + 1]) {
    const live = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
    clockEnable(live, c, 0, id);
    assert.equal(clockSamples(live).length, 0);
  }
}


function assertRepairFinalizeAndAnswer(h, c) {
  const submitted = h.webview.messages.find(m => m.type === "exam_submit");
  assert.ok(submitted, "telemetry closure leaves submission usable");
  assert.equal(submitted.response.answers.q1, "secret closure answer canary");
  let final = h.webview.messages.find(m => m.type === "exam_answer_event" && m.event.kind === "finalize");
  if (!final) {
    const first = h.webview.messages.find(m => m.type === "exam_answer_event");
    assert.ok(first);
    h.webview.emit(stateResponse(c, first.event.sequence, first.event.eventHashHex, HEX32_D,
      false, [{ questionId: first.event.questionId, questionRevision: first.event.questionRevision,
        sequence: first.event.sequence, clientTimeUnixMilliseconds: first.event.clientTimeUnixMilliseconds,
        kind: first.event.kind, answer: first.event.answer, eventHashHex: first.event.eventHashHex }]));
    final = h.webview.messages.find(m => m.type === "exam_answer_event" && m.event.kind === "finalize");
  }
  assert.ok(final, "existing answer checkpoint still permits finalization");
}
function runRepairDirectClosure(disable, rawAdvance = 1000) {
  const c = contextFor(disable ? "direct-disable-clock" : "direct-submit-clock");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST });
  h.webview.emit(stateResponse(c, 0, "", ""));
  const input = latestAnswerInput(h.document);
  input.value = "secret closure answer canary";
  input.dispatchEvent({ type: "input" });
  clockEnable(h, c);
  h.setNow(h.now() + 60000);
  h.setPerfNow(h.perfNow() + rawAdvance);
  if (disable) {
    h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: false,
      collectorId: 42, collectorElapsedMilliseconds: 0 } });
  } else {
    h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
  }
  const samples = clockSamples(h);
  assert.ok(samples.some(s => s.kind === "clock_discontinuity" && s.reason === "discontinuity_wall_clock"), "direct closure must check wall/raw history");
  assert.ok(!samples.some(s => s.kind === "question_dwell"), "direct closure cannot infer dwell across the clock discontinuity");
  assert.ok(samples.every(s => Number.isSafeInteger(s.numericValue)), "fractional raw intervals must still emit valid integer numeric fields");
  assertTelemetryPrivacy(samples);
  if (disable) h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
  assertRepairFinalizeAndAnswer(h, c);
}
function testTelemetryDirectSubmitChecksClockDiscontinuity() { runRepairDirectClosure(false); runRepairDirectClosure(false, 1000.25); }
function testTelemetryDirectDisableChecksClockDiscontinuity() { runRepairDirectClosure(true); runRepairDirectClosure(true, 1000.25); }
function testTelemetryExplicitStartupRetryPreservesAnchorAndRawBaseline() {
  const c = contextFor("startup-retry-clock");
  const h = createHarness(c, null, new FakeStorage(), { manifest: MULTI_MANIFEST, perfNow: 12000.25 });
  clockEnable(h, c, 40);
  const original = clockSamples(h)[0];
  h.webview.messages.length = 0; // Native full-window drop of the original startup.
  const input = latestAnswerInput(h.document);
  input.value = "secret pending retry answer canary";
  input.dispatchEvent({ type: "input" }); // Ordinary observations are pending independently.
  h.advancePerf(1000);
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true,
    collectorId: 42, collectorElapsedMilliseconds: 40, collectorStartupRetry: true } });
  assert.deepEqual(clockSamples(h), [original], "explicit local retry resends the original startup only");
  h.advancePerf(1000);
  clockNavigate(h);
  assert.ok(clockSamples(h).some(s => s.kind === "question_first_view" && s.monotonicElapsedMilliseconds === 2040));
  assert.ok(clockSamples(h).some(s => s.kind === "question_dwell" && s.durationMilliseconds === 2000));
  assert.ok(clockSamples(h).some(s => s.kind === "question_first_answer"), "retry must retain pending ordinary observations for their own flush");
  assertTelemetryPrivacy(clockSamples(h));
  h.webview.messages.length = 0;
  for (const [id, anchor, retry] of [[41, 40, true], [42, 41, true], [42, 40, "true"], [42, 40, false]]) {
    h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true,
      collectorId: id, collectorElapsedMilliseconds: anchor, collectorStartupRetry: retry } });
  }
  assert.equal(clockSamples(h).length, 0, "ordinary or nonmatching requests cannot replay startup");
  const retry = () => h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true,
    collectorId: 42, collectorElapsedMilliseconds: 40, collectorStartupRetry: true } });
  retry(); // Last successful retry was one second ago.
  assert.deepEqual(clockSamples(h), [original]);
  retry();
  assert.equal(clockSamples(h).length, 1, "retry replies are throttled in raw browser time too");
  h.advancePerf(1000);
  retry();
  assert.equal(clockSamples(h).length, 2);
  h.advancePerf(1000);
  retry();
  assert.equal(clockSamples(h).length, 2, "browser retries also have a finite three-attempt budget");
}

function aggregationHarness(name, options = {}) {
  const c = contextFor(name);
  const h = createHarness(c, null, new FakeStorage(), {
    manifest: MULTI_MANIFEST, captureInspection: true, ...options
  });
  clockEnable(h, c);
  h.webview.messages.length = 0;
  return { c, h, api: h.window.captureInspection, t: h.window.captureInspection.telemetry };
}
function aggregationPackets(h) {
  return h.webview.messages.filter(m => m.type === "exam_telemetry_samples");
}
function aggregationDetails(api, count) {
  for (let i = 0; i < count; ++i) {
    api.emitTelemetrySample({ kind: "focus_changed", reason: i % 2 ? "focus_gain" : "focus_blur", numericValue: i % 2 });
  }
}
function assertAggregationPackets(h) {
  const samples = clockSamples(h);
  for (const packet of aggregationPackets(h)) {
    assert.ok(packet.samples.length >= 1 && packet.samples.length <= 32);
    assert.ok(Buffer.byteLength(JSON.stringify(packet)) <= 32 * 1024);
  }
  for (let i = 1; i < samples.length; ++i) {
    assert.ok(samples[i].monotonicElapsedMilliseconds >= samples[i - 1].monotonicElapsedMilliseconds,
      "retained ordinary and terminal observations cannot regress elapsed time");
  }
  assertTelemetryPrivacy(samples);
}
function aggregationPending(t) {
  return [...t.pendingSamples, ...Object.values(t.losses || {}).filter(Boolean),
    ...(t.startupSample ? [t.startupSample] : []), ...(t.terminalUnsent || [])];
}
function testAggregationTwentyThousandTransitionsStayLosslessAndBounded() {
  const { h, api, t } = aggregationHarness("aggregation-flood");
  const input = latestAnswerInput(h.document);
  for (let i = 0; i < 20000; ++i) {
    input.value = i % 2 ? "secret flood B" : "secret flood A";
    input.dispatchEvent({ type: "input" });
  }
  const summaries = t.pendingSamples.filter(s => s.kind === "answer_change_summary");
  assert.equal(summaries.length, 1, "20,000 edits require one pending per-question summary");
  assert.equal(summaries[0].count, 20000);
  assert.equal(summaries[0].numericValue, 20000);
  assert.equal(summaries[0].durationMilliseconds, 0);
  assert.equal(t.pendingSamples.filter(s => s.kind === "question_first_answer").length, 1);
  assert.ok(Buffer.byteLength(JSON.stringify(aggregationPending(t))) <= 2 * 1024 * 1024);
  h.advancePerf(100);
  clockNavigate(h);
  assert.ok(clockSamples(h).some(s => s.kind === "question_dwell" && s.durationMilliseconds === 100));
  assert.ok(clockSamples(h).some(s => s.kind === "question_first_view" && s.questionId === "q2"));
  assert.equal(clockSamples(h).filter(s => s.kind === "answer_change_summary").length, 1);
  assertAggregationPackets(h);
}
function testAggregationCumulativeAndDeltaAcrossFlushes() {
  const { h, api } = aggregationHarness("aggregation-delta");
  api.saveAnswer("q1", "A"); api.saveAnswer("q1", "B"); api.flushTelemetry();
  api.saveAnswer("q1", "B");
  api.saveAnswer("q1", ""); api.saveAnswer("q1", "A"); api.saveAnswer("q1", "B");
  api.flushTelemetry();
  const summaries = clockSamples(h).filter(s => s.kind === "answer_change_summary");
  assert.deepEqual(summaries.map(s => [s.count, s.numericValue]), [[2, 2], [5, 3]]);
  assert.equal(clockSamples(h).filter(s => s.kind === "question_first_answer").length, 1);
  const input = latestAnswerInput(h.document);
  input.dispatchEvent({ type: "compositionstart" });
  input.value = "IME interim"; input.dispatchEvent({ type: "input" });
  api.flushTelemetry();
  assert.equal(clockSamples(h).filter(s => s.kind === "answer_change_summary").length, 2);
  input.dispatchEvent({ type: "compositionend" }); api.flushTelemetry();
  assert.equal(clockSamples(h).filter(s => s.kind === "answer_change_summary").at(-1).numericValue, 1);
  assertAggregationPackets(h);
}
function testAggregationEmptyInputUsesTheExistingEmptyBaseline() {
  const { h, api, t } = aggregationHarness("aggregation-empty-baseline");
  api.saveAnswer("q1", ""); api.saveAnswer("q1", ""); api.flushTelemetry();
  assert.equal(clockSamples(h).length, 0, "unchanged empty input is not a normalized transition");
  api.saveAnswer("q1", "A"); api.flushTelemetry();
  assert.deepEqual(clockSamples(h).filter(s => s.kind === "answer_change_summary").map(s => [s.count, s.numericValue]), [[1, 1]]);
  assert.equal(t.pendingSamples.length, 0);
}
function testAggregationChronologyTracksMutableSummaryTime() {
  const { h, api } = aggregationHarness("aggregation-order");
  h.advancePerf(10); api.saveAnswer("q1", "A");
  h.advancePerf(10); api.emitTelemetrySample({ kind: "question_first_view", questionId: "q2" });
  h.advancePerf(10); api.saveAnswer("q1", "B");
  h.advancePerf(10); api.saveAnswer("q3", "essay draft canary");
  api.flushTelemetry();
  assertAggregationPackets(h);
  assert.deepEqual(clockSamples(h).filter(s => s.kind === "answer_change_summary").map(s => [s.questionId, s.count, s.numericValue]),
    [["q1", 2, 2], ["q3", 1, 1]]);
  assert.ok(clockSamples(h).findIndex(s => s.kind === "question_first_view") <
    clockSamples(h).findIndex(s => s.kind === "answer_change_summary"));
}
function testAggregationDetailsCapacityAndRollingPacketWindow() {
  const { h, api, t } = aggregationHarness("aggregation-capacity");
  aggregationDetails(api, 400);
  assert.equal(t.pendingSamples.length, 256, "ordinary detail retention is capped at 256");
  assert.equal(t.losses.loss_queue_full.count, 144);
  api.flushTelemetry();
  assert.equal(aggregationPackets(h).length, 4, "one flush has at most four attempted ordinary packets");
  api.flushTelemetry();
  h.advancePerf(1000); api.flushTelemetry();
  assert.equal(aggregationPackets(h).length, 4, "rolling window lasts 1001 raw milliseconds");
  h.advancePerf(1); api.flushTelemetry();
  assert.equal(aggregationPackets(h).length, 8);
  assert.ok(t.pendingSamples.length <= 1);
  h.advancePerf(1001); api.flushTelemetry();
  assert.equal(clockSamples(h).filter(s => s.kind === "focus_changed").length, 256);
  assert.equal(clockSamples(h).filter(s => s.reason === "loss_queue_full").reduce((sum, s) => sum + s.count, 0), 144);
  assert.equal(t.pendingSamples.length, 0);
  assertAggregationPackets(h);
}
function testAggregationRollingWindowUsesActualPostAttemptTimes() {
  const { h, api, t } = aggregationHarness("aggregation-post-clock-window");
  aggregationDetails(api, 256);
  const attempts = [];
  const originalPost = h.webview.postMessage.bind(h.webview);
  h.webview.postMessage = message => {
    if (message.type === "exam_telemetry_samples") {
      attempts.push(h.perfNow());
      if (attempts.length <= 4) h.advancePerf(1);
    }
    originalPost(message);
  };
  api.flushTelemetry();
  assert.deepEqual(attempts, [1000, 1001, 1002, 1003]);
  h.setPerfNow(2001); api.flushTelemetry();
  assert.equal(attempts.length, 5, "at2001 only the1000 attempt has expired, leaving one ordinary slot");
  assert.equal(t.pendingSamples.length, 96);
  for (const raw of [2002, 2003, 2004]) { h.setPerfNow(raw); api.flushTelemetry(); }
  for (const raw of attempts) {
    assert.ok(attempts.filter(t => t <= raw && raw - t < 1001).length <= 4,
      `actual post attempts must fit the rolling1001ms window at${raw}`);
  }
  assert.equal(t.pendingSamples.length, 0);
  assertAggregationPackets(h);
}
function testAggregationClockFailureDuringPostingHasOneFiniteTerminal() {
  for (const failure of ["unavailable", "regression"]) {
    const { c, h, api, t } = aggregationHarness(`aggregation-post-clock-${failure}`);
    h.webview.emit(stateResponse(c, 0, "", ""));
    api.saveAnswer("q1", "secret closure answer canary");
    aggregationDetails(api, 200);
    const originalPost = h.webview.postMessage.bind(h.webview);
    let ordinaryAttempts = 0;
    let terminalAttempts = 0;
    h.webview.postMessage = message => {
      if (message.type === "exam_telemetry_samples") {
        if (message.samples.some(s => s.kind === "clock_discontinuity" && s.reason === "discontinuity_monotonic")) {
          ++terminalAttempts;
          throw new Error("terminal unavailable during clock failure");
        }
        ++ordinaryAttempts;
        if (ordinaryAttempts === 1) h.advancePerf(5);
        if (ordinaryAttempts === 2) {
          if (failure === "unavailable") h.window.performance = undefined;
          else h.setPerfNow(1004); // Regresses from the second attempt's1005.
        }
      }
      originalPost(message);
    };
    api.flushTelemetry();
    assert.equal(ordinaryAttempts, 2, "a broken per-post clock cannot grant further ordinary capacity");
    assert.equal(terminalAttempts, 1, "during-flush clock failure has one terminal attempt without recursive ordinary drain");
    assert.equal(t.enabled, false); assert.equal(t.pendingSamples.length, 0);
    assert.ok(t.terminalUnsent.length >= 1 && t.terminalUnsent.length <= 32);
    assert.equal(t.terminalUnsent.at(-1).monotonicElapsedMilliseconds, 5, "terminal uses the last measured attempt clock");
    assert.ok(t.terminalUnsent.some(s => s.reason === "loss_rate_limit"));
    h.window.performance = { now: () => h.perfNow() }; h.setPerfNow(4000);
    api.flushTelemetry(); clockEnable(h, c);
    h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    assert.equal(ordinaryAttempts, 2); assert.equal(terminalAttempts, 1);
    assertRepairFinalizeAndAnswer(h, c);
    assertTelemetryPrivacy(t.terminalUnsent); assertAggregationPackets(h);
  }
}
function testAggregationSubmissionTerminalUsesLatestDrainedClock() {
  for (const failClock of [false, true]) {
    const { c, h, api, t } = aggregationHarness(`aggregation-submit-post-clock-${failClock}`);
    h.webview.emit(stateResponse(c, 0, "", ""));
    api.saveAnswer("q1", "secret closure answer canary"); aggregationDetails(api, 200);
    const originalPost = h.webview.postMessage.bind(h.webview);
    let ordinaryAttempts = 0;
    let terminalAttempts = 0;
    h.webview.postMessage = message => {
      if (message.type === "exam_telemetry_samples") {
        const terminal = message.samples.find(s => s.kind === "exam_submitted" ||
          (s.kind === "clock_discontinuity" && s.reason === "discontinuity_monotonic"));
        if (terminal) ++terminalAttempts;
        else {
          ++ordinaryAttempts;
          h.advancePerf(5);
          if (failClock && ordinaryAttempts === 2) h.window.performance = undefined;
        }
      }
      originalPost(message);
    };
    h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    assert.equal(ordinaryAttempts, failClock ? 2 : 4);
    assert.equal(terminalAttempts, 1, "nested clock closure cannot duplicate the submission terminal attempt");
    const terminal = aggregationPackets(h).at(-1).samples.at(-1);
    assert.equal(terminal.kind, failClock ? "clock_discontinuity" : "exam_submitted");
    assert.equal(terminal.monotonicElapsedMilliseconds, failClock ? 5 : 15,
      "terminal follows the last valid mapped time measured while draining");
    assert.equal(t.pendingSamples.length, 0);
    assertRepairFinalizeAndAnswer(h, c); assertAggregationPackets(h);
  }
}
function testAggregationAllManifestSummaryAndFirstSlotsStayBounded() {
  const manifest = { ...MULTI_MANIFEST, questions: Array.from({ length: 500 }, (_, i) => ({
    id: `q${i + 1}`, type: "short_answer", points: 1, prompt: "Question"
  })) };
  const { h, api, t } = aggregationHarness("aggregation-manifest-capacity", { manifest });
  for (const q of manifest.questions) {
    api.saveAnswer(q.id, "A");
    api.emitTelemetrySample({ kind: "question_first_view", questionId: q.id });
  }
  aggregationDetails(api, 300);
  assert.equal(t.pendingSamples.filter(s => s.kind === "answer_change_summary").length, 500);
  assert.equal(t.pendingSamples.filter(s => s.kind === "question_first_answer" || s.kind === "question_first_view").length, 1000);
  assert.equal(t.pendingSamples.filter(s => s.kind === "focus_changed").length, 256);
  assert.equal(t.losses.loss_queue_full.count, 44);
  assert.ok(Buffer.byteLength(JSON.stringify(aggregationPending(t))) <= 2 * 1024 * 1024);
  api.flushTelemetry();
  assert.equal(aggregationPackets(h).length, 4);
}
function testAggregationFailedFlushStopsAndRetainsGlobalPrefix() {
  const { h, api, t } = aggregationHarness("aggregation-failed-prefix");
  aggregationDetails(api, 100);
  let attempts = 0;
  const originalPost = h.webview.postMessage.bind(h.webview);
  h.webview.postMessage = message => {
    if (message.type === "exam_telemetry_samples" && ++attempts === 1) throw new Error("ordinary post unavailable");
    originalPost(message);
  };
  api.flushTelemetry();
  assert.equal(attempts, 1, "failed post stops the flush before a suffix can overtake it");
  assert.equal(t.pendingSamples.length, 100, "failed post retains its prefix and suffix");
  assert.equal(aggregationPackets(h).length, 0);
  api.flushTelemetry();
  assert.equal(attempts, 4, "failed attempt consumed one rolling packet slot");
  assert.equal(clockSamples(h).length, 96);
  h.advancePerf(1001); api.flushTelemetry();
  assert.equal(clockSamples(h).length, 100);
  assertAggregationPackets(h);
}
function testAggregationRetryReselectsAfterSummaryMovesPastDetail() {
  const { h, api, t } = aggregationHarness("aggregation-failed-mutable");
  h.advancePerf(10); api.saveAnswer("q1", "A");
  const originalPost = h.webview.postMessage.bind(h.webview);
  let attempts = 0;
  h.webview.postMessage = message => {
    if (message.type === "exam_telemetry_samples" && ++attempts === 1) throw new Error("first attempt unavailable");
    originalPost(message);
  };
  api.flushTelemetry();
  h.advancePerf(10); api.emitTelemetrySample({ kind: "focus_changed", reason: "focus_gain" });
  h.advancePerf(10); api.saveAnswer("q1", "B");
  assert.equal(t.pendingSamples.length, 3);
  api.flushTelemetry();
  assertAggregationPackets(h);
  assert.deepEqual(clockSamples(h).map(s => s.monotonicElapsedMilliseconds), [10, 20, 30]);
  assert.deepEqual(clockSamples(h).filter(s => s.kind === "answer_change_summary").map(s => [s.count, s.numericValue]), [[2, 2]]);
  assert.equal(t.pendingSamples.length, 0);
}
function testAggregationIndependentCounterSaturationFlags() {
  const { h, api, t } = aggregationHarness("aggregation-counter-flags");
  const max = 0xffffffff;
  t.answerChangeCount.q1 = max - 1;
  api.saveAnswer("q1", "A");
  let summary = t.pendingSamples.find(s => s.kind === "answer_change_summary");
  assert.equal(summary.count, max); assert.equal(summary.numericValue, 1); assert.equal(summary.durationMilliseconds, 0);
  api.saveAnswer("q1", "B");
  assert.equal(summary.count, max); assert.equal(summary.numericValue, 2); assert.equal(summary.durationMilliseconds, 1);
  summary.numericValue = max;
  api.saveAnswer("q1", "C");
  assert.equal(summary.numericValue, max); assert.equal(summary.durationMilliseconds, 3);
  api.flushTelemetry(); api.saveAnswer("q1", "D");
  summary = t.pendingSamples.find(s => s.kind === "answer_change_summary");
  assert.equal(summary.numericValue, 1); assert.equal(summary.durationMilliseconds, 1, "new delta is exact despite cumulative lower bound");
  t.answerChangeCount.q3 = 0;
  api.saveAnswer("q3", "A");
  summary = t.pendingSamples.find(s => s.kind === "answer_change_summary" && s.questionId === "q3");
  summary.numericValue = max;
  api.saveAnswer("q3", "B");
  assert.equal(summary.count, 2); assert.equal(summary.numericValue, max); assert.equal(summary.durationMilliseconds, 2);
  api.flushTelemetry();
  for (const s of clockSamples(h)) {
    for (const key of ["count", "numericValue", "durationMilliseconds"]) {
      assert.ok(Number.isInteger(s[key]) && s[key] >= 0 && s[key] <= max);
    }
  }
}
function testAggregationLossExactMaximumAndOverflowAreDistinct() {
  const { api, t } = aggregationHarness("aggregation-loss-flags");
  aggregationDetails(api, 257);
  const loss = t.losses.loss_queue_full;
  loss.count = 0xfffffffe;
  aggregationDetails(api, 1);
  assert.equal(loss.count, 0xffffffff); assert.equal(loss.durationMilliseconds, 0); assert.equal(loss.numericValue, 0);
  aggregationDetails(api, 1);
  assert.equal(loss.count, 0xffffffff); assert.equal(loss.durationMilliseconds, 1); assert.equal(loss.numericValue, 0);
}
function testAggregationTerminalReservesLossSlotsAndCountsOmittedDeltas() {
  const { c, h, api, t } = aggregationHarness("aggregation-terminal-units");
  aggregationDetails(api, 300);
  api.flushTelemetry(); // 128 retained details remain, plus 44 queue losses.
  h.advancePerf(1);
  t.answerChangeCount.q1 = 1000; // Omitted units must use delta, never cumulative.
  for (let i = 0; i < 7; ++i) api.saveAnswer("q1", i % 2 ? "B" : "A");
  const before = aggregationPackets(h).length;
  h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
  const terminal = aggregationPackets(h).slice(before);
  assert.equal(terminal.length, 1, "exhausted ordinary capacity permits one finite terminal packet");
  const samples = terminal[0].samples;
  assert.equal(samples.filter(s => s.kind === "exam_submitted").length, 1);
  assert.equal(samples.find(s => s.reason === "loss_queue_full").count, 44);
  assert.equal(samples.find(s => s.reason === "loss_rate_limit").count, (128 - 29) + 1 + 7 + 1,
    "omitted summary represents seven transitions, alongside omitted first-answer and final dwell");
  assert.equal(samples.length, 32, "two nonzero losses and terminal each have a reserved slot");
  assert.equal(t.pendingSamples.length, 0);
  h.advancePerf(1001); api.flushTelemetry();
  assert.equal(aggregationPackets(h).length, before + 1, "terminal retirement prohibits old replay");
  assert.ok(h.webview.messages.some(m => m.type === "exam_submit"));
  assertAggregationPackets(h);
}
function testAggregationTerminalLossPreservesOnlyRelevantLowerBoundFlags() {
  for (const deltaOverflow of [false, true]) {
    const { h, api, t } = aggregationHarness(`aggregation-terminal-saturation-${deltaOverflow}`);
    aggregationDetails(api, 300); api.flushTelemetry();
    h.advancePerf(1);
    t.answerChangeCount.q1 = 0xffffffff;
    api.saveAnswer("q1", "A");
    const summary = t.pendingSamples.find(s => s.kind === "answer_change_summary");
    if (deltaOverflow) { summary.numericValue = 0xffffffff; summary.durationMilliseconds |= 2; }
    t.losses.loss_queue_full.count = 0xffffffff;
    t.losses.loss_queue_full.durationMilliseconds = 1;
    h.webview.emit({ type: "exam_telemetry_enable", enable: { ...contextFor(`aggregation-terminal-saturation-${deltaOverflow}`), enabled: false } });
    const samples = aggregationPackets(h).at(-1).samples;
    const rateLoss = samples.find(s => s.reason === "loss_rate_limit");
    assert.equal(rateLoss.durationMilliseconds, deltaOverflow ? 1 : 0,
      "cumulative overflow alone must not mark the omitted delta as inexact");
    assert.equal(samples.find(s => s.reason === "loss_queue_full").durationMilliseconds, 1);
    assert.equal(samples.at(-1).kind, "clock_discontinuity");
    assert.equal(samples.at(-1).reason, "discontinuity_suspend");
    assertAggregationPackets(h);
  }
}
function testAggregationFailedTerminalPathsStayFiniteAndAnswersUsable() {
  for (const path of ["submit", "disable", "clock"]) {
    const { c, h, api, t } = aggregationHarness(`aggregation-terminal-failure-${path}`);
    h.webview.emit(stateResponse(c, 0, "", ""));
    api.saveAnswer("q1", "secret closure answer canary");
    aggregationDetails(api, 300); api.flushTelemetry();
    let attempts = 0;
    const originalPost = h.webview.postMessage.bind(h.webview);
    h.webview.postMessage = message => {
      if (message.type === "exam_telemetry_samples") { ++attempts; throw new Error("terminal unavailable"); }
      originalPost(message);
    };
    if (path === "disable") h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: false } });
    else if (path === "clock") { h.setPerfNow(-1); clockNavigate(h); }
    else h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    assert.equal(attempts, 1, `${path} has exactly one terminal attempt, including failure`);
    assert.equal(t.pendingSamples.length, 0, "older tail is retired even when terminal delivery fails");
    assert.ok(t.terminalUnsent.length >= 1 && t.terminalUnsent.length <= 32);
    assert.ok(Buffer.byteLength(JSON.stringify(aggregationPending(t))) <= 2 * 1024 * 1024);
    h.advancePerf(2000); api.flushTelemetry();
    clockEnable(h, c); // Same generation cannot reopen or replay terminal.
    if (path !== "submit") h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    assert.equal(attempts, 1);
    assertRepairFinalizeAndAnswer(h, c);
    assertTelemetryPrivacy(t.terminalUnsent);
  }
}
function testAggregationResetCancelsDeferredDrainAndClearsGeneration() {
  const { h, api, t } = aggregationHarness("aggregation-reset");
  aggregationDetails(api, 200); api.flushTelemetry();
  assert.ok(t.deferredTimer !== null, "exhausted capacity schedules one deferred drain");
  const id = t.deferredTimer;
  api.flushTelemetry();
  assert.equal(t.deferredTimer, id, "repeated triggers coalesce into one timer");
  assert.ok(h.timers.has(id));
  api.resetTelemetryState();
  assert.ok(!h.timers.has(id));
  assert.equal(t.deferredTimer, null); assert.equal(t.pendingSamples.length, 0);
  assert.equal(t.startupSample, null); assert.equal(t.terminalUnsent.length, 0);
  assert.equal(Object.values(t.losses).filter(Boolean).length, 0);
}
function testAggregationDeferredTimerDrainsOnceCapacityReturns() {
  const { h, api, t } = aggregationHarness("aggregation-deferred-drain");
  aggregationDetails(api, 200); api.flushTelemetry();
  const id = t.deferredTimer;
  assert.equal(h.timers.get(id).delay, 1001);
  const callback = h.timers.get(id).callback;
  h.timers.delete(id);
  h.advancePerf(1001); callback();
  assert.equal(clockSamples(h).length, 200);
  assert.equal(t.pendingSamples.length, 0); assert.equal(t.deferredTimer, null);
  callback(); // A stale callback cannot cause another drain.
  assert.equal(aggregationPackets(h).length, 7);
  assertAggregationPackets(h);
}
function testAggregationRejectedStartupRetryRetainsPendingObservations() {
  const { c, h, api, t } = aggregationHarness("aggregation-retry-pending");
  const startup = JSON.parse(JSON.stringify(t.startupSample));
  api.saveAnswer("q1", "secret retry answer canary"); aggregationDetails(api, 200);
  const ordinary = JSON.stringify(t.pendingSamples);
  h.advancePerf(1000);
  const originalPost = h.webview.postMessage.bind(h.webview);
  h.webview.postMessage = message => {
    if (message.type === "exam_telemetry_samples" && message.samples[0].kind === "collector_started") throw new Error("startup duplicate rejected");
    originalPost(message);
  };
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42,
    collectorElapsedMilliseconds: 0, collectorStartupRetry: true } });
  assert.equal(JSON.stringify(t.pendingSamples), ordinary);
  assert.deepEqual(JSON.parse(JSON.stringify(t.startupSample)), startup);
  api.flushTelemetry(); h.advancePerf(1001); api.flushTelemetry();
  assert.equal(clockSamples(h).filter(s => s.kind === "answer_change_summary").reduce((sum, s) => sum + s.numericValue, 0), 1);
  assertAggregationPackets(h);
}
function testAggregationPressureFailureRecoveryAndFinalizeMatchOff() {
  const c = contextFor("aggregation-answer-equivalence");
  const run = mode => {
    const storage = new FakeStorage();
    const h = createHarness(c, target => seedDurable(target, c, {
      serverHighestSequence: 1, nextSequence: 2, previousEventHashHex: HEX32_A,
      serverLastEventHashHex: HEX32_A, serverStateHashHex: HEX32_B
    }), storage, { manifest: MULTI_MANIFEST, captureInspection: true });
    const api = h.window.captureInspection;
    if (mode !== "off") clockEnable(h, c);
    h.webview.emit(stateResponse(c, 1, HEX32_A, HEX32_B, false, [{
      questionId: "q1", questionRevision: 1, sequence: 1, clientTimeUnixMilliseconds: h.now(),
      kind: "upsert", answer: "secret recovered answer canary", eventHashHex: HEX32_A
    }]));
    h.document.getElementById("notes").value = "private note pressure canary";
    h.document.getElementById("notes").dispatchEvent({ type: "input" });
    api.saveAnswer("q1", "secret edited answer canary");
    if (mode === "pressure" || mode === "failed" || mode === "clock") {
      aggregationDetails(api, 400);
      api.flushTelemetry();
    }
    if (mode === "failed") {
      const originalPost = h.webview.postMessage.bind(h.webview);
      h.webview.postMessage = message => {
        if (message.type === "exam_telemetry_samples") throw new Error("telemetry unavailable");
        originalPost(message);
      };
    }
    if (mode === "disabled") h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: false } });
    if (mode === "clock") { h.setPerfNow(-1); clockNavigate(h); }
    else clockNavigate(h);
    h.document.getElementById("submit-form").dispatchEvent({ type: "submit", submitter: { value: "submit" } });
    const first = h.webview.messages.find(m => m.type === "exam_answer_event");
    assert.ok(first);
    if (first.event.kind !== "finalize") h.webview.emit(stateResponse(c, first.event.sequence, first.event.eventHashHex, HEX32_D,
      false, [{ questionId: first.event.questionId, questionRevision: first.event.questionRevision,
        sequence: first.event.sequence, clientTimeUnixMilliseconds: first.event.clientTimeUnixMilliseconds,
        kind: first.event.kind, answer: first.event.answer, eventHashHex: first.event.eventHashHex }]));
    const final = h.webview.messages.find(m => m.type === "exam_answer_event" && m.event.kind === "finalize");
    assert.ok(final);
    h.webview.emit(stateResponse(c, final.event.sequence, final.event.eventHashHex, HEX32_C, true));
    assertTelemetryPrivacy(clockSamples(h)); assertTelemetryPrivacy(api.telemetry.terminalUnsent || []);
    return { messages: h.webview.messages.filter(m => m.type !== "exam_telemetry_samples"),
      storage: [...storage.values], response: JSON.parse(JSON.stringify(h.window.NSTU_EXAM_RESPONSE)) };
  };
  const off = run("off");
  for (const mode of ["enabled", "disabled", "pressure", "failed", "clock"]) {
    assert.deepEqual(run(mode), off, `${mode} must preserve exact answer recovery and finalization`);
  }
}

testStaleWatermarkIsIgnored();
testFinalizedResponseDoesNotRequeueFinalize();
testContextSwitchPreservesTargetNamespace();
testContextSwitchRejectsForeignPackage();
testInitialForeignPackageContextCannotLoadStorage();
testLocalFinalizationLocksDynamicControls();
testContextSwitchRestoresEditableControls();
testMismatchedPendingFinalizationDoesNotLockContext();
testContextSwitchQuarantinesStaleTargetNamespace();
testMalformedStoredClearIsQuarantined();
testZeroWatermarkRejectsStateHash();
testRejectedAckSchedulesRetry();
testEquivalentChunkEncodingsAreNotConflicts();
testPendingDraftWinsOverAuthoritativeCheckpoint();
testAnswerMapFailureLeavesDurableDraft();
testInjectedAssetBaseResolvesLocalMedia();
testInjectedAssetBaseUsesBrowserUrlResolution();
testInjectedAssetBaseRejectsExternalAndTraversal();
testInjectedAssetBaseRejectsQueryAndFragmentSuffixes();
testStaticPreviewKeepsRelativeAssetPaths();
testExpiredTimerDoesNotResetAfterReload();
testHostedExamWaitsForAuthenticatedBegin();
testMalformedManifestDoesNotCrashInitialRender();
testTelemetryDisabledByDefaultAndEnableLifecycle();
testTelemetryNavigationAndDwell();
testTelemetryLateEnableAndSilentlyBaselines();
testTelemetryFocusAndVisibility();
testTelemetryImeAndInputNormalization();
testTelemetryClockDiscontinuityAndSubmission();
const reviewTests = [testTelemetryEnableBeforeBeginStartsKnownDwell,
  testTelemetryRecoveryAfterEnableIsSilentlyBaselined,
  testTelemetryPeriodicDiscontinuityResumesKnownDwell,
  testTelemetrySlowLoadUsesGenerationAnchor,
  testTelemetryLateEnableUsesNativeAnchorAndDuplicatePreservesBaseline,
  testTelemetryReloadAndZeroRawClockHaveIndependentOrigins,
  testTelemetryFractionalIntervalsRoundAfterSubtracting,
  testTelemetryInvalidAnchorsAndMissingClockLeaveAnswersUsable,
  testTelemetryRegressionFromZeroAndUnsafeAdditionEndCollection,
  testTelemetryRecoveryFinalizeMatchesCollectionOff, testTelemetryRawClockFailuresAndZeroWallHistory,
  testTelemetryDirectSubmitChecksClockDiscontinuity, testTelemetryDirectDisableChecksClockDiscontinuity,
  testTelemetryExplicitStartupRetryPreservesAnchorAndRawBaseline];
const reviewFailures = [];
for (const test of reviewTests) {
  try { test(); } catch (error) { reviewFailures.push(`${test.name}: ${error.stack}`); }
}
const aggregationTests = [testAggregationTwentyThousandTransitionsStayLosslessAndBounded,
  testAggregationCumulativeAndDeltaAcrossFlushes, testAggregationEmptyInputUsesTheExistingEmptyBaseline,
  testAggregationChronologyTracksMutableSummaryTime,
  testAggregationDetailsCapacityAndRollingPacketWindow, testAggregationRollingWindowUsesActualPostAttemptTimes,
  testAggregationClockFailureDuringPostingHasOneFiniteTerminal, testAggregationSubmissionTerminalUsesLatestDrainedClock,
  testAggregationAllManifestSummaryAndFirstSlotsStayBounded,
  testAggregationFailedFlushStopsAndRetainsGlobalPrefix, testAggregationRetryReselectsAfterSummaryMovesPastDetail,
  testAggregationIndependentCounterSaturationFlags, testAggregationLossExactMaximumAndOverflowAreDistinct,
  testAggregationTerminalReservesLossSlotsAndCountsOmittedDeltas, testAggregationTerminalLossPreservesOnlyRelevantLowerBoundFlags,
  testAggregationFailedTerminalPathsStayFiniteAndAnswersUsable,
  testAggregationResetCancelsDeferredDrainAndClearsGeneration, testAggregationDeferredTimerDrainsOnceCapacityReturns,
  testAggregationRejectedStartupRetryRetainsPendingObservations,
  testAggregationPressureFailureRecoveryAndFinalizeMatchOff];
for (const test of aggregationTests) {
  try { test(); } catch (error) { reviewFailures.push(`${test.name}: ${error.stack}`); }
}
assert.strictEqual(reviewFailures.length, 0, reviewFailures.join("\n"));
console.log(`exam web recovery tests: ${43 + aggregationTests.length} passed`);
