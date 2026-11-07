/**
 * NSTU Exam Studio - Authoring Engine & IELTS Split-Screen Controller
 */

(function () {
  "use strict";

  // Default sample exam modeled after computer-delivered IELTS
  const DEFAULT_EXAM = {
    id: "ielts-sample-01",
    title: "IELTS Academic Reading & Listening Mock Assessment",
    subject: "English Language",
    durationSeconds: 3600, // 60 minutes
    documents: [],
    questions: [
      {
        id: "q1-matching-headings",
        type: "reading",
        points: 4,
        prompt: "Choose the correct heading for Paragraphs A–D from the list of headings below.",
        instruction: "Drag each heading into the appropriate paragraph answer slot, or click a heading then click a slot to place it.",
        passage: "[Paragraph A]\nThe initial exploration of the southern polar continent began in the early nineteenth century with maritime expeditions probing the pack ice. Early navigators braved uncharted waters, fierce katabatic winds, and freezing temperatures without reliable navigational charts.\n\n[Paragraph B]\nEquipping polar expeditions proved to be the single greatest logistical challenge. Teams required specialized high-calorie rations, draft animals such as Manchurian ponies and Greenland sled dogs, and windproof canvas tents capable of withstanding blizzards.\n\n[Paragraph C]\nBy the mid-twentieth century, seasonal exploration gave way to permanent scientific habitations. Modern research outposts now maintain sophisticated meteorological stations, satellite tracking dishes, and subglacial laboratories operating year-round.\n\n[Paragraph D]\nInternational diplomacy culminated in the landmark Antarctic Treaty, signed in Washington in 1959. The treaty established the continent as a scientific preserve, strictly banned all military activity, and postponed indefinitely all national territorial claims.",
        isMatching: true,
        options: [
          "i. Early maritime voyages and extreme survival conditions",
          "ii. Logistical hurdles and specialized expedition gear",
          "iii. Permanent research stations and scientific facilities",
          "iv. Diplomatic agreements and international stewardship",
          "v. Commercial mining and exploitation of subglacial fuel reserves"
        ],
        matchingTargets: [
          "Paragraph A",
          "Paragraph B",
          "Paragraph C",
          "Paragraph D"
        ]
      },
      {
        id: "q2-reading-mc",
        type: "reading",
        points: 1,
        prompt: "According to Paragraph B, what was the primary obstacle facing early expedition planners?",
        instruction: "Choose the correct letter, A, B, C, or D.",
        passage: "[Paragraph A]\nThe initial exploration of the southern polar continent began in the early nineteenth century with maritime expeditions probing the pack ice...\n\n[Paragraph B]\nEquipping polar expeditions proved to be the single greatest logistical challenge. Teams required specialized high-calorie rations, draft animals such as Manchurian ponies and Greenland sled dogs, and windproof canvas tents capable of withstanding blizzards.",
        isMatching: false,
        options: [
          "A. Securing government permission to sail into Antarctic waters",
          "B. Procuring sufficient provisions, specialized equipment, and draft animals",
          "C. Overcoming language barriers between international expedition members",
          "D. Navigating through densely populated sub-Antarctic islands"
        ]
      },
      {
        id: "q3-listening-announcement",
        type: "listening",
        points: 2,
        prompt: "Listen to the polar station orientation briefing and answer the question.",
        instruction: "What mandatory procedure must all researchers complete before leaving the base perimeter?",
        audio: "media/listening-01.mp3",
        options: [
          "Sign the radio logbook and carry a dual-frequency transceiver",
          "Obtain written authorization from the expedition commander",
          "Wait for wind speeds to drop below five knots",
          "Check out an emergency snowmobile with auxiliary fuel tanks"
        ]
      },
      {
        id: "q4-academic-writing",
        type: "essay",
        points: 8,
        prompt: "Some individuals believe that extreme wilderness regions like Antarctica should be completely closed to human activity, while others maintain that scientific exploration brings vital knowledge for planet Earth. Discuss both views and give your own opinion.",
        instruction: "Write at least 250 words in clear academic English.",
        wordLimit: 300,
        answerPlaceholder: "Type your essay response here..."
      }
    ]
  };

  // State
  let currentExam = JSON.parse(JSON.stringify(DEFAULT_EXAM));
  let activeQuestionIndex = 0;
  let previewAnswers = {}; // { qId: { targetIdx: tokenIdx } or value }
  let selectedTokenForPlacement = null; // For click-to-place fallback

  // DOM Elements
  const el = (id) => document.getElementById(id);

  function showToast(message, type = "info") {
    const container = el("toast-container");
    const toast = document.createElement("div");
    toast.className = `toast ${type}`;
    toast.textContent = message;
    container.appendChild(toast);
    setTimeout(() => {
      toast.style.opacity = "0";
      setTimeout(() => toast.remove(), 250);
    }, 3200);
  }

  /* ========================================================================
     Navigation & Mode Switching
     ======================================================================== */

  function setMode(mode) {
    const editorView = el("editor-view");
    const previewView = el("preview-view");
    const btnEditor = el("btn-mode-editor");
    const btnPreview = el("btn-mode-preview");

    if (mode === "preview") {
      syncExamMetadata();
      editorView.style.display = "none";
      previewView.style.display = "flex";
      btnEditor.classList.remove("active");
      btnPreview.classList.add("active");
      renderPreview();
    } else {
      previewView.style.display = "none";
      editorView.style.display = "grid";
      btnPreview.classList.remove("active");
      btnEditor.classList.add("active");
      renderEditor();
    }
  }

  /* ========================================================================
     Editor: Render and Synchronize
     ======================================================================== */

  function renderEditor() {
    // Bind exam settings
    el("exam-id").value = currentExam.id || "";
    el("exam-title").value = currentExam.title || "";
    el("exam-subject").value = currentExam.subject || "";
    renderNumericField("exam-duration");
    el("exam-allowed-origins").value = Array.isArray(currentExam.allowedOrigins)
      ? currentExam.allowedOrigins.join("\n")
      : "";

    // Questions rail
    const navList = el("question-nav-list");
    navList.innerHTML = "";
    el("question-count-badge").textContent = currentExam.questions.length;

    currentExam.questions.forEach((q, idx) => {
      const item = document.createElement("div");
      item.className = `question-nav-item ${idx === activeQuestionIndex ? "active" : ""}`;
      
      let badgeType = q.type;
      let badgeLabel = q.type;
      if (q.isMatching) {
        badgeType = "matching";
        badgeLabel = "Matching";
      } else if (q.type === "multiple_choice") {
        badgeLabel = "MC";
      } else if (q.type === "short_answer") {
        badgeLabel = "Short";
      } else if (q.type === "essay") {
        badgeLabel = "Essay";
      } else if (q.type === "listening") {
        badgeLabel = "Audio";
      } else if (q.type === "reading") {
        badgeLabel = "Reading";
      }

      item.innerHTML = `
        <button class="nav-item-left question-select" type="button" aria-current="${idx === activeQuestionIndex}" aria-label="${escapeHtml(questionButtonName(q, idx))}">
          <span class="nav-item-index">${idx + 1}</span>
          <span class="nav-item-type-badge ${badgeType}">${badgeLabel}</span>
          <span class="nav-item-title">${escapeHtml(q.prompt || "Untitled Question")}</span>
        </button>
        <div class="nav-item-actions">
          <button class="icon-btn delete" type="button" title="Delete question" aria-label="Delete question ${idx + 1}" data-delete="${idx}">🗑️</button>
        </div>
      `;

      item.addEventListener("click", (e) => {
        const previousFocus = document.activeElement;
        const railOwnedFocus = item.contains(previousFocus);
        syncExamMetadata();
        if (e.target.closest("[data-delete]")) {
          deleteQuestion(idx);
        } else {
          activeQuestionIndex = idx;
          renderEditor();
        }
        if (railOwnedFocus && !previousFocus.isConnected) {
          const selected = el("question-nav-list").children[activeQuestionIndex]?.querySelector(".question-select");
          selected?.focus({ preventScroll: true });
          selected?.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "instant" });
        }
      });

      navList.appendChild(item);
    });

    renderActiveQuestionForm();
  }

  function questionButtonName(q, index) {
    const type = q.isMatching ? "Matching" : {
      multiple_choice: "Multiple choice", short_answer: "Short answer",
      essay: "Essay", listening: "Listening", reading: "Reading"
    }[q.type];
    return `Question ${index + 1}: ${type}. ${q.prompt || "Untitled Question"}`;
  }

  function renderActiveQuestionForm() {
    const q = currentExam.questions[activeQuestionIndex];
    if (!q) return;

    el("active-q-title").textContent = `Question ${activeQuestionIndex + 1}`;
    renderNumericField("q-points", q);
    el("q-prompt").value = q.prompt || "";
    el("q-instruction").value = q.instruction || "";

    // Badge
    const badge = el("active-q-badge");
    badge.className = "nav-item-type-badge " + (q.isMatching ? "matching" : q.type);
    badge.textContent = q.isMatching ? "Matching (IELTS Drag & Drop)" : q.type.toUpperCase();

    // Stimulus (Passage)
    const stimulus = el("stimulus-container");
    if (q.type === "reading" || q.isMatching) {
      stimulus.style.display = "block";
      el("q-passage").value = q.passage || "";
    } else {
      stimulus.style.display = "none";
    }

    // Audio container
    const audioContainer = el("audio-container");
    if (q.type === "listening") {
      audioContainer.style.display = "block";
      el("q-audio-path").value = q.audio || "";
    } else {
      audioContainer.style.display = "none";
    }

    // Type sections
    el("matching-editor").style.display = q.isMatching ? "grid" : "none";
    el("mc-editor").style.display = (!q.isMatching && (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading")) ? "block" : "none";
    el("sa-editor").style.display = (q.type === "short_answer") ? "block" : "none";
    el("essay-editor").style.display = (q.type === "essay") ? "block" : "none";

    // Populate matching tokens & targets
    if (q.isMatching) {
      renderTokenEditor(q);
    } else if (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading") {
      renderMcOptionEditor(q);
    } else if (q.type === "short_answer") {
      el("q-sa-placeholder").value = q.answerPlaceholder || "";
    } else if (q.type === "essay") {
      renderNumericField("q-essay-limit", q);
    }
  }

  function renderTokenEditor(q) {
    if (!Array.isArray(q.options)) q.options = [];
    if (!Array.isArray(q.matchingTargets)) q.matchingTargets = [];

    // Tokens list
    const tokenList = el("token-input-list");
    tokenList.innerHTML = "";
    q.options.forEach((opt, idx) => {
      const row = document.createElement("div");
      row.className = "chip-input-row";
      row.innerHTML = `
        <span class="chip-badge-num">#${idx + 1}</span>
        <input class="form-input" type="text" value="${escapeHtml(opt)}" data-token-idx="${idx}">
        <button class="icon-btn delete" type="button" data-del-token="${idx}">✕</button>
      `;
      row.querySelector("input").addEventListener("input", (e) => {
        q.options[idx] = e.target.value;
      });
      row.querySelector("[data-del-token]").addEventListener("click", () => {
        q.options.splice(idx, 1);
        renderTokenEditor(q);
      });
      tokenList.appendChild(row);
    });

    // Targets list
    const targetList = el("target-input-list");
    targetList.innerHTML = "";
    q.matchingTargets.forEach((tgt, idx) => {
      const row = document.createElement("div");
      row.className = "chip-input-row";
      row.innerHTML = `
        <span class="chip-badge-num">Target ${idx + 1}</span>
        <input class="form-input" type="text" value="${escapeHtml(tgt)}" data-target-idx="${idx}">
        <button class="icon-btn delete" type="button" data-del-target="${idx}">✕</button>
      `;
      row.querySelector("input").addEventListener("input", (e) => {
        q.matchingTargets[idx] = e.target.value;
      });
      row.querySelector("[data-del-target]").addEventListener("click", () => {
        q.matchingTargets.splice(idx, 1);
        renderTokenEditor(q);
      });
      targetList.appendChild(row);
    });
  }

  function renderMcOptionEditor(q) {
    if (!Array.isArray(q.options)) q.options = [];
    const list = el("mc-option-list");
    list.innerHTML = "";
    q.options.forEach((opt, idx) => {
      const letter = String.fromCharCode(65 + idx);
      const row = document.createElement("div");
      row.className = "chip-input-row";
      row.innerHTML = `
        <span class="chip-badge-num">${letter}.</span>
        <input class="form-input" type="text" value="${escapeHtml(opt)}" data-mc-idx="${idx}">
        <button class="icon-btn delete" type="button" data-del-mc="${idx}">✕</button>
      `;
      row.querySelector("input").addEventListener("input", (e) => {
        q.options[idx] = e.target.value;
      });
      row.querySelector("[data-del-mc]").addEventListener("click", () => {
        q.options.splice(idx, 1);
        renderMcOptionEditor(q);
      });
      list.appendChild(row);
    });
  }

  function addQuestion(type) {
    syncExamMetadata();
    const nextNum = currentExam.questions.length + 1;
    let newQ;

    if (type === "matching") {
      newQ = {
        id: `q${nextNum}-matching`,
        type: "reading",
        points: 4,
        prompt: `Questions ${nextNum}: Match each heading to the appropriate section.`,
        instruction: "Drag each heading into the target slot, or click to place.",
        passage: `[Paragraph A]\nInsert your reading passage here...\n\n[Paragraph B]\nSecond paragraph text...`,
        isMatching: true,
        options: [
          "i. First heading description",
          "ii. Second heading description",
          "iii. Third heading description"
        ],
        matchingTargets: [
          "Paragraph A",
          "Paragraph B"
        ]
      };
    } else if (type === "reading") {
      newQ = {
        id: `q${nextNum}-reading`,
        type: "reading",
        points: 1,
        prompt: `According to the passage, what is the author's primary conclusion?`,
        instruction: "Choose one answer.",
        passage: `[Paragraph A]\nInsert your reading passage here...`,
        isMatching: false,
        options: ["Option A", "Option B", "Option C", "Option D"]
      };
    } else if (type === "multiple_choice") {
      newQ = {
        id: `q${nextNum}-mc`,
        type: "multiple_choice",
        points: 1,
        prompt: "Choose the statement that best answers the question.",
        instruction: "Select one option.",
        options: ["Option A", "Option B", "Option C", "Option D"]
      };
    } else if (type === "short_answer") {
      newQ = {
        id: `q${nextNum}-short-answer`,
        type: "short_answer",
        points: 1,
        prompt: "Complete the sentence with NO MORE THAN TWO WORDS.",
        instruction: "Write your answer in the box below.",
        answerPlaceholder: "Type your answer here..."
      };
    } else if (type === "essay") {
      newQ = {
        id: `q${nextNum}-essay`,
        type: "essay",
        points: 8,
        prompt: "Discuss the advantages and disadvantages of this development.",
        instruction: "Write at least 250 words.",
        wordLimit: 250,
        answerPlaceholder: "Write your essay response here..."
      };
    } else if (type === "listening") {
      newQ = {
        id: `q${nextNum}-listening`,
        type: "listening",
        points: 1,
        prompt: "Listen to the recording and choose the correct answer.",
        instruction: "Select one option.",
        audio: "media/listening-01.mp3",
        options: ["Option A", "Option B", "Option C", "Option D"]
      };
    }

    const idSuffix = newQ.id.slice(newQ.id.indexOf("-"));
    let idNumber = nextNum;
    while (currentExam.questions.some(q => q.id === newQ.id) ||
           Object.prototype.hasOwnProperty.call(previewAnswers, newQ.id)) {
      newQ.id = `q${++idNumber}${idSuffix}`;
    }
    currentExam.questions.push(newQ);
    activeQuestionIndex = currentExam.questions.length - 1;
    renderEditor();
    showToast(`Added Question ${nextNum}`, "success");
  }

  function deleteQuestion(idx) {
    if (currentExam.questions.length <= 1) {
      showToast("An exam must have at least one question.", "error");
      return;
    }
    currentExam.questions.splice(idx, 1);
    if (activeQuestionIndex >= currentExam.questions.length) {
      activeQuestionIndex = currentExam.questions.length - 1;
    }
    renderEditor();
    showToast("Question deleted", "info");
  }

  /* ========================================================================
     IELTS Live Preview: Split Screen & Drag-and-Drop
     ======================================================================== */

  let previewIndex = 0;

  function resetPreviewState() {
    previewIndex = 0;
    previewAnswers = {};
    selectedTokenForPlacement = null;
    if (el("preview-view").style.display === "flex") renderPreview();
  }

  function renderPreview() {
    el("preview-exam-title").textContent = currentExam.title || "IELTS Assessment Preview";
    const minutes = Math.round((currentExam.durationSeconds || 3600) / 60);
    el("preview-timer").textContent = `${String(minutes).padStart(2, "0")}:00`;

    const q = currentExam.questions[previewIndex];
    if (!q) return;

    el("p-nav-counter").textContent = `Question ${previewIndex + 1} of ${currentExam.questions.length}`;
    el("btn-p-prev").disabled = previewIndex === 0;
    el("btn-p-next").disabled = previewIndex === currentExam.questions.length - 1;

    // Header info
    el("p-question-badge").textContent = q.isMatching ? "IELTS Matching" : q.type.toUpperCase();
    el("p-question-points").textContent = `${q.points ?? 1} points`;
    el("p-question-prompt").textContent = q.prompt || "";
    el("p-question-instruction").textContent = q.instruction || "";

    // Left Column: Stimulus Passage
    const passageContent = el("p-passage-content");
    const audioContainer = el("p-audio-container");
    const leftPane = el("preview-pane-left");
    const referenceLabel = !q.passage && q.audio ? "Audio reference" : "Reference material";
    el("p-passage-heading").textContent = referenceLabel;
    el("p-passage-sub").textContent = q.passage || q.audio
      ? `Question ${previewIndex + 1} of ${currentExam.questions.length}` : "";
    leftPane.setAttribute("aria-label", referenceLabel);

    if (q.passage) {
      leftPane.style.display = "block";
      passageContent.innerHTML = formatIeltsParagraphs(q.passage);
    } else if (q.audio) {
      leftPane.style.display = "block";
      passageContent.innerHTML = `<p style="color: var(--text-muted); font-style: italic;">Listen to the audio track below while completing the questions on the right.</p>`;
    } else {
      // If there's no passage/stimulus, expand the right column
      leftPane.style.display = "none";
    }

    if (q.audio) {
      audioContainer.style.display = "block";
      el("p-audio-player").src = q.audio;
    } else {
      audioContainer.style.display = "none";
    }

    // Right Column: Interactive Answer Surface
    const matchingSurface = el("p-matching-surface");
    const mcSurface = el("p-mc-surface");
    const saSurface = el("p-sa-surface");
    const essaySurface = el("p-essay-surface");

    matchingSurface.style.display = "none";
    mcSurface.style.display = "none";
    saSurface.style.display = "none";
    essaySurface.style.display = "none";

    if (q.isMatching) {
      matchingSurface.style.display = "block";
      renderPreviewMatching(q);
    } else if (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading") {
      mcSurface.style.display = "flex";
      renderPreviewMc(q);
    } else if (q.type === "short_answer") {
      saSurface.style.display = "block";
      const input = el("p-sa-input");
      input.placeholder = q.answerPlaceholder || "Type your answer here...";
      input.value = previewAnswers[q.id] || "";
      input.oninput = (e) => { previewAnswers[q.id] = e.target.value; };
    } else if (q.type === "essay") {
      essaySurface.style.display = "block";
      const area = el("p-essay-input");
      area.value = previewAnswers[q.id] || "";
      updateEssayWordCount(area.value, q.wordLimit);
      area.oninput = (e) => {
        previewAnswers[q.id] = e.target.value;
        updateEssayWordCount(e.target.value, q.wordLimit);
      };
    }
  }

  function formatIeltsParagraphs(text) {
    if (!text) return "";
    const lines = text.split(/\n\s*\n/);
    return lines.map(line => {
      const trimmed = line.trim();
      if (!trimmed) return "";
      // Check for [Paragraph A] or [A] marker
      const markerMatch = trimmed.match(/^\[(Paragraph\s+[A-Za-z0-9]+|[A-Za-z0-9]+)\]\s*(.*)/i);
      if (markerMatch) {
        return `<p class="ielts-paragraph"><span class="paragraph-marker">${escapeHtml(markerMatch[1])}</span>${escapeHtml(markerMatch[2])}</p>`;
      }
      return `<p class="ielts-paragraph">${escapeHtml(trimmed)}</p>`;
    }).join("");
  }

  function updateEssayWordCount(text, limit) {
    const words = text.trim() ? text.trim().split(/\s+/).length : 0;
    el("p-essay-count").textContent = `${words}${limit ? ` / ${limit}` : ""} words`;
  }

  /* ========================================================================
     Drag-and-Drop & Matching Engine (with Click-to-Place Fallback)
     ======================================================================== */

  function renderPreviewMatching(q) {
    if (!previewAnswers[q.id]) {
      previewAnswers[q.id] = {}; // { targetIdx: tokenIdx }
    }
    const currentAssignments = previewAnswers[q.id];

    // Identify which tokens are already placed
    const placedTokenIndices = new Set(Object.values(currentAssignments));

    // 1. Render Token Tray
    const tray = el("p-token-tray");
    tray.innerHTML = "";

    q.options.forEach((optText, tokenIdx) => {
      if (placedTokenIndices.has(tokenIdx)) return; // Already placed in a slot

      const chip = document.createElement("button");
      chip.className = "drag-chip";
      chip.type = "button";
      chip.draggable = true;
      chip.dataset.tokenIdx = tokenIdx;
      chip.setAttribute("aria-label", `Heading ${tokenIdx + 1}: ${optText}`);
      chip.setAttribute("aria-pressed", String(selectedTokenForPlacement === tokenIdx));
      chip.innerHTML = `
        <span class="chip-handle-icon">⠿</span>
        <span>${escapeHtml(optText)}</span>
      `;

      if (selectedTokenForPlacement === tokenIdx) {
        chip.classList.add("selected-for-click");
      }

      // Drag events
      chip.addEventListener("dragstart", (e) => {
        chip.classList.add("is-dragging");
        e.dataTransfer.setData("text/plain", String(tokenIdx));
        e.dataTransfer.effectAllowed = "move";
      });

      chip.addEventListener("dragend", () => {
        chip.classList.remove("is-dragging");
      });

      // Click to select (fallback for trackpad / touch)
      chip.addEventListener("click", () => {
        const previousFocus = chip.contains(document.activeElement) ? document.activeElement : null;
        if (selectedTokenForPlacement === tokenIdx) {
          selectedTokenForPlacement = null;
        } else {
          selectedTokenForPlacement = tokenIdx;
        }
        renderPreviewMatching(q);
        restoreMatchingFocus(q, "token", tokenIdx, previousFocus);
      });

      tray.appendChild(chip);
    });

    if (tray.children.length === 0) {
      tray.innerHTML = `<span style="font-size: 12px; color: var(--text-faint); padding: 8px;">All headings have been placed in slots.</span>`;
    }

    // 2. Render Target Slots
    const targetList = el("p-target-list");
    targetList.innerHTML = "";

    (q.matchingTargets || []).forEach((tgtLabel, targetIdx) => {
      const row = document.createElement("div");
      row.className = "target-row";

      const labelBox = document.createElement("div");
      labelBox.className = "target-label-box";
      labelBox.textContent = tgtLabel;

      const slot = document.createElement("div");
      slot.className = "target-drop-slot";
      slot.dataset.targetIdx = targetIdx;

      const assignedTokenIdx = currentAssignments[targetIdx];
      const activation = document.createElement("button");
      activation.type = "button";
      activation.className = "target-select";
      const hasAssignment = assignedTokenIdx !== undefined && assignedTokenIdx !== null;
      const tokenText = hasAssignment ? q.options[assignedTokenIdx] || "" : "";
      activation.setAttribute("aria-label", `Target ${targetIdx + 1}: ${tgtLabel}. ${hasAssignment ? `Assigned heading ${assignedTokenIdx + 1}: ${tokenText}` : "Unassigned"}`);

      if (assignedTokenIdx !== undefined && assignedTokenIdx !== null) {
        // Slot is filled
        slot.classList.add("filled");
        const placed = document.createElement("div");
        placed.className = "placed-chip";
        const text = document.createElement("span");
        text.textContent = tokenText;
        activation.appendChild(text);
        const removal = document.createElement("button");
        removal.className = "chip-remove-btn";
        removal.type = "button";
        removal.title = "Remove heading back to tray";
        removal.textContent = "✕";
        removal.setAttribute("aria-label", `Remove heading from target ${targetIdx + 1}: ${tgtLabel}`);
        removal.addEventListener("click", (e) => {
          const previousFocus = slot.contains(document.activeElement) ? document.activeElement : null;
          e.stopPropagation();
          delete currentAssignments[targetIdx];
          renderPreviewMatching(q);
          restoreMatchingFocus(q, "target", targetIdx, previousFocus);
        });
        placed.append(activation, removal);
        slot.appendChild(placed);
      } else {
        // Slot is empty
        const placeholder = document.createElement("span");
        placeholder.className = "slot-placeholder-text";
        placeholder.textContent = "Drop heading here or click to assign";
        activation.appendChild(placeholder);
        slot.appendChild(activation);
      }

      // Drag Over / Leave
      slot.addEventListener("dragover", (e) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = "move";
        slot.classList.add("drag-over");
      });

      slot.addEventListener("dragleave", () => {
        slot.classList.remove("drag-over");
      });

      // Drop
      slot.addEventListener("drop", (e) => {
        const active = document.activeElement;
        const previousFocus = tray.contains(active) || targetList.contains(active) ? active : null;
        e.preventDefault();
        slot.classList.remove("drag-over");
        const tokenIdxStr = e.dataTransfer.getData("text/plain");
        if (tokenIdxStr !== "") {
          const tIdx = parseInt(tokenIdxStr, 10);
          assignTokenToSlot(q, targetIdx, tIdx);
          restoreMatchingFocus(q, "target", targetIdx, previousFocus);
        }
      });

      // Click to place
      slot.addEventListener("click", () => {
        const previousFocus = slot.contains(document.activeElement) ? document.activeElement : null;
        if (selectedTokenForPlacement !== null) {
          assignTokenToSlot(q, targetIdx, selectedTokenForPlacement);
          selectedTokenForPlacement = null;
        } else if (assignedTokenIdx !== undefined) {
          // If clicked on an already filled slot without a token selected, remove it
          delete currentAssignments[targetIdx];
          renderPreviewMatching(q);
        }
        restoreMatchingFocus(q, "target", targetIdx, previousFocus);
      });

      row.append(labelBox, slot);
      targetList.appendChild(row);
    });
  }

  function restoreMatchingFocus(q, kind, index, previousFocus) {
    if (!previousFocus || previousFocus.isConnected || previousFocus.ownerDocument !== document ||
        currentExam.questions[previewIndex] !== q || getComputedStyle(el("preview-view")).display === "none" ||
        el("export-modal").classList.contains("open")) return;
    const target = kind === "token"
      ? Array.from(el("p-token-tray").children).find(node => Number(node.dataset.tokenIdx) === index)
      : Array.from(el("p-target-list").children).map(row => row.children[1])
        .find(node => Number(node?.dataset.targetIdx) === index)?.querySelector(".target-select");
    if (usableExportFocus(target)) {
      target.focus({ preventScroll: true });
      target.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "instant" });
    }
  }

  function assignTokenToSlot(q, targetIdx, tokenIdx) {
    const assignments = previewAnswers[q.id];
    // If this token was already in another slot, remove it from that slot first
    for (const [sKey, tVal] of Object.entries(assignments)) {
      if (tVal === tokenIdx) {
        delete assignments[sKey];
      }
    }
    assignments[targetIdx] = tokenIdx;
    renderPreviewMatching(q);
  }

  function renderPreviewMc(q) {
    const surface = el("p-mc-surface");
    surface.innerHTML = "";
    surface.setAttribute("role", "radiogroup");
    surface.setAttribute("aria-labelledby", "p-question-prompt");
    const selected = previewAnswers[q.id];

    (q.options || []).forEach((optText, optIdx) => {
      const label = document.createElement("label");
      label.className = `mc-option-label ${selected === optIdx ? "checked" : ""}`;
      label.innerHTML = `
        <div class="mc-radio-circle" aria-hidden="true"></div>
        <span>${escapeHtml(optText)}</span>
      `;
      const input = document.createElement("input");
      input.type = "radio";
      input.className = "mc-option-input";
      input.name = `nstu-preview-choice-${q.id}`;
      input.value = String(optIdx);
      input.checked = selected === optIdx;
      input.setAttribute("aria-label", `Option ${optIdx + 1}: ${optText}`);
      input.addEventListener("focus", revealChoiceFocus);
      input.addEventListener("change", () => {
        if (!input.checked) return;
        previewAnswers[q.id] = optIdx;
        Array.from(surface.children).forEach((option, index) => {
          option.classList.toggle("checked", index === optIdx);
          option.querySelector(".mc-option-input").checked = index === optIdx;
        });
      });
      label.prepend(input);
      surface.appendChild(label);
    });
  }

  /* ========================================================================
     Splitter Resizer Logic
     ======================================================================== */

  function initSplitter() {
    const splitter = el("preview-splitter");
    const container = el("preview-split-container");
    const leftPane = el("preview-pane-left");

    let isDragging = false;

    splitter.addEventListener("mousedown", (e) => {
      isDragging = true;
      splitter.classList.add("active");
      document.body.style.cursor = "col-resize";
      e.preventDefault();
    });

    window.addEventListener("mousemove", (e) => {
      if (!isDragging) return;
      const rect = container.getBoundingClientRect();
      const offset = e.clientX - rect.left;
      const percentage = (offset / rect.width) * 100;
      if (percentage >= 25 && percentage <= 75) {
        leftPane.style.flex = "none";
        leftPane.style.width = `${percentage}%`;
      }
    });

    window.addEventListener("mouseup", () => {
      if (isDragging) {
        isDragging = false;
        splitter.classList.remove("active");
        document.body.style.cursor = "";
      }
    });
  }

  /* ========================================================================
     Validation & Packaging (.nstuexam Export)
     ======================================================================== */

  function parseOriginsInput(raw) {
    if (!raw || typeof raw !== "string") return [];
    const lines = raw.split(/[\r\n,]+/);
    const seen = new Set();
    const origins = [];
    for (const item of lines) {
      const trimmed = item.trim();
      if (!trimmed) continue;
      if (!seen.has(trimmed)) {
        seen.add(trimmed);
        origins.push(trimmed);
      }
    }
    return origins;
  }

  function validateOrigin(origin) {
    if (typeof origin !== "string" || origin.length < 1 || origin.length > 253) return false;
    if (!/^[\x21-\x7e]+$/.test(origin)) return false;
    if (origin.includes("@")) return false; // no userinfo
    let host = origin;
    if (/^https?:\/\//i.test(host)) {
      host = host.replace(/^https?:\/\//i, "");
    } else if (host.startsWith("//")) {
      host = host.substring(2);
    } else if (host.includes("://")) {
      return false; // unsupported scheme
    }
    const cut = host.search(/[\/?#]/);
    if (cut !== -1) {
      host = host.substring(0, cut);
    }
    const colon = host.lastIndexOf(":");
    if (colon !== -1) {
      const port = host.substring(colon + 1);
      if (!/^\d{1,5}$/.test(port) || Number(port) < 1 || Number(port) > 65535) return false;
      host = host.substring(0, colon);
    }
    if (!host || host.length > 253) return false;
    const labels = host.split(".");
    for (const label of labels) {
      if (!label || label.length > 63) return false;
      if (!/^[a-zA-Z0-9-]+$/.test(label)) return false;
      if (label.startsWith("-") || label.endsWith("-")) return false;
    }
    return true;
  }

  function validateExam(exam) {
    const errors = [];
    if (!exam.id || !/^[a-zA-Z0-9._-]{1,96}$/.test(exam.id)) {
      errors.push("Package ID must be 1–96 alphanumeric characters (dashes and underscores allowed).");
    }
    if (!exam.title || exam.title.length < 1 || exam.title.length > 240) {
      errors.push("Assessment title must be between 1 and 240 characters.");
    }
    if (!exam.durationSeconds || exam.durationSeconds < 60 || exam.durationSeconds > 86400) {
      errors.push("Duration must be between 1 minute and 24 hours.");
    }
    if (exam.allowedOrigins) {
      if (!Array.isArray(exam.allowedOrigins)) {
        errors.push("Allowed origins must be a list.");
      } else if (exam.allowedOrigins.length > 32) {
        errors.push("Allowed origins list cannot exceed 32 items.");
      } else {
        const seen = new Set();
        exam.allowedOrigins.forEach((orig, idx) => {
          if (seen.has(orig)) {
            errors.push(`Duplicate origin declared: ${orig}`);
          }
          seen.add(orig);
          if (!validateOrigin(orig)) {
            errors.push(`Allowed origin #${idx + 1} (${orig}) is invalid. Must be a bare hostname or http(s) URL (1–253 chars, no userinfo).`);
          }
        });
      }
    }
    if (!Array.isArray(exam.questions) || exam.questions.length === 0) {
      errors.push("Exam must contain at least one question.");
    } else if (exam.questions.length > 500) {
      errors.push("Exam exceeds the 500 questions quota limit.");
    }

    exam.questions.forEach((q, idx) => {
      const qNum = idx + 1;
      if (!q.id || !/^[a-zA-Z0-9._-]{1,96}$/.test(q.id)) {
        errors.push(`Question #${qNum} ID is invalid.`);
      }
      if (!q.prompt || q.prompt.trim().length === 0) {
        errors.push(`Question #${qNum} has an empty prompt.`);
      }
      if (q.isMatching) {
        if (!q.options || q.options.length < 2) {
          errors.push(`Question #${qNum} (Matching) must have at least 2 draggable items in the token bank.`);
        }
        if (!q.matchingTargets || q.matchingTargets.length < 1) {
          errors.push(`Question #${qNum} (Matching) must have at least 1 target slot.`);
        }
      } else if (q.type === "multiple_choice" || q.type === "listening" || q.type === "reading") {
        if (!q.options || q.options.length < 2) {
          errors.push(`Question #${qNum} must have at least 2 answer options.`);
        }
      }
    });

    return errors;
  }

  // Authoring keeps its full model. This boundary admits only values the current
  // schema, stager and native JsonReader can consume without data substitution.
  function validateExportContract(exam) {
    const errors = [];
    const object = value => value !== null && typeof value === "object" && !Array.isArray(value);
    const text = (value, path, min, max, maxBytes = 65536) => {
      if (typeof value !== "string") { errors.push(`${path} must be text.`); return; }
      if (value.length < min || value.length > max) errors.push(`${path} must contain ${min}–${max} characters.`);
      // TextEncoder replaces lone surrogates; native JSON decoding rejects them.
      for (let i = 0; i < value.length; i++) {
        const unit = value.charCodeAt(i);
        if (unit >= 0xd800 && unit <= 0xdbff) {
          const next = value.charCodeAt(++i);
          if (!(next >= 0xdc00 && next <= 0xdfff)) { errors.push(`${path} contains an unpaired Unicode surrogate.`); return; }
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
          errors.push(`${path} contains an unpaired Unicode surrogate.`); return;
        }
      }
      if (new TextEncoder().encode(value).byteLength > maxBytes) errors.push(`${path} exceeds the native ${maxBytes} UTF-8 byte limit.`);
    };
    const integer = (value, path, min, max) => {
      if (!Number.isInteger(value) || value < min || value > max) errors.push(`${path} must be an integer from ${min} to ${max}.`);
    };
    const identifier = (value, path) => {
      if (typeof value !== "string" || !/^[a-zA-Z0-9._-]{1,96}$/.test(value)) errors.push(`${path} must be a valid 1–96 character ID.`);
    };
    if (!object(exam)) return ["Assessment must be an object."];
    identifier(exam.id, "Package ID");
    text(exam.title, "Assessment title", 1, 240);
    integer(exam.durationSeconds, "Duration (seconds)", 60, 86400);
    for (const [key, max] of [["subject", 120], ["candidate", 160]]) {
      if (Object.hasOwn(exam, key)) text(exam[key], key, 0, max);
    }
    if (Object.hasOwn(exam, "allowedOrigins")) {
      if (!Array.isArray(exam.allowedOrigins) || exam.allowedOrigins.length > 32) errors.push("Allowed origins must be a list of at most 32 entries.");
      else {
        const seen = new Set();
        exam.allowedOrigins.forEach((origin, index) => {
          if (!validateOrigin(origin) || seen.has(origin)) errors.push(`Allowed origin #${index + 1} is invalid or duplicated.`);
          seen.add(origin);
        });
      }
    }
    if (Object.hasOwn(exam, "documents") && (!Array.isArray(exam.documents) || exam.documents.length !== 0)) {
      errors.push("Export cannot bundle authored documents. Keep them in the draft until media packaging is supported.");
    }
    if (!Array.isArray(exam.questions) || exam.questions.length < 1 || exam.questions.length > 256) {
      errors.push("The current native reader supports 1–256 questions per Export. Authoring and Save retain up to 500.");
      return errors;
    }
    const ids = new Set();
    exam.questions.forEach((q, index) => {
      const path = `Question #${index + 1}`;
      if (!object(q)) { errors.push(`${path} must be an object.`); return; }
      identifier(q.id, `${path} ID`);
      if (ids.has(q.id)) errors.push(`${path} has a duplicate ID.`);
      ids.add(q.id);
      if (!["multiple_choice", "short_answer", "essay", "listening", "reading"].includes(q.type)) errors.push(`${path} has an unsupported native type.`);
      if (q.isMatching === true) errors.push(`${path}: Matching is available in draft Preview but unsupported by the native runtime.`);
      for (const key of ["audio", "pdf"]) {
        if (Object.hasOwn(q, key)) errors.push(`${path}: Export cannot bundle declared ${key} media, including blank paths.`);
      }
      text(q.prompt, `${path} prompt`, 1, 8000, 8000);
      for (const [key, max] of [["instruction", 4000], ["passage", 30000], ["answerPlaceholder", 240]]) {
        if (Object.hasOwn(q, key)) text(q[key], `${path} ${key}`, 0, max);
      }
      if (Object.hasOwn(q, "points")) integer(q.points, `${path} points`, 0, 1000);
      if (Object.hasOwn(q, "wordLimit")) integer(q.wordLimit, `${path} word limit`, 1, 5000);
      const needsOptions = ["multiple_choice", "listening", "reading"].includes(q.type);
      if (Object.hasOwn(q, "options") || needsOptions) {
        if (!Array.isArray(q.options) || q.options.length < 2 || q.options.length > 12) errors.push(`${path} options must contain 2–12 entries.`);
        else q.options.forEach((option, n) => text(option, `${path} option #${n + 1}`, 1, 2000, 2000));
      }
    });
    return errors;
  }

  function compileManifest(exam) {
    // Generate clean manifest matching manifest.schema.json
    const manifest = {
      id: exam.id,
      title: exam.title,
      subject: Object.hasOwn(exam, "subject") ? exam.subject : "General",
      durationSeconds: exam.durationSeconds,
      documents: Object.hasOwn(exam, "documents") ? exam.documents : [],
      questions: exam.questions.map(q => {
        const item = {
          id: q.id,
          type: q.type,
          prompt: q.prompt,
          instruction: Object.hasOwn(q, "instruction") ? q.instruction : "",
          points: Object.hasOwn(q, "points") ? q.points : 1
        };
        for (const key of ["passage", "options", "audio", "pdf", "wordLimit", "answerPlaceholder"]) {
          if (Object.hasOwn(q, key)) item[key] = q[key];
        }
        return item;
      })
    };
    for (const key of ["candidate", "allowedOrigins"]) {
      if (Object.hasOwn(exam, key)) manifest[key] = exam[key];
    }
    return manifest;
  }

  async function readRunnerAsset(relativePath, maximumBytes) {
    const response = await fetch(relativePath, { redirect: "error" });
    if (!response.ok || new URL(response.url).origin !== location.origin || !response.body) {
      throw new Error(`Runner asset unavailable: ${relativePath}`);
    }
    const reader = response.body.getReader();
    const chunks = [];
    let length = 0;
    let readFailed = false;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        length += value.byteLength;
        if (length > maximumBytes) throw new Error(`Runner asset exceeds limit: ${relativePath}`);
        chunks.push(value);
      }
    } catch (error) {
      readFailed = true;
      throw error;
    } finally {
      let cleanupFailed = false, cleanupError;
      try { await reader.cancel(); }
      catch (error) { cleanupFailed = true; cleanupError = error; }
      try { reader.releaseLock(); }
      catch (error) {
        if (!cleanupFailed) { cleanupFailed = true; cleanupError = error; }
      }
      if (cleanupFailed && !readFailed) throw cleanupError;
    }
    const bytes = new Uint8Array(length);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    return bytes;
  }

  async function verifiedBrandPng(bytes) {
    if (bytes.byteLength !== 171569) throw new Error("Bundled NSTU image has an invalid length.");
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
    const hex = Array.from(digest, value => value.toString(16).padStart(2, "0")).join("");
    if (hex !== "0b3cf7023212288d978bd898d0fa9ec1dcb7e82c92b678fa2bd1d42e18272486") {
      throw new Error("Bundled NSTU image does not match the supplied artwork.");
    }
    return bytes;
  }

  function losslessRunnerText(bytes) {
    const text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
    const encoded = new TextEncoder().encode(text);
    if (!text || encoded.length !== bytes.length || encoded.some((value, index) => value !== bytes[index])) {
      throw new Error("Runner text cannot be preserved as UTF8.");
    }
    return text;
  }

  let exportDialogOpener = null;

  function usableExportFocus(node) {
    return !!node && node.ownerDocument === document && node.isConnected &&
      node !== document.body && node !== document.documentElement &&
      !node.closest("[hidden], [inert]") && !node.matches(":disabled") &&
      node.tabIndex >= 0 && node.getClientRects().length > 0 &&
      !["hidden", "collapse"].includes(getComputedStyle(node).visibility);
  }

  function exportDialogButtons() {
    const modal = el("export-modal");
    return Array.from(modal.querySelectorAll("button")).filter(node =>
      modal.contains(node) && usableExportFocus(node));
  }

  function focusExportDialog() {
    const cancel = el("btn-cancel-export");
    if (usableExportFocus(cancel) && el("export-modal").contains(cancel)) {
      cancel.focus();
      if (document.activeElement === cancel) return;
    }
    const first = exportDialogButtons()[0];
    if (first) first.focus();
    if (!first || document.activeElement !== first) el("export-modal").focus();
  }

  function openExportDialog(event) {
    const modal = el("export-modal");
    if (!modal.classList.contains("open")) {
      exportDialogOpener = [event?.currentTarget, document.activeElement, el("btn-export-pkg")]
        .find(node => usableExportFocus(node) && !modal.contains(node)) || null;
    }
    modal.inert = false;
    modal.classList.add("open");
    focusExportDialog();
  }

  function closeExportDialog() {
    const modal = el("export-modal");
    if (!modal.classList.contains("open")) return;
    modal.classList.remove("open");
    modal.inert = true;
    const opener = exportDialogOpener;
    exportDialogOpener = null;
    const canReturn = usableExportFocus(opener);
    if (canReturn) opener.focus();
    if ((!canReturn || document.activeElement !== opener) && usableExportFocus(el("btn-export-pkg"))) {
      el("btn-export-pkg").focus();
    }
  }

  function handleExportDialogKey(event) {
    if (!el("export-modal").classList.contains("open") || event.ctrlKey || event.altKey || event.metaKey) return;
    if (event.key === "Escape" && !event.shiftKey) {
      event.preventDefault();
      closeExportDialog();
    } else if (event.key === "Tab") {
      event.preventDefault();
      const buttons = exportDialogButtons();
      const index = buttons.indexOf(document.activeElement);
      const next = index < 0 ? (event.shiftKey ? buttons.length - 1 : 0)
        : (index + (event.shiftKey ? -1 : 1) + buttons.length) % buttons.length;
      (buttons[next] || el("export-modal")).focus();
    }
  }

  let authoringResizeFrame = null;
  function queueAuthoringFocusReveal() {
    if (authoringResizeFrame !== null) window.cancelAnimationFrame(authoringResizeFrame);
    authoringResizeFrame = window.requestAnimationFrame(() => {
      authoringResizeFrame = null;
      const editor = el("editor-view");
      const active = document.activeElement;
      if (getComputedStyle(editor).display === "none" || el("export-modal").classList.contains("open") ||
          !editor.contains(active) || !usableExportFocus(active)) return;
      active.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "instant" });
    });
  }

  let matchingResizeFrame = null;
  function queueMatchingFocusReveal() {
    if (matchingResizeFrame !== null) window.cancelAnimationFrame(matchingResizeFrame);
    matchingResizeFrame = null;
    if (getComputedStyle(el("preview-view")).display === "none") return;
    matchingResizeFrame = window.requestAnimationFrame(() => {
      matchingResizeFrame = null;
      const active = document.activeElement;
      if (getComputedStyle(el("preview-view")).display === "none" ||
          !currentExam.questions[previewIndex]?.isMatching || el("export-modal").classList.contains("open") ||
          !el("p-matching-surface").contains(active) || !usableExportFocus(active)) return;
      active.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "instant" });
    });
  }

  let choiceResizeFrame = null;
  function queueChoiceFocusReveal() {
    if (choiceResizeFrame !== null) window.cancelAnimationFrame(choiceResizeFrame);
    choiceResizeFrame = null;
    if (getComputedStyle(el("preview-view")).display === "none") return;
    choiceResizeFrame = window.requestAnimationFrame(() => {
      choiceResizeFrame = null;
      revealChoiceFocus();
    });
  }

  function revealChoiceFocus() {
    const active = document.activeElement;
    const q = currentExam.questions[previewIndex];
    if (!q || q.isMatching || !["multiple_choice", "reading", "listening"].includes(q.type) ||
        getComputedStyle(el("preview-view")).display === "none" || el("export-modal").classList.contains("open") ||
        active?.tagName !== "INPUT" || active.type !== "radio" || !active.classList.contains("mc-option-input") ||
        !el("p-mc-surface").contains(active) || !usableExportFocus(active)) return;
    active.parentNode.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "instant" });
  }

  let previewTextResizeFrame = null;
  function queuePreviewTextFocusReveal() {
    if (previewTextResizeFrame !== null) window.cancelAnimationFrame(previewTextResizeFrame);
    previewTextResizeFrame = null;
    if (getComputedStyle(el("preview-view")).display === "none") return;
    previewTextResizeFrame = window.requestAnimationFrame(() => {
      previewTextResizeFrame = null;
      revealPreviewTextFocus();
    });
  }

  function revealPreviewTextFocus() {
    const active = document.activeElement;
    const q = currentExam.questions[previewIndex];
    const field = q?.type === "essay" ? el("p-essay-input")
      : q?.type === "short_answer" ? el("p-sa-input") : null;
    if (!q || q.isMatching || active !== field || getComputedStyle(el("preview-view")).display === "none" ||
        el("export-modal").classList.contains("open") || !el("preview-view").contains(active) || !usableExportFocus(active)) return;
    active.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "instant" });
  }

  let numericDraftModel = null;
  let numericDraftOwners = new WeakMap();
  const numericFields = {
    "exam-duration": { label: "Duration", key: "durationSeconds", min: 1, max: 1440, scale: 60, fallback: 3600 },
    "q-points": { label: "Points", key: "points", min: 0, max: 1000, scale: 1, fallback: 1 },
    "q-essay-limit": { label: "Word limit", key: "wordLimit", min: 1, max: 5000, scale: 1, optional: true }
  };

  function numericDraft(id, question) {
    if (numericDraftModel !== currentExam) {
      numericDraftModel = currentExam;
      numericDraftOwners = new WeakMap();
    }
    const spec = numericFields[id];
    const owner = id === "exam-duration" ? currentExam : question;
    if (!owner || (id === "q-essay-limit" && owner.type !== "essay")) return null;
    if (!numericDraftOwners.has(owner)) numericDraftOwners.set(owner, {});
    const drafts = numericDraftOwners.get(owner);
    if (!drafts[id]) {
      const value = owner[spec.key] ?? spec.fallback;
      drafts[id] = { raw: value === undefined ? "" : String(value / spec.scale), rejected: false };
      if (id === "exam-duration") {
        drafts[id].seededRaw = drafts[id].raw;
        drafts[id].seededSeconds = value;
      }
    }
    return drafts[id];
  }

  function numericError(id, draft) {
    const spec = numericFields[id];
    if (spec.optional && draft.raw === "" && !draft.rejected) return "";
    if (id === "exam-duration") {
      const seconds = draft.raw === draft.seededRaw ? draft.seededSeconds : Number(draft.raw) * 60;
      return !draft.rejected && /^\d+(?:\.\d+)?$/.test(draft.raw.trim()) && Number.isInteger(seconds) &&
        seconds >= 60 && seconds <= 86400 ? "" : "Duration must convert to whole seconds from 60 to 86400 (1–1440 minutes).";
    }
    const value = Number(draft.raw);
    return !draft.rejected && /^\d+$/.test(draft.raw.trim()) && Number.isInteger(value) &&
      value >= spec.min && value <= spec.max ? ""
      : `${spec.label} must be a whole number from ${spec.min} to ${spec.max}.`;
  }

  function showNumericError(id, draft) {
    const message = numericError(id, draft);
    el(id).setAttribute("aria-invalid", String(!!message));
    const error = el(id + "-error");
    error.textContent = message;
    error.hidden = !message;
  }

  function renderNumericField(id, question) {
    const draft = numericDraft(id, question);
    if (!draft) return;
    if (el(id).value !== draft.raw) el(id).value = draft.raw;
    showNumericError(id, draft);
  }

  function captureNumericField(id, question, inputEvent = false) {
    const draft = numericDraft(id, question);
    if (!draft) return;
    const node = el(id);
    if (inputEvent || node.value !== draft.raw) {
      draft.raw = node.value;
      draft.rejected = node.validity?.valid === false || !!node.validity?.badInput;
    }
    if (!numericError(id, draft)) {
      const spec = numericFields[id];
      const owner = id === "exam-duration" ? currentExam : question;
      if (spec.optional && draft.raw === "") delete owner[spec.key];
      else owner[spec.key] = id === "exam-duration" && draft.raw === draft.seededRaw
        ? draft.seededSeconds : Number(draft.raw) * spec.scale;
    }
    showNumericError(id, draft);
  }

  function numericExportErrors() {
    const errors = [];
    const duration = numericError("exam-duration", numericDraft("exam-duration"));
    if (duration) errors.push(duration);
    currentExam.questions.forEach((q, index) => {
      for (const id of q.type === "essay" ? ["q-points", "q-essay-limit"] : ["q-points"]) {
        const error = numericError(id, numericDraft(id, q));
        if (error) errors.push(`Question #${index + 1}: ${error}`);
      }
    });
    return errors;
  }

  function syncExamMetadata() {
    currentExam.id = el("exam-id").value.trim();
    currentExam.title = el("exam-title").value.trim();
    currentExam.subject = el("exam-subject").value.trim();
    captureNumericField("exam-duration");
    const origins = parseOriginsInput(el("exam-allowed-origins").value);
    if (origins.length > 0) {
      currentExam.allowedOrigins = origins;
    } else {
      delete currentExam.allowedOrigins;
    }
  }

  let exportGeneration = 0;
  async function exportPackage(event) {
    const generation = ++exportGeneration;
    syncExamMetadata();

    const statusBox = el("export-validation-status");
    const hashesBox = el("export-hashes");
    const btnDownload = el("btn-download-nstuexam");

    openExportDialog(event);
    statusBox.style.color = "var(--text-main)";
    statusBox.innerHTML = "⏳ Validating exam schema and compiling package...";
    hashesBox.style.display = "none";
    btnDownload.disabled = true;
    btnDownload.onclick = null;
    for (const id of ["lbl-archive-hash", "lbl-content-hash", "lbl-stage-command"]) el(id).textContent = "-";

    const errors = validateExportContract(currentExam);
    if (errors.length === 0) errors.push(...validateExam(currentExam), ...numericExportErrors());
    if (errors.length > 0) {
      statusBox.style.color = "var(--danger)";
      statusBox.innerHTML = `<strong>Validation Failed (${errors.length} errors):</strong><ul style="margin: 8px 0 0 18px;">${errors.map(e => `<li>${escapeHtml(e)}</li>`).join("")}</ul>`;
      return;
    }

    try {
      const manifestObj = compileManifest(currentExam);
      const manifestJson = JSON.stringify(manifestObj, null, 2);
      if (new TextEncoder().encode(manifestJson).byteLength > 131072) {
        throw new Error("Manifest exceeds the current native reader's 131072 UTF-8 byte limit. The complete draft can still be saved.");
      }
      const packageId = manifestObj.id;

      const zip = new NstuZip.Writer();
      zip.add("manifest.json", manifestJson);

      // Add web runner files from template.js or relative fetch
      const runner = window.NSTU_RUNNER_TEMPLATE;
      let brand;
      if (runner !== undefined) {
        if (!runner || ["indexHtml", "stylesCss", "appJs"].some(key => typeof runner[key] !== "string" || !runner[key])) {
          throw new Error("Bundled runner text is incomplete.");
        }
        const base64 = runner.brandPngBase64;
        if (typeof base64 !== "string" || base64.length !== 228760 || !/^[A-Za-z0-9+/]*={0,2}$/.test(base64)) {
          throw new Error("Bundled NSTU image encoding is invalid.");
        }
        const binary = atob(base64);
        if (btoa(binary) !== base64) throw new Error("Bundled NSTU image encoding is not canonical.");
        brand = await verifiedBrandPng(Uint8Array.from(binary, value => value.charCodeAt(0)));
        zip.add("exam/web/index.html", runner.indexHtml);
        zip.add("exam/web/styles.css", runner.stylesCss);
        zip.add("exam/web/app.js", runner.appJs);
      } else {
        // Fallback fetch
        const [hRes, cRes, jRes, png] = await Promise.all([
          readRunnerAsset("../web/index.html", 1024 * 1024),
          readRunnerAsset("../web/styles.css", 1024 * 1024),
          readRunnerAsset("../web/app.js", 1024 * 1024),
          readRunnerAsset("../web/assets/nstu-brand.png", 171569)
        ]);
        brand = await verifiedBrandPng(png);
        zip.add("exam/web/index.html", losslessRunnerText(hRes));
        zip.add("exam/web/styles.css", losslessRunnerText(cRes));
        zip.add("exam/web/app.js", losslessRunnerText(jRes));
      }
      zip.add("exam/web/assets/nstu-brand.png", brand);

      const result = await zip.build();
      if (generation !== exportGeneration) return;

      statusBox.style.color = "var(--success)";
      statusBox.innerHTML = `✓ <strong>Exam Package Validated & Compiled Successfully!</strong><br><span style="font-size: 12px; color: var(--text-muted);">${result.fileCount} files · ${Math.round(result.totalBytes / 1024)} KiB</span>`;

      el("lbl-archive-hash").textContent = result.archiveSha256;
      el("lbl-content-hash").textContent = result.contentSha256;

      const stageCmd = `& stage-exam-package.ps1 -ArchivePath "$env:TEMP\\${packageId}.nstuexam" -PublishRoot "$env:ProgramData\\NSTU\\exams\\packages" -ExpectedArchiveSha256 "${result.archiveSha256}" -ExpectedContentSha256 "${result.contentSha256}" -AllowUnsigned`;
      el("lbl-stage-command").textContent = stageCmd;

      hashesBox.style.display = "flex";
      btnDownload.disabled = false;

      btnDownload.onclick = () => {
        const url = URL.createObjectURL(result.blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = `${packageId}.nstuexam`;
        a.click();
        URL.revokeObjectURL(url);
        showToast("Package downloaded successfully", "success");
      };

    } catch (err) {
      if (generation !== exportGeneration) return;
      statusBox.style.color = "var(--danger)";
      statusBox.innerHTML = `<strong>Build Error:</strong> ${escapeHtml(err.message)}`;
    }
  }

  /* ========================================================================
     Import & Save Draft
     ======================================================================== */

  async function handleImportFile(file) {
    // Admission is read-only: do not touch the current draft or Preview until
    // every renderer-consumed field of the candidate is safe to use.
    const admit = (candidate) => {
      const object = (value, path) => {
        if (value === null || typeof value !== "object" || Array.isArray(value)) {
          throw new Error(`${path} must be an object.`);
        }
      };
      const text = (value, path, maximum) => {
        if (typeof value !== "string") throw new Error(`${path} must be text.`);
        const iterator = value[Symbol.iterator]();
        let count = 0;
        while (!iterator.next().done) {
          if (++count > maximum) throw new Error(`${path} exceeds its text limit.`);
        }
      };
      const optionalText = (value, fields, prefix) => {
        for (const [key, maximum] of Object.entries(fields)) {
          if (Object.hasOwn(value, key)) text(value[key], `${prefix}.${key}`, maximum);
        }
      };
      const numeric = (value, key, prefix) => {
        if (Object.hasOwn(value, key) &&
            (typeof value[key] !== "number" || !Number.isFinite(value[key]))) {
          throw new Error(`${prefix}.${key} must be a finite number.`);
        }
      };
      const list = (value, path, maximum, textMaximum) => {
        if (!Array.isArray(value) || value.length > maximum) {
          throw new Error(`${path} must be a bounded text list.`);
        }
        value.forEach((item, index) => text(item, `${path}[${index}]`, textMaximum));
      };
      const validId = value => typeof value === "string" && /^[a-zA-Z0-9._-]{1,96}$/.test(value);
      object(candidate, "Manifest");
      if (!validId(candidate.id)) throw new Error("Manifest.id is invalid.");
      optionalText(candidate, { title: 240, subject: 120 }, "Manifest");
      numeric(candidate, "durationSeconds", "Manifest");
      if (Object.hasOwn(candidate, "allowedOrigins")) list(candidate.allowedOrigins, "Manifest.allowedOrigins", 32, 253);
      if (!Array.isArray(candidate.questions) || candidate.questions.length === 0 || candidate.questions.length > 500) {
        throw new Error("Manifest.questions must contain 1–500 question records.");
      }
      const ids = new Set();
      const types = new Set(["multiple_choice", "short_answer", "essay", "listening", "reading"]);
      candidate.questions.forEach((q, index) => {
        const path = `questions[${index}]`;
        object(q, path);
        if (!validId(q.id) || ids.has(q.id) || q.id in {}) {
          throw new Error(`${path}.id is invalid or conflicts with another answer identity.`);
        }
        ids.add(q.id);
        if (!types.has(q.type)) throw new Error(`${path}.type is not supported.`);
        if (Object.hasOwn(q, "isMatching") && typeof q.isMatching !== "boolean") {
          throw new Error(`${path}.isMatching must be a boolean.`);
        }
        optionalText(q, { prompt: 8000, instruction: 4000, passage: 30000, answerPlaceholder: 240, audio: 8000 }, path);
        numeric(q, "points", path);
        numeric(q, "wordLimit", path);
        // These authoring row budgets deliberately do not apply the canonical
        // runtime's 12-option constraint to editable matching/draft data.
        if (q.isMatching || Object.hasOwn(q, "options")) list(q.options, `${path}.options`, 500, 2000);
        if (Object.hasOwn(q, "matchingTargets")) list(q.matchingTargets, `${path}.matchingTargets`, 500, 2000);
      });
    };
    try {
      let candidate;
      let success;
      if (file.name.endsWith(".json")) {
        const text = await file.text();
        candidate = JSON.parse(text);
        success = "Manifest imported successfully";
      } else if (file.name.endsWith(".nstuexam") || file.name.endsWith(".zip")) {
        const buffer = await file.arrayBuffer();
        const reader = new NstuZip.Reader(buffer);
        const entries = await reader.readEntries();
        const manifestEntry = entries.find(e => e.path === "manifest.json");
        if (!manifestEntry) throw new Error("Package does not contain manifest.json.");
        const text = new TextDecoder().decode(manifestEntry.bytes);
        candidate = JSON.parse(text);
        success = `Imported .nstuexam package (${entries.length} files)`;
      } else {
        throw new Error("Please select a .nstuexam or .json file.");
      }
      admit(candidate);
      currentExam = candidate;
      activeQuestionIndex = 0;
      resetPreviewState();
      renderEditor();
      showToast(success, "success");
    } catch (err) {
      showToast("Import failed: " + err.message, "error");
    }
  }

  function saveDraft() {
    syncExamMetadata();

    const jsonStr = JSON.stringify(currentExam, null, 2);
    const blob = new Blob([jsonStr], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `${currentExam.id}-draft.json`;
    a.click();
    URL.revokeObjectURL(url);
    showToast("Authoring draft downloaded", "success");
  }

  function escapeHtml(str) {
    if (typeof str !== "string") return "";
    return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  /* ========================================================================
     Initialization & Event Binding
     ======================================================================== */

  function init() {
    const themeButton = el("btn-studio-theme");
    const applyTheme = (theme) => {
      const dark = theme === "dark";
      document.documentElement.dataset.theme = dark ? "dark" : "light";
      themeButton.textContent = dark ? "Light appearance" : "Dark appearance";
      themeButton.setAttribute("aria-label", `Switch to ${dark ? "light" : "dark"} appearance`);
    };
    let storedTheme = "light";
    try { storedTheme = localStorage.getItem("nstu-studio-theme") || "light"; } catch { /* Appearance stays local when storage is unavailable. */ }
    applyTheme(storedTheme);
    themeButton.addEventListener("click", () => {
      const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
      applyTheme(theme);
      try { localStorage.setItem("nstu-studio-theme", theme); } catch { /* Preserve authoring when preference storage fails. */ }
    });
    // Mode toggles
    el("btn-mode-editor").addEventListener("click", () => setMode("editor"));
    el("btn-mode-preview").addEventListener("click", () => setMode("preview"));
    el("btn-preview-exit").addEventListener("click", () => setMode("editor"));

    // Header actions
    el("btn-load-sample").addEventListener("click", () => {
      currentExam = JSON.parse(JSON.stringify(DEFAULT_EXAM));
      activeQuestionIndex = 0;
      resetPreviewState();
      renderEditor();
      showToast("Loaded IELTS Academic Sample Assessment", "info");
    });

    el("btn-open-file").addEventListener("click", () => el("file-import-input").click());
    el("file-import-input").addEventListener("change", (e) => {
      if (e.target.files && e.target.files[0]) {
        handleImportFile(e.target.files[0]);
      }
    });

    el("btn-save-draft").addEventListener("click", saveDraft);
    el("btn-export-pkg").addEventListener("click", exportPackage);

    // Modal controls
    el("btn-close-export").addEventListener("click", closeExportDialog);
    el("btn-cancel-export").addEventListener("click", closeExportDialog);
    document.addEventListener("keydown", handleExportDialogKey);
    window.addEventListener("resize", queueAuthoringFocusReveal);
    window.addEventListener("resize", queueMatchingFocusReveal);
    window.addEventListener("resize", queueChoiceFocusReveal);
    window.addEventListener("resize", queuePreviewTextFocusReveal);
    el("p-sa-input").addEventListener("focus", revealPreviewTextFocus);
    el("p-essay-input").addEventListener("focus", revealPreviewTextFocus);

    // Add Question
    el("btn-add-question").addEventListener("click", () => {
      const type = el("select-add-type").value;
      addQuestion(type);
    });

    // Question form bindings
    el("exam-duration").addEventListener("input", () => captureNumericField("exam-duration", undefined, true));
    el("q-points").addEventListener("input", () => captureNumericField("q-points", currentExam.questions[activeQuestionIndex], true));
    el("q-points").addEventListener("change", () => captureNumericField("q-points", currentExam.questions[activeQuestionIndex]));

    el("q-prompt").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) {
        q.prompt = e.target.value;
        const navItem = el("question-nav-list").children[activeQuestionIndex];
        if (navItem) {
          navItem.querySelector(".nav-item-title").textContent = e.target.value || "Untitled Question";
          navItem.querySelector(".question-select")?.setAttribute("aria-label", questionButtonName(q, activeQuestionIndex));
        }
      }
    });

    el("q-instruction").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) q.instruction = e.target.value;
    });

    el("q-sa-placeholder").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q && q.type === "short_answer") q.answerPlaceholder = e.target.value;
    });

    el("q-essay-limit").addEventListener("input", (e) => {
      captureNumericField("q-essay-limit", currentExam.questions[activeQuestionIndex], true);
    });

    el("q-passage").addEventListener("input", (e) => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) q.passage = e.target.value;
    });

    el("btn-add-token").addEventListener("click", () => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q && q.isMatching) {
        const nextNum = (q.options ? q.options.length : 0) + 1;
        q.options.push(`Item ${nextNum} description`);
        renderTokenEditor(q);
      }
    });

    el("btn-add-target").addEventListener("click", () => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q && q.isMatching) {
        const nextLetter = String.fromCharCode(65 + (q.matchingTargets ? q.matchingTargets.length : 0));
        q.matchingTargets.push(`Paragraph ${nextLetter}`);
        renderTokenEditor(q);
      }
    });

    el("btn-add-mc-option").addEventListener("click", () => {
      const q = currentExam.questions[activeQuestionIndex];
      if (q) {
        const nextLetter = String.fromCharCode(65 + (q.options ? q.options.length : 0));
        q.options.push(`Option ${nextLetter}`);
        renderMcOptionEditor(q);
      }
    });

    // Preview Navigation
    el("btn-p-prev").addEventListener("click", () => {
      if (previewIndex > 0) {
        previewIndex--;
        renderPreview();
      }
    });

    el("btn-p-next").addEventListener("click", () => {
      if (previewIndex < currentExam.questions.length - 1) {
        previewIndex++;
        renderPreview();
      }
    });

    initSplitter();
    renderEditor();
  }

  // Start application on DOM ready
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }

})();
