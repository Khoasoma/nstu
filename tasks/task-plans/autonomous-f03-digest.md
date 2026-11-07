# F03 — transactional teacher digest admission

Status: PLAN_READY; implementation requires root PLAN_ACCEPT. Owner: /root/digest_repair. Run: autonomous-f03-20261108-r1. Base: fix/autonomous-f03-digest-20261108, HEAD 59cb171f7044dcadc8776feaa08374b670fe369b. Tracked working tree and index are clean; historical untracked _agents and ignored Package build outputs remain preserved.

Objective: reject every malformed or all-zero content SHA-256 digest before selected/all-online teacher load dispatch, preserving the output digest on every parser failure. Existing contract requires exactly 64 ASCII hex characters and at least one nonzero byte; individual zero digits are valid.

## Current source and admitted writes

Current main.cpp:549–570 parses incrementally into output; a bad late nibble or all-zero value returns false after mutation. Selected load:4417–4431 checks the boolean but has inaccurate English-only invalid text. All-online:4444–4467 ignores the boolean and may pass a partial nonzero digest to launch_exam. Common exam_control.cpp:116–127 accepts a nonzero digest; host verifies package bytes, so this is invalid dispatch/loading-context admission, not a digest verification bypass. Demo export also calls the parser; retain its current demo behavior and explicitly discard the new nodiscard result there.

Only these four product/test paths may change:

| Path | Baseline SHA256 |
| --- | --- |
| server/app/main.cpp | 10f126ff697c8c76ad3bf0d324f07cc780fb2053d0088742e3e79f19a55f1229 |
| server/app/exam_digest.hpp | ABSENT (freshly checked) |
| tests/exam_digest_tests.cpp | ABSENT (freshly checked) |
| tests/CMakeLists.txt | d9d9ea805d2fcceeeeebc71dffa05f5f813eccc28aeb22d7cba1066e66345d9f |

No compatible exported canonical parser exists. The other hex parsers found are private signature-vector parsing in common/src/update_manifest.cpp or internal client/src/exam_host.cpp templates, with different contracts. Extract the existing teacher parser into server/app/exam_digest.hpp under nstu::ui, with [[nodiscard]], std::string_view and noexcept. Decode into a local Sha256Digest candidate, reject invalid ASCII and zero candidate, then assign only on success. No new callback abstraction or dispatch API.

Read-only dependency pins:

| Path | SHA256 |
| --- | --- |
| AGENTS.md | 833c40ac671d6c3a0dd6b763aa537285e6eab4ec22fcd5d119e10e85c3646be5 |
| tasks/autonomous-workflow.md | 0a00d42fdeb6ea668a25b6f9f48098ed810741dc87341d51fe76105f246c3434 |
| tasks/todo.md | e98be6c268650b749ec4b3377d10c973aebbf39ceeab12dcc21fda7496db17cc |
| CMakeLists.txt | 0997335a1ec0cb6a7a15df52d4722909011857399e743eee9e7ca39421a5c8d6 |
| server/CMakeLists.txt | 3dad785ee4ed07dc9ac45b091c0bfad41bc96cce7e012475458a090b06ba6380 |
| common/include/nstu/auth.hpp | d84cab0f5dd42ab06bfd08ebcd647d49246bdc707b04d2019990d8e90b649753 |
| common/src/exam_control.cpp | 239fe3f709db235fbc3101a9ef25f6ca54b791b8c77bce7e672c89fe98278bcc |

## Acceptance groups (three)

1. Transactional parser: meaningful input/output tests reject invalid final high/low nibble after a nonzero prefix, invalid first nibble, 0/63/65 length, leading/trailing whitespace and non-ASCII byte; all-zero input fails. Every failure preserves a sentinel output. Known upper/lower/mixed-case digest vectors, zero-containing digest and only-final-byte-nonzero input decode to expected bytes. RED evidence runs the test against an exact extraction of the current parser in private scratch; GREEN uses the actual new production helper. Add a platform-independent CMake test with existing warnings and assertion-enabling conventions.
2. Actual selected/bulk guards: validate before constructing ExamLaunchRequest or entering the bulk target loop; share the production parser, copy the accepted digest into each request, and render the same localized invalid-input status (English: "Content SHA-256 must contain exactly 64 hex characters and must not be all zeros.", corresponding Vietnamese via existing tr). Preserve selected online/active disabling, bulk online/unlaunched filters, entered selected candidate and hostname bulk candidates, path normalization, existing success text, and launch_exam/backend guards. A non-author inspects actual ImGui handlers and control-flow dominance at final source hashes. Do not claim parser unit tests or synthetic callback tests prove GUI clicks or actual backend allocations. Current narrow source admission is proved by the actual guards plus compile; independent native two-machine behavior remains owner-later.
3. Build/review: actual nstu-server compiles with zero project warnings; focused nstu.exam_digest and nstu.exam_control pass, then complete fast and strict gate semantics under root lease. A fresh non-author reviews four-path cumulative diff and receipts. Physical/native acceptance, MSVC exact-SHA CI and integration remain pending root coordination.

## Execution and commands

Before edits verify root PLAN_ACCEPT, exact accepted plan hash, HEAD/index/source pins, and _agents/f03/WRITER.json claim. Private state/receipts/logs live only _agents/f03; builds only build/f03-*; never overwrite old _agents/autonomous-resume, Package WRITER/state, build/mingw, build/gate or Package logs. Persist immutable intent/result receipts and atomically replace state with previous snapshot retained.

After acceptance, standalone RED/GREEN tests use C:/msys64/ucrt64/bin/g++.exe -std=c++20 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I common/include tests/exam_digest_tests.cpp -o build/f03-parser/exam_digest_tests.exe, with UCRT64 first on PATH. RED instead compiles the same fixture against the exact privately extracted baseline parser. Check sanitizer compiler/runtime availability separately; use isolated sanitizer compilation if supported and preserve unavailable diagnostics without making an unqualified sanitizer claim.

Root lease required before full/actual product builds and CTest: configure CMake -S . -B build/f03-fast -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DNSTU_ENABLE_PACKAGING=OFF; build --target nstu-server nstu_exam_digest_tests nstu_exam_control_tests. CTest --test-dir build/f03-fast -R "^nstu\\.(exam_digest|exam_control)$" --output-on-failure. Root may approve a copied verify-gate.sh runner under _agents/f03 with only hardcoded build directory substitutions to build/f03-fast and build/f03-strict, preserving all checks/flags/assertions. Run unpiped via Git Bash with raw logs/exit status; standard original script is read-only and old outputs remain unchanged.

Risks/limits: this fixes input admission only; scan/stage/package mapping, demo grade export, cohort synchronization, role/key/ticket/UWF/wire/crypto and host safety are outside scope. A new header must include auth.hpp for the canonical digest type; parser tests require no network or backend callbacks. Invalid digest feedback becomes localized and accurate. GUI callback source review is distinct from actual native runtime evidence. No commit, push, main integration or live application/key/role mutation is authorized to this writer before root coordination.
