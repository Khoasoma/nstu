# PKG01: restore package allowedOrigins compatibility

Status: PLAN_READY (revision 2, disposable signing fixture admitted by root); no product edits before independent PLAN_ACCEPT.

Owner: delivery_package, sole writer for the package files below. Root coordinates review, full-gate lease, publication and integration. Human Git identity remains khoasoma <songtu438@gmail.com>; no attribution.

## Revision, scope and baseline

Managed worktree: C:/Users/xkhoa/.codex/worktrees/autonomous-pkg01/nstu-ui-reviewed-20261107

Exact clean starting HEAD: edb79e872339e372de9ee662ed71e9a0cacef154 (detached). Source reference is D:/Codex/im/work/nstu-ui-reviewed-20261107. Live application, its processes, Coding Agent checkpoint and all other worktrees/build outputs are outside write scope. The product-owner autonomous mandate explicitly authorizes this isolated repair and independent review. WORKSPACE_SCOPE, AGENTS and tasks/autonomous-workflow.md apply; private original roadmap and _agents remain unpublished.

Product/test allowlist (only):

- packaging/stage-exam-package.ps1
- packaging/test-exam-origin-contract.ps1 (new, actual stager integration tests)
- tests/exam_origin_package_projection.mjs (new, read-only Studio projection fixture helper)

Plan/evidence: this plan, local unpublished _agents/autonomous-resume ownership/operation receipts and build/pkg01-evidence outputs. No schema/native parser changes are admitted. No Studio JS/HTML/CSS or shared Studio tests, client/service/agent, Focus/main.cpp, CMake or global verification scripts may change. The actual stager path is packaging/, correcting a historical scripts/ path assumption.

SHA-256 baseline (actual bytes, before edits):

| Path | SHA-256 |
| --- | --- |
| packaging/stage-exam-package.ps1 | 761BC249628D7A7B39DE60C0A1E8D32F3222F84E5A896EF0EAA968FB3E6FB96F |
| packaging/test-exam-origin-contract.ps1 | absent, new owned file |
| tests/exam_origin_package_projection.mjs | absent, new owned file |
| exam/schema/manifest.schema.json | 96DF3D03C9991ADB4630379378CDF0A0D726ECEF854A7440602F108BFE5190FB |
| exam/studio/studio.js | 228819D8EB4EF5638D0C6BB45057B59377EA0F0609C23EA01D59FB77C8BE8979 |
| client/src/exam_host.cpp | C01463F745DCAAAD40A623B2BE59BC2AD1BC2C17B97695C95F485C5DA1C18A84 |
| client/src/exam_lockdown_controller.cpp | 9AB9FF7ACA2B34ACDE78EA044EE360B91F3A7902FD9E11A2D874282492794B56 |
| client/include/nstu/exam_lockdown_controller.hpp | 49968E4170C936C60CEB8CBCF2366215565CA7E0CF505969C19B14C51AA54C9D |
| packaging/test-exam-package-staging.ps1 | 770E5BD09BFAD25CC3B7E004E13DEC14D61BC653711998ADAE23401DDBD5D9EA |
| tests/exam_lockdown_controller_tests.cpp | D9A228C9EBCF9B04097F597A52BBB16B8300D7807B81CC7E9CDB5AE810F84D5F |
| scripts/verify-gate.sh | 1E0683D109608DAC248E5B96B6EDE5D165CBDD96C7E8018ACFA78E29953609EA |
| tasks/todo.md | E98BE6C268650B749EC4B3377D10C973AEBBF39CEEAB12DCC21FDA7496DB17CC |
| tasks/autonomous-workflow.md | 0A00D42FDEB6EA668A25B6F9F48098ED810741DC87341D51FE76105F246C3434 |

## Actual contract and correction

