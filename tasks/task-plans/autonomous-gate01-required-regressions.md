# GATE01: mandatory current-source regression entrypoints

Status: REPIN_READY. Root non-author accepted the prior plan ECFC6F97763DEE21DBA0B75AC854DDF6A5054F49C6F0230FB8BC26D7BC81C9C2; no product edits before final repin admission.

Owner: delivery_gates, sole writer for the four product paths below. Root owns independent reviews, full/raw CTest serialization, publication and integration. Managed worktree: C:/Users/xkhoa/.codex/worktrees/autonomous-gate01/nstu-ui-reviewed-20261107. Original planning HEAD: be5a52b964bb3013d346577f4d6dfcc4dd998b20. Clean repinned detached implementation HEAD: dddf739ca2be87090ba1f18a2e320982f2528f86, tree b235b09ee8fa2526b2ee5a56a9e366ea0ddac314. This is root's accepted local-source Studio E01 + origin-helper integration; exact-head Windows CI is pending and not accepted by this repin. The autonomous human mandate authorizes isolated engineering; no push/main/release/key-infrastructure action is assigned here.

## Exact scope and source bindings

Product allowlist (four files only):
- scripts/verify-gate.sh
- .github/workflows/windows.yml
- scripts/tests/required-regressions.mjs (new shared verification helper)
- scripts/tests/verify-required-regressions-tests.mjs (new entrypoint regression test)

Plan/evidence: this file, unpublished _agents/autonomous-resume ownership and immutable receipts, own build/gate01-evidence logs. No app, Studio, schema, service, agent, packaging, CMake, NativeUIA or live/checkpoint/build edits.

SHA-256 before product changes:
| Path | SHA-256 |
| --- | --- |
| scripts/verify-gate.sh | 1E0683D109608DAC248E5B96B6EDE5D165CBDD96C7E8018ACFA78E29953609EA |
| .github/workflows/windows.yml | 0634F0D3DD4EC7DDAA8D6AE11C3B736571971BF136ABDE3A5DD71FD7368C9CBC |
| scripts/tests/required-regressions.mjs | ABSENT |
| scripts/tests/verify-required-regressions-tests.mjs | ABSENT |
| AGENTS.md | 833C40AC671D6C3A0DD6B763AA537285E6EAB4EC22FCD5D119E10E85C3646BE5 |
| tasks/autonomous-workflow.md | 0A00D42FDEB6EA668A25B6F9F48098ED810741DC87341D51FE76105F246C3434 |
| tasks/todo.md | E98BE6C268650B749EC4B3377D10C973AEBBF39CEEAB12DCC21FDA7496DB17CC |
| tasks/plan.md | 6387998F211484792FFB45E0F28AEF29CDFC31D134030FA7F6C8A60B14439FA1 |
| exam/studio/studio.js | FB797C6E172F7C2F4507A27977F59FB710EDA1C3877EBFA5ABB1EEE4572D22F6 |
| exam/studio/index.html | 8C3510BE8F60194829B95839D4B6716ECE537009E376B8383D23E90C58DAE53A |
| tests/exam_runner_template_tests.mjs | 9457DD15CC87651249C70D6DD37A296B024F2D9E86008076F72587D5D3B89889 |
| tests/ui_desktop_token_tests.mjs | 304E200EAEA6ABC18F095C5D1C04D099EA3C40156E616249B17113EE3365A68E |
| scripts/generate-ui-desktop-tokens.mjs | DAC01DC36C369D8D61DADA4FA3C00590CB3ECC45FD5B8215968392062E885BEA |
| packaging/test-exam-origin-contract.ps1 | 7B29937528C910F6E23B7511DF0BAEC41A764728C9A7ACFA7DCCF44671E37975 |
| packaging/test-exam-package-staging.ps1 | 770E5BD09BFAD25CC3B7E004E13DEC14D61BC653711998ADAE23401DDBD5D9EA |
| tests/exam_origin_package_projection.mjs | B52DF525E76D1E54F5D8044F1AA75E500AA7971FEEF05067A726C3473C9A4406 |

WORKSPACE_SCOPE.md is absent in this managed tree. Investigation/write scope follows root's exact mandate; no broad filesystem search or other-worktree writes. AGENTS, tasks/todo, tasks/plan and autonomous-workflow read; historical task completion/CI claims remain historical.

## Problem and implementation contract

Local gate currently requires only Node syntax/recovery; runner/controller tests, desktop token tests/generation parity and packaging conformance are unregistered. Windows asset validation runs native Node commands before later successful commands, without explicit child-exit checks. Strict configure writes to the shared fixed /tmp/nstu-cfg.log. The removed-assertion rule is a NOTE over unstaged tests/ only.

Create one actual shared Node verification entrypoint rooted from its file URL. Before starting suites, require all direct suite scripts/inputs and PowerShell 7 plus Windows PowerShell 5.1; missing input/runtime is an explicit failure. Run actual token generator --check, app syntax, existing web recovery, Studio runner/controller suite, desktop token suite, and package origin + staging suites under both real PowerShell runtimes. Preserve child output and propagate spawn errors, signals and every nonzero exit; emit REQUIRED REGRESSIONS: PASS only after all suites succeed. No default skip flags, alternate manifests, fake acceptance or reduced suites.

