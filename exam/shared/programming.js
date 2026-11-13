(function () {
  "use strict";
  const LANGUAGES = ["cpp20", "python3"], MAX_SOURCE = 32768, MAX_STORE = 8 * 1024 * 1024;
  const ROOT_FIELDS = ["id", "title", "subject", "candidate", "durationSeconds", "allowedOrigins", "documents", "questions", "contentMode", "programmingVersion", "sessionMode", "languages"];
  const PROBLEM_FIELDS = ["id", "type", "revision", "prompt", "instruction", "points", "inputDescription", "outputDescription", "constraints", "explanation", "samples", "timeLimitMs", "memoryLimitMiB"];
  const RESPONSE_FIELDS = ["type", "requestId", "operation", "problemId", "problemRevision", "language", "draftRevision", "sourceDigest", "status", "receiptId", "latestReceiptId", "serverTime", "serverStartUnixMilliseconds", "deadlineUnixMilliseconds", "sampleId", "sampleMatched", "stdout", "stderr", "diagnostics", "truncated", "sourceBase64", "finalized", "runnerReady", "reason", "context", "fullContext", "programmingVersion", "packageId", "packageDigestHex", "clientIdHex", "sessionIdHex", "candidateId", "receiptDigest"];
  const STATUSES = ["saved", "received", "ready", "unavailable", "completed", "compilation_error", "runtime_error", "timeout", "cancelled", "recovered", "finalized", "rejected", "conflict", "closed", "late"];
  const object = value => value !== null && typeof value === "object" && !Array.isArray(value);
  const bytes = value => new TextEncoder().encode(value).length;
  const safeId = value => typeof value === "string" && /^[A-Za-z0-9_][A-Za-z0-9._-]{0,95}$/.test(value) && !(value in {});
  const integer = (value, min, max) => Number.isSafeInteger(value) && value >= min && value <= max;
  function textError(value, limit) {
    if (typeof value !== "string") return "Text is required.";
    if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(value)) return "Text contains unsupported control characters.";
    for (let i = 0; i < value.length; ++i) {
      const unit = value.charCodeAt(i);
      if (unit >= 0xd800 && unit <= 0xdbff) {
        const next = value.charCodeAt(++i);
        if (!(next >= 0xdc00 && next <= 0xdfff)) return "Text contains invalid Unicode.";
      } else if (unit >= 0xdc00 && unit <= 0xdfff) return "Text contains invalid Unicode.";
    }
    return bytes(value) > limit ? `Text exceeds ${limit} UTF-8 bytes.` : "";
  }
  const sourceError = value => textError(value, MAX_SOURCE);
  function projectPublicManifest(model) {
    const out = {};
    for (const field of ROOT_FIELDS) if (Object.hasOwn(model, field) && field !== "questions") out[field] = model[field];
    out.questions = (model.questions || []).map(problem => {
      const item = {};
      for (const field of PROBLEM_FIELDS) if (Object.hasOwn(problem, field) && field !== "samples") item[field] = problem[field];
      item.revision = item.revision ?? 1;
      item.samples = (problem.samples || []).map(sample => ({ id: sample.id, input: sample.input, output: sample.output }));
      return item;
    });
    return out;
  }
  function validatePublicManifest(model) {
    const errors = [];
    const fields = (value, allowed, path) => {
      if (!object(value)) { errors.push(`${path} must be an object.`); return false; }
      for (const key of Object.keys(value)) if (!allowed.includes(key)) errors.push(`${path}.${key} is not a public programming field.`);
      return true;
    };
    const text = (value, maximum, path, required = false) => {
      const error = textError(value, maximum);
      if (error || (required && !value)) errors.push(`${path}: ${error || "Text is required."}`);
    };
    if (!fields(model, ROOT_FIELDS, "Manifest")) return errors;
    if (!safeId(model.id)) errors.push("Package ID is invalid.");
    text(model.title, 240, "Title", true);
    for (const [field, maximum] of [["subject", 120], ["candidate", 160]]) if (Object.hasOwn(model, field)) text(model[field], maximum, field);
    if (!integer(model.durationSeconds, 60, 86400)) errors.push("Duration must be 60–86400 seconds.");
    if (model.contentMode !== "computer_science" || model.programmingVersion !== 1) errors.push("Programming format version is unsupported.");
    if (!["exam", "contest", "practice"].includes(model.sessionMode)) errors.push("Session mode is unsupported.");
    if (!Array.isArray(model.languages) || model.languages.length !== 2 || model.languages.some((language, index) => language !== LANGUAGES[index])) errors.push("C++20 and Python 3 are required.");
    for (const field of ["documents", "allowedOrigins"]) if (Object.hasOwn(model, field) && (!Array.isArray(model[field]) || model[field].length)) errors.push(`Programming ${field} must be empty; content is local text.`);
    if (!Array.isArray(model.questions) || model.questions.length < 1 || model.questions.length > 64) errors.push("Programming packages require 1–64 problems.");
    else {
      const ids = new Set();
      model.questions.forEach((problem, index) => {
        const path = `Problem ${index + 1}`;
        if (!fields(problem, PROBLEM_FIELDS, path)) return;
        if (!safeId(problem.id) || ids.has(problem.id)) errors.push(`${path}: problem ID is invalid or duplicated.`);
        ids.add(problem.id);
        if (problem.type !== "programming" || !integer(problem.revision ?? 1, 1, 0xffffffff)) errors.push(`${path}: type/revision is invalid.`);
        text(problem.prompt, 8000, `${path} statement`, true);
        for (const field of ["instruction", "inputDescription", "outputDescription", "constraints", "explanation"]) if (Object.hasOwn(problem, field)) text(problem[field], 4000, `${path} ${field}`);
        if (Object.hasOwn(problem, "points") && !integer(problem.points, 0, 1000)) errors.push(`${path}: points are invalid.`);
        if (!integer(problem.timeLimitMs, 100, 10000) || !integer(problem.memoryLimitMiB, 16, 512)) errors.push(`${path}: runtime limits are invalid.`);
        if (!Array.isArray(problem.samples) || problem.samples.length < 1 || problem.samples.length > 8) errors.push(`${path}: 1–8 public sample pairs are required.`);
        else {
          const samples = new Set();
          problem.samples.forEach(sample => {
            if (!fields(sample, ["id", "input", "output"], `${path} sample`)) return;
            if (!safeId(sample.id) || samples.has(sample.id)) errors.push(`${path}: sample ID is invalid or duplicated.`);
            samples.add(sample.id);
            text(sample.input, 4096, `${path} public input`); text(sample.output, 4096, `${path} public output`);
          });
        }
      });
    }
    try { if (bytes(JSON.stringify(model)) > 131072) errors.push("Public manifest exceeds 128 KiB."); }
    catch (_) { errors.push("Manifest cannot be serialized."); }
    return errors;
  }
  let requestCounter = 0;
  function createSession(manifest, options) {
    const context = { ...options.context }, contextText = JSON.stringify(context);
    const problems = new Map(manifest.questions.map(problem => [problem.id, problem]));
    const key = options.key, pending = new Map(), runs = new Map(), sentRequests = new Set(), recoveryConflicts = new Set();
    let data = { context: contextText, documents: {}, selected: {}, samples: {}, receipts: {}, finishRequested: false, finished: false }, saved = false, ready = false, error = "", finalRequest = null;
    const documentKey = (id, language) => `${id}:${language}`;
    const validDocument = (id, language) => problems.has(id) && LANGUAGES.includes(language);
    const changed = () => { if (options.onChange) options.onChange(); };
    function persist() {
      try {
        const histories = Object.values(data.documents);
        let historyBytes = histories.reduce((total, doc) => total + bytes(JSON.stringify(doc.undo || [])) + bytes(JSON.stringify(doc.redo || [])), 0);
        // shortcut: restart history is capped at 2 MiB; expand only after cohort storage calibration.
        while (historyBytes > 2 * 1024 * 1024) {
          const doc = histories.find(item => item.undo.length || item.redo.length);
          if (!doc) break;
          const removed = doc.undo.length ? doc.undo.shift() : doc.redo.shift();
          historyBytes -= bytes(JSON.stringify(removed));
        }
        const value = JSON.stringify({ ...data, pending: Array.from(pending.values()), recoveryConflicts: Array.from(recoveryConflicts) });
        if (bytes(value) > MAX_STORE) throw new Error("Programming recovery storage is full.");
        if (!options.storage || !key) throw new Error("Programming recovery storage is unavailable.");
        options.storage.setItem(key, value); saved = true;
      } catch (failure) { saved = false; error = failure.message || "Code is not saved."; }
      return saved;
    }
    function document(id, language) {
      if (!validDocument(id, language)) throw new Error("Unknown programming document.");
      const name = documentKey(id, language);
      if (!data.documents[name]) data.documents[name] = { source: "", revision: 1, start: 0, end: 0, undo: [], redo: [] };
      return data.documents[name];
    }
    function update(id, language, source, selection = {}, history = true) {
      const doc = document(id, language);
      if (data.finishRequested || data.finished) return false;
      if (source !== doc.source) {
        if (history) { doc.undo.push({ source: doc.source, start: doc.start, end: doc.end }); doc.redo = []; }
        doc.source = source; ++doc.revision;
        recoveryConflicts.delete(documentKey(id, language));
      }
      doc.start = selection.start ?? doc.start; doc.end = selection.end ?? doc.end;
      const validation = sourceError(source);
      if (validation) { saved = false; error = validation; changed(); return false; }
      error = ""; const result = persist(); changed(); return result;
    }
    function undo(id, language, redo = false) {
      const doc = document(id, language), from = redo ? doc.redo : doc.undo, to = redo ? doc.undo : doc.redo;
      if (data.finishRequested || !from.length) return null;
      to.push({ source: doc.source, start: doc.start, end: doc.end });
      const value = from.pop(); doc.source = value.source; doc.start = value.start; doc.end = value.end; ++doc.revision;
      recoveryConflicts.delete(documentKey(id, language));
      persist(); changed(); return doc;
    }
    function transmit(value) {
      let sent = false;
      try { sent = options.send && options.send(value) === true; } catch (_) { /* Keep the durable snapshot for retry. */ }
      if (sent) sentRequests.add(value.requestId); else sentRequests.delete(value.requestId);
      if (!sent) error = "Offline. Pending solutions have not been received.";
      return sent;
    }
    function request(operation, id = "", language = "", sampleId = "") {
      if (!["draft", "submit", "run", "cancel", "recover", "finalize", "ready"].includes(operation)) return null;
      const value = { type: "exam_programming_request", operation,
        requestId: `cs-${Date.now().toString(36)}-${++requestCounter}-${Math.random().toString(36).slice(2, 10)}`, context: { ...context } };
      if (operation === "finalize") {
        value.receiptIds = []; value.noSourceProblemIds = [];
        for (const problemId of problems.keys()) {
          const selected = selectedLanguage(problemId), doc = document(problemId, selected);
          if (recoveryConflicts.has(documentKey(problemId, selected))) return null;
          if (!doc.source) value.noSourceProblemIds.push(problemId);
          else {
            const receipt = data.receipts[documentKey(problemId, selected)];
            if (!receipt || !receipt.verified || receipt.sourceDigest !== options.hash(doc.source)) return null;
            value.receiptIds.push(receipt.receiptId);
          }
        }
      }
      if (!["finalize", "ready"].includes(operation)) {
        if (!validDocument(id, language)) return null;
        const doc = document(id, language);
        Object.assign(value, { problemId: id, problemRevision: problems.get(id).revision ?? 1, language });
        if (["draft", "submit", "run"].includes(operation)) value.draftRevision = doc.revision;
        if (["draft", "submit", "run"].includes(operation)) {
          if (recoveryConflicts.has(documentKey(id, language))) { error = "Programming recovery source conflict. Edit this draft before sending it."; changed(); return null; }
          if (sourceError(doc.source) || (operation !== "draft" && !doc.source)) { error = sourceError(doc.source) || "Source is empty."; changed(); return null; }
          value.source = doc.source;
        }
        if (operation === "run") {
          if (!problems.get(id).samples.some(sample => sample.id === sampleId)) return null;
          value.sampleId = sampleId;
        }
        if (operation === "cancel") {
          const active = runs.get(id); if (!active || active.status !== "running") return null;
          value.targetRunId = active.requestId;
        }
      }
      const replaced = operation === "draft" ? Array.from(pending.values()).filter(intent => intent.operation === "draft" &&
        intent.problemId === id && intent.problemRevision === value.problemRevision && intent.language === language) : [];
      if (pending.size - replaced.length >= 256) { error = "Programming recovery queue is full."; changed(); return null; }
      replaced.forEach(intent => pending.delete(intent.requestId));
      pending.set(value.requestId, value);
      if (!persist()) { pending.delete(value.requestId); replaced.forEach(intent => pending.set(intent.requestId, intent)); changed(); return null; }
      replaced.forEach(intent => sentRequests.delete(intent.requestId));
      if (operation === "run") runs.set(id, { requestId: value.requestId, source: value.source, language, sampleId, status: "running" });
      if (!transmit(value) && operation === "run") { runs.set(id, { ...runs.get(id), status: "unavailable" }); ready = false; }
      changed(); return value;
    }
    function selectedLanguage(id) { return data.selected[id] || "cpp20"; }
    function selectedSample(id) { return data.samples[id] || problems.get(id).samples[0].id; }
    function delivery(id, language) {
      const doc = document(id, language), receipt = data.receipts[documentKey(id, language)];
      if (receipt && receipt.verified) return receipt.sourceDigest === options.hash(doc.source) ? receipt.late ? "late" : "received" : "changed";
      const queued = Array.from(pending.values()).find(item => item.operation === "submit" && item.problemId === id && item.language === language);
      if (queued) return sentRequests.has(queued.requestId) ? "sending" : "queued";
      return doc.source ? "draft" : "empty";
    }
    function maybeFinalize() {
      if (!data.finishRequested || data.finished || finalRequest) return;
      if (!persist()) return;
      let waiting = false;
      for (const id of problems.keys()) {
        const language = selectedLanguage(id), doc = document(id, language), receipt = data.receipts[documentKey(id, language)];
        if (recoveryConflicts.has(documentKey(id, language))) return;
        if (doc.source && (!receipt || !receipt.verified || receipt.sourceDigest !== options.hash(doc.source))) {
          waiting = true;
          if (!Array.from(pending.values()).some(intent => intent.operation === "submit" && intent.problemId === id &&
              intent.language === language && intent.draftRevision === doc.revision && intent.source === doc.source)) request("submit", id, language);
        }
      }
      if (waiting) return;
      finalRequest = request("finalize");
    }
    function receive(message) {
      if (!object(message) || Object.keys(message).some(field => !RESPONSE_FIELDS.includes(field)) ||
          message.type !== "exam_programming_response" || !STATUSES.includes(message.status)) return false;
      const intent = pending.get(message.requestId);
      if (!intent || intent.operation !== message.operation) return false;
      if (Object.hasOwn(message, "programmingVersion") && message.programmingVersion !== 1) return false;
      for (const name of ["fullContext", "context"]) if (Object.hasOwn(message, name)) {
        if (!object(message[name]) || Object.keys(message[name]).some(field => !Object.hasOwn(context, field)) ||
            Object.entries(context).some(([field, value]) => message[name][field] !== value)) return false;
      }
      for (const [field, value] of Object.entries(context)) if (Object.hasOwn(message, field) && message[field] !== value) return false;
      if (Object.hasOwn(message, "reason") && textError(message.reason, 512)) return false;
      for (const field of ["serverStartUnixMilliseconds", "deadlineUnixMilliseconds"]) if (Object.hasOwn(message, field) && !integer(message[field], 0, Number.MAX_SAFE_INTEGER)) return false;
      if (Object.hasOwn(message, "finalized") && typeof message.finalized !== "boolean") return false;
      if (!["ready", "finalize"].includes(intent.operation)) {
        if (message.problemId !== intent.problemId || message.language !== intent.language ||
            (Object.hasOwn(message, "problemRevision") && message.problemRevision !== intent.problemRevision)) return false;
        if (["draft", "submit", "run"].includes(intent.operation) && message.draftRevision !== intent.draftRevision) return false;
        if (Object.hasOwn(intent, "source") && ["saved", "received", "completed", "compilation_error", "runtime_error", "timeout", "cancelled", "late"].includes(message.status) && message.sourceDigest !== options.hash(intent.source)) return false;
      }
      if (intent.operation === "run") {
        if ((Object.hasOwn(message, "sampleId") && message.sampleId !== intent.sampleId) ||
            (Object.hasOwn(message, "sampleMatched") && typeof message.sampleMatched !== "boolean")) return false;
        if (!["completed", "compilation_error", "runtime_error", "timeout", "cancelled", "unavailable", "rejected", "closed", "late"].includes(message.status)) return false;
        const streams = [message.stdout ?? "", message.stderr ?? "", message.diagnostics ?? ""];
        if (streams.some(stream => textError(stream, 24576)) || streams.reduce((total, stream) => total + bytes(stream), 0) > 24576 ||
            (Object.hasOwn(message, "truncated") && typeof message.truncated !== "boolean")) return false;
        const current = runs.get(intent.problemId);
        if (!current || current.requestId !== intent.requestId) return false;
        runs.set(intent.problemId, { ...current, ...message, stale: document(intent.problemId, intent.language).source !== intent.source });
        if (message.status === "unavailable") ready = false;
      } else if (["stdout", "stderr", "diagnostics", "truncated", "sampleId", "sampleMatched"].some(field => Object.hasOwn(message, field))) return false;
      if (intent.operation === "recover" && message.status === "recovered") {
        let source;
        try {
          if (typeof message.sourceBase64 !== "string" || message.sourceBase64.length > 43692) return false;
          const raw = atob(message.sourceBase64);
          if (btoa(raw) !== message.sourceBase64) return false;
          source = new TextDecoder("utf-8", { fatal: true }).decode(Uint8Array.from(raw, char => char.charCodeAt(0)));
        } catch (_) { return false; }
        if (sourceError(source) || !integer(message.draftRevision, 0, Number.MAX_SAFE_INTEGER) ||
            (message.draftRevision === 0 && source) || message.sourceDigest !== options.hash(source)) return false;
        const receiptId = message.latestReceiptId || message.receiptId;
        if (receiptId && (!safeId(receiptId) || typeof message.receiptDigest !== "string" || !/^[0-9a-f]{64}$/.test(message.receiptDigest))) return false;
        const doc = document(intent.problemId, intent.language), name = documentKey(intent.problemId, intent.language);
        if (!data.finishRequested && (message.draftRevision > doc.revision || (doc.revision === 1 && !doc.source && !doc.undo.length))) {
          if (doc.source !== source) { doc.undo.push({ source: doc.source, start: doc.start, end: doc.end }); doc.redo = []; }
          doc.source = source; doc.revision = Math.max(1, message.draftRevision); recoveryConflicts.delete(name);
        } else if (!data.finishRequested && message.draftRevision === doc.revision && doc.source !== source) {
          recoveryConflicts.add(name); error = "Programming recovery source conflict. Local source is retained; edit this draft before sending it.";
          persist(); changed(); return false;
        } else if (!data.finishRequested && message.draftRevision === doc.revision && doc.source === source) recoveryConflicts.delete(name);
        if (receiptId) {
          data.receipts[name] = { sourceDigest: message.receiptDigest, receiptId, verified: true, serverTime: message.serverTime };
        }
        if (message.finalized === true) { data.finishRequested = true; data.finished = true; }
      }
      if (intent.operation === "submit" && ["received", "late"].includes(message.status)) {
        if (!safeId(message.receiptId) || !integer(message.serverTime, 1, Number.MAX_SAFE_INTEGER)) return false;
        data.receipts[documentKey(intent.problemId, intent.language)] = { sourceDigest: options.hash(intent.source), receiptId: message.receiptId, serverTime: message.serverTime, verified: true, late: message.status === "late" };
      }
      if (intent.operation === "ready") { ready = message.status === "ready" && message.runnerReady !== false; }
      if (intent.operation === "cancel" && message.status === "cancelled") {
        const active = runs.get(intent.problemId);
        if (active && active.requestId === intent.targetRunId) runs.set(intent.problemId, { ...active, cancelRequested: true });
      }
      if (intent.operation === "finalize" && message.status === "finalized") {
        if (!safeId(message.receiptId)) return false;
        data.finished = true;
      }
      if (["unavailable", "rejected", "conflict", "closed"].includes(message.status) || (message.status === "late" && intent.operation !== "submit")) error = typeof message.reason === "string" ? message.reason : message.status;
      else { pending.delete(message.requestId); sentRequests.delete(message.requestId); }
      persist(); maybeFinalize(); changed(); return true;
    }
    function finish() {
      if (data.finishRequested || data.finished) { maybeFinalize(); return false; }
      // Selected documents stay locked while durable source delivery is retried.
      for (const [id, run] of runs) if (run.status === "running") request("cancel", id, run.language);
      data.finishRequested = true;
      maybeFinalize(); changed(); return data.finished;
    }
    try {
      const stored = options.storage && key ? JSON.parse(options.storage.getItem(key) || "null") : null;
      if (stored && stored.context === contextText && bytes(JSON.stringify(stored)) <= MAX_STORE) {
        for (const [id] of problems) for (const language of LANGUAGES) {
          const name = documentKey(id, language), doc = stored.documents && stored.documents[name];
          if (object(doc) && !sourceError(doc.source) && integer(doc.revision, 1, Number.MAX_SAFE_INTEGER)) data.documents[name] = { source: doc.source, revision: doc.revision, start: doc.start || 0, end: doc.end || 0,
            undo: Array.isArray(doc.undo) ? doc.undo.filter(item => object(item) && !sourceError(item.source)).slice(-64) : [],
            redo: Array.isArray(doc.redo) ? doc.redo.filter(item => object(item) && !sourceError(item.source)).slice(-64) : [] };
          if (stored.selected && LANGUAGES.includes(stored.selected[id])) data.selected[id] = stored.selected[id];
          if (stored.samples && problems.get(id).samples.some(sample => sample.id === stored.samples[id])) data.samples[id] = stored.samples[id];
        }
        for (const intent of Array.isArray(stored.pending) ? stored.pending.slice(0, 256) : []) {
          if (object(intent) && safeId(intent.requestId) && JSON.stringify(intent.context) === contextText &&
              ["draft", "submit", "recover", "finalize", "ready"].includes(intent.operation) &&
              (["ready", "finalize"].includes(intent.operation) || validDocument(intent.problemId, intent.language)) &&
              (!Object.hasOwn(intent, "source") || !sourceError(intent.source))) pending.set(intent.requestId, intent);
        }
        data.finishRequested = stored.finishRequested === true;
        if (Object.hasOwn(stored, "recoveryConflicts")) {
          if (!Array.isArray(stored.recoveryConflicts) || stored.recoveryConflicts.length > problems.size * LANGUAGES.length ||
              stored.recoveryConflicts.some(name => typeof name !== "string" || !Object.hasOwn(data.documents, name)) ||
              new Set(stored.recoveryConflicts).size !== stored.recoveryConflicts.length) throw new Error("Invalid recovery conflict markers.");
          for (const name of stored.recoveryConflicts) recoveryConflicts.add(name);
          if (recoveryConflicts.size) error = "Programming recovery source conflict. Local source is retained; edit this draft before sending it.";
        }
        finalRequest = Array.from(pending.values()).find(intent => intent.operation === "finalize") || null;
        data.finished = false; saved = true;
      }
    } catch (_) {
      for (const id of problems.keys()) for (const language of LANGUAGES) recoveryConflicts.add(documentKey(id, language));
      error = "Programming recovery needs teacher assistance.";
    }
    return { document, update, undo, request, receive, finish, delivery,
      select(id, language) { if (!validDocument(id, language) || data.finishRequested) return false; data.selected[id] = language; persist(); changed(); return true; },
      selectSample(id, sample) { if (!problems.get(id).samples.some(item => item.id === sample)) return false; data.samples[id] = sample; persist(); changed(); return true; },
      selectedLanguage, selectedSample, runResult(id) { const run = runs.get(id); return run ? { ...run, stale: document(id, run.language).source !== run.source || selectedLanguage(id) !== run.language || selectedSample(id) !== run.sampleId } : null; },
      frozen: () => data.finishRequested || data.finished, finished: () => data.finished,
      localSaved: () => saved, error: () => error, runnerReady: () => ready,
      invalidateRunner() { ready = false; changed(); },
      retry() {
        const intents = Array.from(pending.values());
        if (!data.finished && !ready && !intents.some(intent => intent.operation === "ready")) request("ready");
        for (const intent of intents) {
          if (["run", "cancel"].includes(intent.operation) || (Object.hasOwn(intent, "source") && recoveryConflicts.has(documentKey(intent.problemId, intent.language)))) continue;
          if (intent.operation === "finalize" && Array.from(problems.keys()).some(id => recoveryConflicts.has(documentKey(id, selectedLanguage(id))))) continue;
          transmit(intent);
        }
        maybeFinalize();
      },
      recover() { for (const id of problems.keys()) for (const language of LANGUAGES) request("recover", id, language); request("ready"); },
      receipt(id, language) { return data.receipts[documentKey(id, language)] || null; } };
  }
  const COPY = {
    en: { language: "Language", run: "Run sample", submit: "Submit solution", sample: "Public sample", input: "Input", expected: "Expected output", output: "Program output", diagnostics: "Diagnostics", source: "Source code", search: "Find in source", find: "Find next", help: "Tab indents. Escape then Tab leaves the editor. Ctrl+S saves; Ctrl+Enter runs a public sample.", unavailable: "Sample execution is unavailable. Keep editing.", preview: "Teacher preview · Sample execution is unavailable in this preview.", hidden: "Results are hidden during this attempt.", saved: "Saved on this computer", unsaved: "Code is not saved", received: "Received", late: "Received after the deadline; not eligible for grading.", changed: "Draft changed since the last received solution.", sending: "Sending solution…", queued: "Queued on this computer", draft: "Draft", empty: "No draft", stopped: "Time ended. Final responses are pending delivery.", finished: "Attempt finished. The teacher received the final response set.", stale: "Result from an earlier draft", truncated: "Output display limit reached; output is incomplete.", running: "Running public sample…", constraints: "Constraints", explanation: "Explanation", inputDescription: "Input format", outputDescription: "Output format" },
    vi: { language: "Ngôn ngữ", run: "Chạy ví dụ", submit: "Nộp lời giải", sample: "Ví dụ công khai", input: "Dữ liệu vào", expected: "Kết quả mong đợi", output: "Kết quả chương trình", diagnostics: "Thông báo", source: "Mã nguồn", search: "Tìm trong mã nguồn", find: "Tìm tiếp", help: "Tab thụt lề. Escape rồi Tab để rời trình soạn thảo. Ctrl+S lưu; Ctrl+Enter chạy ví dụ.", unavailable: "Chưa hỗ trợ chạy ví dụ. Bạn có thể tiếp tục viết mã.", preview: "Bản xem trước của giáo viên · Chưa hỗ trợ chạy ví dụ.", hidden: "Kết quả được ẩn trong thời gian làm bài.", saved: "Đã lưu trên máy này", unsaved: "Mã nguồn chưa được lưu", received: "Đã nhận", late: "Đã nhận sau hạn; không đủ điều kiện chấm điểm.", changed: "Bản nháp đã thay đổi sau lời giải được nhận gần nhất.", sending: "Đang gửi lời giải…", queued: "Đã xếp hàng trên máy này", draft: "Bản nháp", empty: "Chưa có bản nháp", stopped: "Đã hết giờ. Bài làm cuối cùng đang chờ gửi.", finished: "Đã kết thúc. Giáo viên đã nhận bài làm cuối cùng.", stale: "Kết quả của bản nháp trước", truncated: "Đã chạm giới hạn hiển thị; kết quả chưa đầy đủ.", running: "Đang chạy ví dụ công khai…", constraints: "Giới hạn", explanation: "Giải thích", inputDescription: "Dữ liệu vào", outputDescription: "Dữ liệu ra" }
  };
  function copy(language, key) { return (COPY[language] || COPY.en)[key] || key; }
  function publicRunDescription(language, result) {
    if (!result) return "";
    if (result.status === "running") return result.cancelRequested ? (language === "vi" ? "Đã yêu cầu dừng ví dụ công khai." : "Public run stop requested.") : copy(language, "running");
    if (result.truncated) return copy(language, "truncated");
    if (result.status === "completed" && typeof result.sampleMatched === "boolean") return language === "vi" ? (result.sampleMatched ? "Kết quả khớp ví dụ công khai." : "Kết quả khác ví dụ công khai.") : (result.sampleMatched ? "Public sample output matches." : "Public sample output differs.");
    const labels = { en: { completed: "Public run completed.", compilation_error: "Public run compilation failed.", runtime_error: "Public sample did not finish.", timeout: "Public sample exceeded the run limit.", cancelled: "Public run stopped." },
      vi: { completed: "Đã hoàn tất chạy ví dụ công khai.", compilation_error: "Biên dịch để chạy ví dụ công khai thất bại.", runtime_error: "Ví dụ công khai không hoàn tất.", timeout: "Ví dụ công khai đã vượt thời gian chạy.", cancelled: "Đã dừng chạy ví dụ công khai." } };
    return (labels[language] || labels.en)[result.status] || copy(language, "unavailable");
  }
  function mountWorkspace(parent, session, problem, options) {
    const doc = parent.ownerDocument, nodes = [], editors = new Map();
    const make = (tag, className, text = "") => { const node = doc.createElement(tag); node.className = className || ""; node.textContent = text; return node; };
    const labelled = (node, key) => { nodes.push([node, key]); return node; };
    const root = make("section", "cs-problem"), statement = make("article", "cs-statement"), workspace = make("section", "cs-source-workspace");
    statement.appendChild(make("h2", "", problem.prompt));
    if (problem.instruction) statement.appendChild(make("p", "", problem.instruction));
    for (const field of ["inputDescription", "outputDescription", "constraints", "explanation"]) if (problem[field]) { statement.appendChild(labelled(make("h3"), field)); statement.appendChild(make("div", "cs-statement-text", problem[field])); }
    const toolbar = make("div", "cs-toolbar"), languageLabel = labelled(make("label"), "language"), language = make("select", "cs-language");
    language.id = `cs-language-${problem.id}`; languageLabel.htmlFor = language.id;
    for (const [value, title] of [["cpp20", "C++20"], ["python3", "Python 3"]]) { const option = make("option", "", title); option.value = value; language.appendChild(option); }
    const filename = make("span", "cs-filename"), run = labelled(make("button", "secondary-button"), "run"), submit = labelled(make("button", "primary-button"), "submit");
    run.type = submit.type = "button";
    toolbar.append(languageLabel, language, filename, run, submit); workspace.appendChild(toolbar);
    const help = labelled(make("p", "cs-editor-help"), "help"); help.id = `cs-help-${problem.id}`;
    const buffers = make("div", "cs-buffers");
    for (const codeLanguage of LANGUAGES) {
      const area = make("textarea", "cs-source"); area.spellcheck = false; area.autocapitalize = "off"; area.autocomplete = "off"; area.wrap = "off";
      area.setAttribute("aria-describedby", help.id); area.value = session.document(problem.id, codeLanguage).source;
      let composing = false, escapeTab = false, timer = 0;
      const capture = () => session.update(problem.id, codeLanguage, area.value, { start: area.selectionStart, end: area.selectionEnd });
      area.addEventListener("compositionstart", () => { composing = true; });
      area.addEventListener("beforeinput", () => { if (!composing) { const model = session.document(problem.id, codeLanguage); model.start = area.selectionStart; model.end = area.selectionEnd; } });
      area.addEventListener("compositionend", () => { composing = false; if (!session.frozen()) { capture(); session.request("draft", problem.id, codeLanguage); } });
      area.addEventListener("input", () => { if (composing || session.frozen()) return; capture(); clearTimeout(timer); timer = setTimeout(() => session.request("draft", problem.id, codeLanguage), 400); });
      area.addEventListener("keydown", event => {
        if (composing || event.isComposing || session.frozen()) return;
        if (event.key === "Escape") { escapeTab = true; return; }
        if (event.key === "Tab" && !event.shiftKey && !escapeTab) { event.preventDefault(); const start = area.selectionStart, end = area.selectionEnd; area.setRangeText("\t", start, end, "end"); capture(); return; }
        escapeTab = false;
        if (event.ctrlKey && event.key.toLowerCase() === "s") { event.preventDefault(); capture(); session.request("draft", problem.id, codeLanguage); }
        if (event.ctrlKey && ["z", "y"].includes(event.key.toLowerCase())) { event.preventDefault(); const value = session.undo(problem.id, codeLanguage, event.key.toLowerCase() === "y" || event.shiftKey); if (value) { area.value = value.source; area.setSelectionRange(value.start, value.end); } }
        if (event.ctrlKey && event.key === "Enter" && !event.shiftKey && session.runnerReady()) { event.preventDefault(); capture(); session.request("run", problem.id, codeLanguage, session.selectedSample(problem.id)); }
      });
      editors.set(codeLanguage, { area, capture, composing: () => composing }); buffers.appendChild(area);
    }
    workspace.append(buffers, help);
    const searchRow = make("div", "cs-search"), searchLabel = labelled(make("label"), "search"), search = make("input"), find = labelled(make("button", "quiet-button"), "find");
    search.type = "search"; search.id = `cs-search-${problem.id}`; searchLabel.htmlFor = search.id; find.type = "button";
    find.addEventListener("click", () => { const area = editors.get(session.selectedLanguage(problem.id)).area, term = search.value; if (!term) return; let index = area.value.indexOf(term, area.selectionEnd); if (index < 0) index = area.value.indexOf(term); if (index >= 0) { area.focus(); area.setSelectionRange(index, index + term.length); } });
    searchRow.append(searchLabel, search, find); workspace.appendChild(searchRow);
    const sampleLabel = labelled(make("label"), "sample"), samples = make("select", "cs-samples"); samples.id = `cs-samples-${problem.id}`; sampleLabel.htmlFor = samples.id;
    problem.samples.forEach((sample, index) => { const option = make("option", "", `Sample ${index + 1}`); option.value = sample.id; samples.appendChild(option); });
    const consoleBox = make("section", "cs-console"), sampleRow = make("div", "cs-toolbar"); sampleRow.append(sampleLabel, samples); consoleBox.appendChild(sampleRow);
    const fields = {};
    for (const key of ["input", "expected", "output", "diagnostics"]) { const block = make("section", "cs-console-section"); block.appendChild(labelled(make("h3"), key)); const pre = make("pre"); block.appendChild(pre); fields[key] = pre; consoleBox.appendChild(block); }
    const status = make("p", "cs-status"); status.setAttribute("role", "status"); status.setAttribute("aria-live", "polite"); workspace.append(consoleBox, status); root.append(statement, workspace); parent.appendChild(root);
    language.addEventListener("change", () => { const selected = language.value, current = editors.get(session.selectedLanguage(problem.id)); if (current.composing()) { language.value = session.selectedLanguage(problem.id); return; } current.capture(); session.select(problem.id, selected); refresh(); });
    samples.addEventListener("change", () => { session.selectSample(problem.id, samples.value); refresh(); });
    run.addEventListener("click", () => { const current = editors.get(session.selectedLanguage(problem.id)), active = session.runResult(problem.id); if (active && active.status === "running") { session.request("cancel", problem.id, active.language); return; } if (!current.composing() && session.runnerReady()) { current.capture(); session.request("run", problem.id, session.selectedLanguage(problem.id), session.selectedSample(problem.id)); } });
    submit.addEventListener("click", () => { const current = editors.get(session.selectedLanguage(problem.id)); if (!current.composing()) { current.capture(); session.request("submit", problem.id, session.selectedLanguage(problem.id)); } });
    function refresh() {
      const ui = options.language(), selected = session.selectedLanguage(problem.id), sample = problem.samples.find(item => item.id === session.selectedSample(problem.id)) || problem.samples[0];
      nodes.forEach(([node, key]) => { node.textContent = copy(ui, key); }); language.value = selected; samples.value = sample.id; filename.textContent = selected === "cpp20" ? "main.cpp" : "main.py";
      for (const [codeLanguage, editor] of editors) { editor.area.hidden = codeLanguage !== selected; editor.area.disabled = options.started() !== true || session.frozen(); editor.area.setAttribute("aria-label", `${copy(ui, "source")} · ${codeLanguage === "cpp20" ? "main.cpp" : "main.py"}`); const model = session.document(problem.id, codeLanguage); if (!editor.composing() && editor.area.value !== model.source) editor.area.value = model.source; }
      run.disabled = options.preview || !session.runnerReady() || options.started() !== true || session.frozen(); submit.disabled = options.preview || options.started() !== true || session.frozen(); language.disabled = session.frozen();
      fields.input.textContent = sample.input; fields.expected.textContent = sample.output;
      const result = session.runResult(problem.id); fields.output.textContent = result ? result.stdout || "" : ""; fields.diagnostics.textContent = result ? [result.stderr, result.diagnostics].filter(Boolean).join("\n") : "";
      if (result && result.status === "running") run.textContent = ui === "vi" ? "Dừng chạy" : "Stop run";
      if (result && result.status === "running" && result.cancelRequested) { run.textContent = ui === "vi" ? "Đã yêu cầu dừng" : "Stop requested"; run.disabled = true; }
      if (result && result.status === "completed" && typeof result.sampleMatched === "boolean" && !result.truncated) fields.output.setAttribute("aria-label", ui === "vi" ? (result.sampleMatched ? "Kết quả khớp ví dụ công khai" : "Kết quả khác ví dụ công khai") : (result.sampleMatched ? "Public sample output matches" : "Public sample output differs"));
      const receipt = session.receipt(problem.id, selected);
      const staleText = result && result.stale ? (ui === "vi" ? "Kết quả của bản nháp hoặc ví dụ trước." : "Result from an earlier draft or sample.") : "";
      const message = [options.preview ? copy(ui, "preview") : session.finished() ? copy(ui, "finished") : session.frozen() ? copy(ui, "stopped") : copy(ui, session.localSaved() ? "saved" : "unsaved"),
        copy(ui, session.delivery(problem.id, selected)), receipt && receipt.verified ? new Date(receipt.serverTime).toLocaleTimeString(ui === "vi" ? "vi-VN" : "en-US") : "",
        copy(ui, "hidden"), !session.runnerReady() ? copy(ui, "unavailable") : "", publicRunDescription(ui, result), staleText, session.error()].filter(Boolean).join(" · ");
      if (status.textContent !== message) status.textContent = message;
    }
    refresh();
    return { root, refresh, capture() { return editors.get(session.selectedLanguage(problem.id)).capture(); }, composing() { return editors.get(session.selectedLanguage(problem.id)).composing(); } };
  }
  window.NstuProgramming = { LANGUAGES, MAX_SOURCE, sourceError, validatePublicManifest, projectPublicManifest, createSession, mountWorkspace, copy };
}());
