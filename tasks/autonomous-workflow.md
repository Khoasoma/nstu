# NSTU resumable task workflow

The user authorized recurring continuation, scoped subagents, planning before
every task, and conditional promotion of completed verified work to `main` on
October 27, 2026. This authorization persists across reconnects. It does not
replace missing acceptance, invent runtime evidence, or make incomplete code
ready for `main`.

The app heartbeat checks this chat every minute. Its local recovery index is
`_agents/autonomous-resume/STATE.json`. Git and `tasks/todo.md` remain authoritative;
the retired orchestrator and its failed state remain historical evidence.

## Before each task

1. Read `AGENTS.md`, the current plan/checklist, this workflow and the applicable
   roadmap sections. Reconcile branch, HEAD, index, dirty paths and current CI.
2. Inspect actual existing code and direct callers. Write a task plan under
   `tasks/task-plans/` with a stable ID, objective, at most three acceptance
   groups, dependencies, file allowlist, test commands, compatibility and risks.
   Split work larger than five product files before assigning a writer.
3. Bind the plan to the current HEAD, plan/checklist hashes and exact source
   hashes in the local recovery state. Preserve the baseline and immutable
   operation receipts under `_agents/autonomous-resume/operations/`.
4. Delegate research and independent review concurrently. Assign one product
   writer for shared files and one build owner per build directory. Root owns
   shared plans, recovery state, staging and Git publication. A coder may be a
   scoped subagent; its reviewer must not have authored its changes.

## Phase and operation receipts

Use `planning -> reproducing -> implementing -> focused_tests -> full_gates ->
reviewing -> feature_publish -> ci_wait -> promotion_check -> complete`.
Rejections return to a scoped repair with a new operation receipt.

Before dispatching an agent or command, atomically persist its operation ID,
intent, pre-HEAD/index/source bindings, owner, expected outcome and log paths.
After dispatch, persist the returned agent/session/process ID. On completion,
record the actual exit code, source bindings, review verdict or exact CI SHA.
Write state through a flushed temporary file and same-directory replacement;
keep the previous valid snapshot. Do not overwrite an immutable receipt.

Claim product ownership exclusively in
`_agents/autonomous-resume/WRITER.json`; record task/run/thread/agent identifiers.
Every writer verifies that claim and its plan before editing. A stale timestamp
alone never permits takeover. Release or transfer a claim only after confirming
the previous owner stopped or completed and reconciling its source changes.

## What each resume wake does

- Inspect live agents, saved command sessions/processes, receipts and Git state.
  An empty saved job list does not prove no job is running.
- Continue the existing task or wait for its active worker. Recognize completed
  operations from their results; do not repeat a recorded command blindly.
- Count a test or review only when its source/plan bindings still apply. Treat
  missing exit status, truncated evidence or changed hashes as unverified.
- If state is malformed or ownership is unknown, reconcile affected paths
  read-only and continue independent eligible planning. Preserve all files.
- Retry external failures with bounded backoff; do not launch repeated reviewers
  or duplicate commits while nothing has changed. Stay quiet without actionable
  progress; report completion, meaningful changes, failure or required action.

| Interrupted operation | Reconciliation before retry |
|---|---|
| Editing | Inspect owner and diff against saved source hashes; resume the existing writer or preserve its partial work. |
| Build/test/review | Attach to its saved live session/agent; accept only recorded completion against matching source. |
| Commit | Inspect HEAD, parent, tree and index against intent; record an already-created intended commit instead of duplicating it. |
| Push | Query the intended remote ref and ancestry; record success if the intended commit arrived. Reconcile unexpected movement; never force-push. |
| Main integration | Inspect recorded old main/candidate refs and merge state; recognize completed promotion and preserve conflicts or unexpected movement. |

## Main promotion gate

The gate applies to the declared integration scope, not every future enhancement.
Its production behavior must be complete and functioning, required independent
acceptance/runtime evidence must exist, known blocking defects must be resolved,
and focused, fast, strict and browser tests must match the candidate sources.
All three required MSVC Release CI jobs must pass for the exact feature revision.

Before main integration, record remote main, candidate SHA/tree, intended strategy
and included paths. Preserve unrelated edits. If integration changes the tested
tree, verify that resulting revision before publication. Push authorized main
normally, then verify CI for its exact SHA; repair failures rather than claiming
success. Do not merge or push unfinished production behavior merely because CI
is green. Current capture, acceptance and durable-channel gaps block promotion.

## Publication and privacy

Stage only explicitly reviewed paths/hunks. Never publish
`PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, `HANDOFF.md` or `_agents/`.
The original product roadmap is a private local source; extracted requirements
may appear in derived task plans. Check the candidate tree as well as the index.
Use `khoasoma <songtu438@gmail.com>` without attribution. Preserve the unrelated
README/About edits and the historical banner provenance incident.

Real VM/UWF mutations, production certificate operations and school pilots keep
their existing human/runtime gates. No automatic SAS approval, raw shell in
Teacher mode, invasive capture or unsupported exam-safety claim is authorized.