Both local modes invoke this helper followed by entrypoint regression tests with checked exits. A required-regression failure ends the local gate with LOCAL GATE: FAIL before C++ build work; successful regressions cannot bypass build/CTest. All four Windows build/test jobs invoke the same two real Node entrypoints with explicit Get-Command and LASTEXITCODE guards; existing C++ build/CTest, installer and assets checks remain mandatory. No release permissions/triggers change. Replace strict fixed configure log with an owned mktemp directory used for config/build/test logs, checked creation and cleanup trap; display full configure diagnostics on failures while preserving final nonzero gate result. Cleanup removes only known owned log files and its empty directory. This owns script logs only: runtime C++ fixtures still require root's exclusive full/raw CTest lease.

## Acceptance groups

1. Registration and fail closed: standard local fast/strict and Windows build variants require current runner/controller, web67, desktop token tests, generator parity and origin/staging conformance under both PowerShell runtimes. Node/missing required input/child failure cannot yield success. No C++ targets or existing tests/assertions suppressed.
2. Meaningful entrypoint regressions: copy actual current verification sources and necessary fixture assets into a GUID/mkdtemp owned OS-temp leaf, initialize its own Git repository and track its fixture sources using real Git. Positive control invokes the shared helper and runs actual suites. Negative cases invoke the actual standard scripts/verify-gate.sh, remove required script, alter actual generated token bytes, append a real failing assertion to a copied existing runner test, inject nonzero exit after actual package staging, and omit required Node/PowerShell runtime from child PATH. Require nonzero exit, the intended failure diagnostic, LOCAL GATE: FAIL and absence of LOCAL GATE: PASS/the helper PASS verdict. The helper runs before self-tests, so intentional failures avoid recursion and stop before build/CTest. No fake build tools or skip flags are used. These inert copies are entrypoint fault-injection tests, never product acceptance substitutes. Additionally assert local/Windows registration remains present; real full gates verify successful caller execution.
3. Ownership/evidence: no product edit before PLAN_ACCEPT and final repin admission; add regression tests first and capture RED against missing/unregistered helper, then minimal implementation and GREEN. Unique config log ownership includes mktemp failure/nonzero configuration diagnostics; no runtime-isolation claim. Focused actual tests, root-leased fast/strict gates and independent cumulative non-author review are required before commit. Tests use the repinned accepted Studio E01 + current origin-helper source; no old-producer result counts.

## Ordered verification and dependencies

- Root non-author PLAN_ACCEPT is recorded externally in operations/resume-reconciliation-and-gate01-plan-accept-r3.json and in local immutable receipts. The clean tree has advanced to root's accepted combined Studio E01 + origin-helper exact SHA dddf739ca2be87090ba1f18a2e320982f2528f86, preserving plan/local receipts and refreshing source/plan/HEAD bindings. Four owned product inputs are unchanged; relevant E01 producer/helper dependencies now match root acceptance. Final repin admission remains required before product mutation; no stale-producer evidence counts for final acceptance.
- Persist exclusive writer claim and immutable RED/implementation/test receipts bound to plan/HEAD/source hashes; verify before edits.
- Write entrypoint tests and capture RED. Run node scripts/tests/verify-required-regressions-tests.mjs after implementation; syntax checks for both helpers, Bash syntax and YAML validation.
- Run node scripts/tests/required-regressions.mjs against current actual sources, capturing unpiped output/exit. No hidden package-runtime skip.
- Request root's exclusive full/raw CTest lease; configure own build/mingw with MinGW/UCRT64 then run scripts/verify-gate.sh and --strict, logs in own tree. Root still owns serial fixture execution across workstreams.
- Reconcile accepted E01/current origin projection hashes before final evidence. Keep dependency changes outside allowlist and re-run affected entrypoints on combined source.
- CODE_READY for separate non-author review before commit; commit only after root coordinates accepted evidence. No push/main/release authorization assigned here.

## Bounded assertion-semantics follow-up proposal (no implementation in GATE01)

Do not turn every removed assert line into an unconditional failure: refactors legitimately move equivalent checks and raw deleted-line heuristics miss weakening through changed expected values, staged changes or commits. A separate task should bind a reviewed baseline to exact base/candidate SHA and full staged/unstaged plus commit-range test diffs, flag deleted/changed assertions, skips and threshold edits as review-required, then require independently recorded semantic dispositions for each flagged hunk. Evidence must come from the coordinator's non-author review tied to actual immutable source hashes; repo-authored approval stamps are not authority. The reviewer verifies preserved behavior using replacement coverage and failure-path evidence, rejecting real suppression or weakened semantics. Base selection/review provenance and handling pure moves need design acceptance before automation. Current NOTE remains a known bounded gap; mandatory semantic review remains enforced by the delivery workflow.

## Limits

These automated gates establish current-source engineering regression evidence only. NativeUIA standalone MSVC probe remains separately required and outside root CMake; it is not counted here. No CI success, VM/lab, candidate runtime, measured Focus FPS, signing-key custody or production-readiness claim follows from helper results.