Producer: exam/studio/studio.js compileManifest emits allowedOrigins only for a nonempty authoring array and preserves its strings. Manifest schema v1 permits an optional array, at most 32 unique strings of length 1..253, including an explicit empty array. The native exam host validates schema-like shape plus parse_origin_hostname before inspection and extract_exam_lockdown_policy. That parser accepts printable ASCII without whitespace or @, bare DNS labels or case-insensitive http(s) prefixes (also its existing // prefix), optional path/query/fragment suffixes and decimal port 1..65535 of at most five digits; labels are ASCII alphanumeric/hyphen, 1..63 chars without leading/trailing hyphen or empty labels. Exact string duplicates fail; case variants remain distinct. The host also rejects total strings over 253 bytes.

Stager Test-ExamManifest currently omits allowedOrigins from its case-sensitive supported-properties list, so every exported nonempty list fails before signature validation. Repair only that compatibility gap: admit the existing property and enforce its schema shape plus the native semantic parser rules using strict PowerShell type checks, ordinal duplicate handling and explicit ASCII parsing. Preserve original manifest bytes, origin strings and order. Do not use Uri normalization, DNS resolution, case folding of stored values or broad wildcard policy. No authoring/schema/parser change, deleted network policy, invented product manifest/signing authority/autosave, or altered lockdown behavior.

Trust boundary: hostile ZIP and manifest bytes entering an administrative stager. Assets at risk are accepted package integrity, publisher identity and package filesystem. Existing archive digest, UTF-8/JSON, unknown-property, schema/asset/path/resource, CMS signer/thumbprint, content digest, reparse and atomic publication checks retain their order and behavior. Origin validation remains inside manifest shape validation before publisher signature, extraction/publication and final content-digest acceptance. No native app, service or security state changes occur in tests.

## Acceptance groups

1. Compatibility: actual current Studio compileManifest projection packages with omitted and nonempty allowedOrigins stage in explicit unsigned local test mode; schema-valid explicit empty array and semantic boundaries (32 values, 253-char hostname, ports and permitted prefixes/suffixes) stage byte-for-byte. Unknown and casing-variant properties still fail.
2. Fail closed: scalar/object/null arrays, nonstring/empty/too-long/too-many/duplicate entries, whitespace/control/non-ASCII, userinfo, unsupported schemes, malformed DNS labels and ports fail before publication. Origin strings cannot bypass native semantics. Manifest-before-signature and archive-digest-before-manifest validation order remains covered.
3. Integrity and evidence: default unsigned refusal, signed-success with a disposable nonproduction certificate, malformed/tampered signature refusal, configured publisher/thumbprint requirement and mismatch, content/archive tamper, original bytes/digests and no transient published artifacts are covered against the actual child-process stager. Existing package staging suite and focused native origin tests remain green; full fast/strict gates run only under root's exclusive lease in this tree. Independent CODE_ACCEPT is required before any commit or integration. Package tests do not establish real publisher issuance/trust custody, candidate network enforcement, Save/Open/import round trips or runtime/production acceptance.

## Sequence and commands

1. Independent reviewer returns PLAN_ACCEPT (or scoped corrections). Root owns cross-review assignment; no product mutation before that verdict.
2. Verify unchanged source/owner claim. Add new tests first. Use a helper that reads the authoritative Studio source and evaluates only its actual compileManifest function against inert fixtures; emit projection bytes, never run UI handlers, write autosave or alter Studio. Disposable schema-only explicit-empty/invalid fixtures supplement actual projection output and are identified as conformance fixtures.
3. RED: run PowerShell 7 actual staging conformance script and capture failing exported nonempty-origin acceptance before repair. Confirm failure is the unsupported allowedOrigins property, while omitted-origin control stages.
4. Minimal stager implementation; rerun to GREEN, inspect source diff and unchanged non-owned hashes.
5. Run existing packaging/test-exam-package-staging.ps1 under both supported PowerShell 7 and Windows PowerShell 5.1; run new conformance suite under both, Node helper syntax check and native origin/controller test target in own build. Request root exclusive full-gate lease; configure own build/mingw, run scripts/verify-gate.sh and scripts/verify-gate.sh --strict unpiped with logs/exits in build/pkg01-evidence. Never write another owner's build directory.
6. Independent read-only cumulative code review and receipts; root coordinates exact-revision integration, subsequent integrated gates/CI and INDEV status. Leave changes reviewable until CODE_ACCEPT.

Typical focused command: pwsh.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File packaging/test-exam-origin-contract.ps1; compatibility command powershell.exe with the same arguments. Full gate uses C:/Program Files/Git/bin/bash.exe with UCRT64 before other compiler directories.

Tests create a GUID-named temporary leaf beneath the OS temp directory, verify it belongs to the current test and is below the resolved temp root before recursive cleanup, and never read real credentials. No existing signed-success fixture or public certificate fixture was found in packaging/tests/exam. Root explicitly ruled that an ephemeral test-only key/certificate is permitted for package conformance and that the mandate's prohibition concerns invented product signing authority. Generate a disposable RSA key and self-signed certificate in memory only, clearly labeled NSTU PKG01 TEST ONLY, create detached CMS fixtures for the exact projection bytes, and embed only the public certificate/signature in temporary package archives. Pass its explicitly selected public thumbprint to the actual child stager. Dispose the key/certificate; never register a trust-store certificate, persist/export private key bytes, access actual publisher material, or change product trust checks. Cover signed-success, wrong/missing configured publisher and altered manifest with stale signature. This proves the existing CMS/explicit-thumbprint test path only and is not real publisher acceptance.

## Risks and limits

The schema's string constraints are broader than native DNS semantics; the stager deliberately retains native semantics, preventing acceptance of schema-only values the host rejects. Existing native protocol-relative // acceptance is compatibility, not a new policy. Existing parsing allows path/query/fragment suffixes; matching it does not assert WebView navigation or WFP endpoint enforcement. This repair restores an established producer-consumer contract and does not broaden candidate network policy. Producer-side empty-list omission is preserved; an explicit empty list is tested separately. Separate known Studio data-integrity tasks remain owned by Coding Agent.
