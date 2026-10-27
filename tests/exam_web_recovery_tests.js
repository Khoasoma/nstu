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
  let perfNow = 1000;
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
      static now() { return now; }
    },
    console
  });
  vm.runInContext(APP_SOURCE, context, { filename: "exam/web/app.js" });
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
      collectorId: 42,
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
      collectorId: 101,
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
      collectorId: 55,
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
      collectorId: 77,
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
      collectorId: 88,
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
      collectorId: 99,
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
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42 } });
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
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42 } });
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
  h.webview.emit({ type: "exam_telemetry_enable", enable: { ...c, enabled: true, collectorId: 42 } });
  h.setNow(h.now() + 60000);
  h.setPerfNow(h.perfNow() + 1000);
  h.intervalCallbacks.find(timer => timer.delay === 30000).callback();
  h.webview.messages.length = 0;
  h.advancePerf(5000);
  h.document.querySelector('[data-action="next"]').dispatchEvent({ type: "click" });
  const samples = h.webview.messages.filter(m => m.type === "exam_telemetry_samples").flatMap(m => m.samples);
  assert.ok(samples.some(s => s.kind === "question_dwell" && s.durationMilliseconds === 5000));
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
  testTelemetryPeriodicDiscontinuityResumesKnownDwell];
const reviewFailures = [];
for (const test of reviewTests) {
  try { test(); } catch (error) { reviewFailures.push(`${test.name}: ${error.stack}`); }
}
assert.strictEqual(reviewFailures.length, 0, reviewFailures.join("\n"));
console.log("exam web recovery tests: 32 passed");
