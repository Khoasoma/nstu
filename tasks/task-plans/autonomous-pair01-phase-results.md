# PAIR01 phase receipts

Owner: delivery_pairing. Worktree: `C:/Users/xkhoa/.codex/worktrees/autonomous-pair01/nstu-ui-reviewed-20261107`. Initial/current HEAD: `edb79e872339e372de9ee662ed71e9a0cacef154`, detached, no commits or integration. PAIR01A plan SHA256 `69C89BD75A671BC1AE057EFBEEEE6E858528B1D2947C565270AEBCD789ED08C4` independently accepted by root/package; root's receipt SHA256 `2b59c465b1419277df0e17a49effed584bbde35aa99a291ced00ae4518c0c2de`.

## PAIR01A — CODE_READY (scoped; full delivery incomplete)

Exactly four product files changed. No service/agent/server wiring, crypto, network, persistence, policy, old payload implementation or existing test assertion was changed. New local message kinds 42/43/44 and strict request/reply/end codecs are additive. New kinds require valid payloads at generic encode/decode boundary; empty kind42 remains rejected, preserving the unchanged existing client_ipc assertion.

| Product file | Current SHA256 |
|---|---|
| `client/include/nstu/agent_protocol.hpp` | `900590EDC0649AAB5A0B86033429B0625B2C47FCC4B273E1FD6C43D6AAC4D142` |
| `client/src/agent_protocol.cpp` | `CE10D93995D698D996CEE37E0A895479877FA8B9A2D86C65D88D5643EA18063F` |
| `tests/service_pairing_confirmation_tests.cpp` | `25E37CD151A7721304E25514121EAB8179F8C8721CB55CAD274461F5D3A3589E` |
| `tests/CMakeLists.txt` | `35D54A293BB4BB25BAD173CC32872012793FE7218F6E8A0E26B53E21DB94855F` |

Baseline: own MinGW Debug configure/build exited 0, zero warning diagnostics. Four existing suites passed 4/4 in 27.08 seconds: pairing_transport, client_ipc, client_pairing, ui_pairing. Log `build/pair01-evidence/baseline-pairing-tests.log`, SHA256 `A0F759472CFEF2880661F27485BC3865E0CB47B90F49EFF5E88EC58BD3BE70CC`.

RED receipts:

- Initial frame-support test failed with `decoded.has_value()` (exit `-1073740791`). It initially used an empty request. After broader verification exposed existing empty-kind42 rejection, this incomplete fixture was corrected; it is not the final compatibility regression evidence.
- Corrected valid-request RED compiles the baseline decoder from exact `git show edb79e8:client/src/agent_protocol.cpp` in own evidence output with the unchanged named-pipe implementation (no pipe opened). A hand-derived complete type42 request frame is rejected, assertion `decode_agent_message(wire).has_value()` fails, build exit 0/run exit `-1073740791`. `phaseA-valid-request-red-run.log` SHA256 `A4E797EAC890B309A00D3461062A39A0D1BB9A0F925195E7999037435B50BF30`. Harness `build/pair01-evidence/phaseA-valid-request-red.cpp` and baseline decoder snapshot remain evidence-only, never staged.
- New malformed-frame rejection test failed against the first additive decoder: assertion `!decode_agent_message(wire)` for empty kind42, build exit 0/run exit `-1073740791`. This led to validating only new kinds at generic frame boundary, preserving old-kind frame behavior. `phaseA-frame-validation-red-run.log` SHA256 `4BF70D16048559517CEA7FC16D965A44A3D166061A85AF8A300713CAE49D5D0B`.

Final GREEN:

```text
cmake --build build/mingw --target nstu_service_pairing_confirmation_tests nstu_client_pairing_tests nstu_ui_pairing_tests nstu_client_ipc_tests nstu_pairing_transport_tests -j 4
build exit 0, zero compiler warnings
ctest --test-dir build/mingw --output-on-failure -R 'nstu\.(service_pairing_confirmation|client_pairing|ui_pairing|client_ipc|pairing_transport)$'
1/5 nstu.pairing_transport .............. Passed 0.28 sec
2/5 nstu.client_ipc ..................... Passed 0.14 sec
3/5 nstu.client_pairing ................. Passed 1.41 sec
4/5 nstu.ui_pairing ..................... Passed 20.45 sec
5/5 nstu.service_pairing_confirmation ... Passed 0.09 sec
100% tests passed, 0 tests failed out of 5
Total Test time (real) = 22.38 sec
ctest exit 0
```

Final build log `build/pair01-evidence/phaseA-complete-build.log`, SHA256 `2831240A68DD8F8DA71B5F39E37A6107B0C5F6D7EF48AF1E594949E4009F25C1`. Test log `build/pair01-evidence/phaseA-complete-tests.log`, SHA256 `474236481E8D6018B7AD7B883A60FCC2AD46E2B2D69347907C5A281F5A516222`.

Additional owned source/test object compile: GNU C++20 `-O2 -Wall -Wextra -Wpedantic -Werror`, both compile exit 0 and empty diagnostics. Log `build/pair01-evidence/phaseA-strict-compile-final.log`, SHA256 `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`. This is scoped warning verification, not the full strict gate or MSVC CI. `git diff --check` exit 0. All old kind values and old choice/selection/code/status fixtures remain preserved; malformed token/choice/flag/code/text/expiry/truncation/trailing payload tests pass.

Full fast/strict gates NOT RUN: root assigned current exclusive gate lease to Focus. Windows CI/native runtime/physical human comparison NOT TESTED. Product pairing still lacks real callback/UI teacher visibility until B/C/D; no incomplete integration is authorized. Await independent PAIR01A CODE_ACCEPT before any phase commit, and fresh phaseB admission before product changes.

## PAIR01B predecessor binding prepared

Current HEAD remains above; A cumulative bytes exactly match table. B new helper/header paths remain ABSENT. `client/CMakeLists.txt` remains SHA256 `0F04741B9C7458E5548DC033B4D15C0BB59A72BBBDB6347E5D9D49E6A61DBD7E`. B shared test predecessor SHA256 is `25E37CD151A7721304E25514121EAB8179F8C8721CB55CAD274461F5D3A3589E`. A codec headers/source are read-only dependencies in B with the current table hashes. Revalidate after any reviewer correction/commit; changed bindings require a new admission. B/C/D have design acceptance only, no product write admission yet.

## PAIR01B — original CODE_READY snapshot (rejected by B-R1/B-R2 review)

Root accepted phase A source and phase B plan `F19451592BA659ED84282B1B568DBD3BE2CC6245A4594415B4C82871CE739D4D`, with independent review receipt SHA256 `997b6060c9cd0e131dedbae0194e07257c4b81f5c20c83ab1bd15c9026a1aef8`. Root persisted the exact predecessor admission. No commits/integration were made; HEAD remains `edb79e872339e372de9ee662ed71e9a0cacef154` plus accepted A and current B dirty bytes. No main/server files changed. The A codec header/source and tests CMake remain exactly their accepted hashes above; earlier A test assertions are retained.

Exactly five B product files:

| Product file | Current SHA256 |
|---|---|
| `client/include/nstu/service_pairing_confirmation.hpp` | `870F70456763AB11E908F489BFAAB0EA42C47936BD18EACB0FA75958A73068CB` |
| `client/include/nstu/agent_pairing_confirmation.hpp` | `969ED29C02FCDAF27C03DBE0486937AC2FAB88A146D4CF62D230DC0567D5B893` |
| `client/src/service_pairing_confirmation.cpp` | `CC2468DDBDE0DD8A97553CAF6DFE918C7FC73772D09E4C11A5BF2CDB556B7660` |
| `client/CMakeLists.txt` | `AF09AF201D46C134B5F5A0A7347AE459135A5D0AF9C988B7B1CC1DB6FE3A0F31` |
| `tests/service_pairing_confirmation_tests.cpp` | `8CCCA68A3FCC37A584640A281B90E449F444A135F58B86029DE222E2FCFBB5B7` |

Implementation: random 128-bit prompt token, monotonic maximum 180-second timeout, validated WTS session plus unique connection generation, separate begin-admission generation to prevent delayed starts superseding newer attempts, and at-most-250-ms callback slices with stop wake. New/invalid same-peer starts invalidate prior state. Current peer/token are required for reply, dispatch and final credential handoff; disconnect/cancel/replay/supersession fail closed. Queue/network/sink/RNG calls occur outside broker locks. Terminal handoff consumes the current decision atomically; cancellation or peer loss while waiting for teacher cannot hand back an enrolled config. Student input model never confirms on receipt, requires actual presentation and exact explicit typed six digits, and consumes one terminal reply. No native main/UI wiring exists yet.

Real production wrapper constructs the existing student callback and invokes unchanged `pair_with_server`. It exercises only refusal/error paths over real TCP in this phase. Positive crypto/enrollment integration deliberately waits for C's actual teacher visibility API; no hidden teacher SAS is used to bypass comparison. State-only affirmative unit tests use the fixed public `123456` fixture and do not enroll any device or open any operational pipe.

RED evidence (build exits 0, assertion run exits `-1073740791`):

- Missing production callback seam: old display-only real TCP invocation reported `failed` rather than explicit `student_refused`. `phaseB-refusal-red-run.log`, SHA256 `F9AFC4FADA5F40635DB4F545BF0C6216984E01022A98E6A705FFAFA6A82F9BB3`.
- Extreme configured seconds caused the underlying pairing deadline to overflow before refusal could run. `phaseB-overflow-red-run.log`, SHA256 `BD86782436C7AE635299A159579A4AD71294FA22D601F2DA568386F627F4A000`. Fix clamps both broker timeout and actual library option before duration conversion.
- Invalid new same-peer attempt left the old attempt alive. `phaseB-supersession-red-run.log`, SHA256 `298D685D14977CA8A7997718B7F30968B77C3C188908D38F422FE3F7CDD396D7`. Fix invalidates before preparation and protects delayed preparation with a monotonic admission generation.

Final scoped verification, exact hashes above:

```text
cmake --build build/mingw --target nstu_service_pairing_confirmation_tests nstu_client_pairing_tests nstu_ui_pairing_tests nstu_client_ipc_tests nstu_pairing_transport_tests -j 4
build exit 0, zero warnings
ctest --test-dir build/mingw --output-on-failure -R 'nstu\.(service_pairing_confirmation|client_pairing|ui_pairing|client_ipc|pairing_transport)$'
1/5 nstu.pairing_transport .............. Passed 4.56 sec
2/5 nstu.client_ipc ..................... Passed 0.13 sec
3/5 nstu.client_pairing ................. Passed 1.13 sec
4/5 nstu.ui_pairing ..................... Passed 20.40 sec
5/5 nstu.service_pairing_confirmation ... Passed 2.59 sec
100% tests passed, 0 tests failed out of 5
Total Test time (real) = 28.82 sec
ctest exit 0
```

Log `phaseB-final-build.log` SHA256 `3DBC297550437930B477EDB70CA4EA7E1A15BFFDAE93F6C2CDCA4E627E45E8AE`; `phaseB-final-tests.log` SHA256 `24D10E9B41AADA16FC7B28815161F7643895ABEDAA1F93046B62EC36A00BA208`; owned source and test object compiles with GNU C++20 `-O2 -Wall -Wextra -Wpedantic -Werror` both exit 0, empty `phaseB-strict-compile.log` SHA256 `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`. `git diff --check` exits 0. All log paths are under own `build/pair01-evidence/`. An initial unqualified Clock type build error and copied loop-variable warning were fixed; no suppressions or assertion weakening were added.

Coverage: bounded slices, stop wake, empty/invalid teacher entry, unshown prompt, mismatch, rejection/close semantics, timeout/expiry equality/future overflow, duplicate replies, wrong/stale tokens, same/different WTS generation, stale disconnect/end, invalid supersession, malformed payload, enqueue failure, cancellation/disconnect before and after state acknowledgment, and token-bound final handoff. Real TCP negative paths return no key/config and server key count stays zero.

Full fast/strict gates/Windows CI/actual UI/human two-endpoint operation remain NOT TESTED. Root's C01 global gate lease is preserved; no full gate runs from this owner. B source awaits independent CODE_ACCEPT. C source writing is blocked pending root's accepted Focus-byte incorporation and refreshed actual HEAD/source pins plus phase-specific admission. Current still-unmodified C source: server main SHA256 `5A73B2879B19B3796636E2368DD4A52B6F54D38A54E918F5B6141FAB5124DC60`; control header `4DE76F40CD668952FD8E77CD7ED6ED9EBC77A07E568DDBF902D47FAD0DF3339F`; control source `47884F9CACCC4305C3FAF8D7E3D7199177C1791E1D6508DB786C38E6B1BC5EE8`; new pairing_request_view.hpp ABSENT. Preserve exact A/B dirty bytes during Focus import. No incomplete flow may be committed/published/integrated.

## PAIR01B repair — B_CODE_READY after B-R1/B-R2 (2026-11-07)

Root upheld independent `CODE_REJECT_PHASE_B` receipt SHA256 `6e95903edf35dbdc773801e420c135aa472f3d0117ae2b124c70a75555ce3305`. B-R1 found expiry missing at final consumption; B-R2 found an external end-message sink after the authority guard. Both were verified in source and with new public-fixture REDs before repair. No source outside B's five-file scope was edited; accepted A codec/header and tests CMake hashes remain unchanged. HEAD remains `edb79e872339e372de9ee662ed71e9a0cacef154`; no server/main edits, commit, publication or integration.

Current five-file B source binding:

| Product file | Current SHA256 |
|---|---|
| `client/include/nstu/service_pairing_confirmation.hpp` | `40A532355B41A57D0A7413088B19AFC405B9E5DB2DCB21BA98C2FF18EAB64D62` |
| `client/include/nstu/agent_pairing_confirmation.hpp` | `969ED29C02FCDAF27C03DBE0486937AC2FAB88A146D4CF62D230DC0567D5B893` |
| `client/src/service_pairing_confirmation.cpp` | `11D74915208A56FC6C9D0EAC87A50214D32830FDBAB2273793508C8F76ABB7B0` |
| `client/CMakeLists.txt` | `AF09AF201D46C134B5F5A0A7347AE459135A5D0AF9C988B7B1CC1DB6FE3A0F31` |
| `tests/service_pairing_confirmation_tests.cpp` | `8C0328C57CC85B3C2B87F9EAC421CA41F05D256DC2EBF82D92220C15CB11DC44` |

B-R1: `finish_attempt` rechecks both steady deadline and absolute GetTickCount64 expiry under its mutex and consumes expired state as timed_out. A valid shown/matching public `123456` acknowledgment cannot outlive the original broker window merely because the underlying pairing function starts another teacher wait. RED `phaseB-R1-red-run.log` SHA256 `81811A9037F32AC4055F8F8D7A8EC435968177CB5DA15A74FA22AEDE7B8E9542`: assertion expected timed_out after confirmed-state expiry; build exit 0/run exit `-1073740791`. GREEN `phaseB-R1-green-{build,run}.log` exits 0.

B-R2: the production wrapper now uses shared `finish_interactive_confirmation`, exercised directly with public digits. All external cleanup enqueue callbacks finish before the last atomic broker consumption. That consumption checks token/current validated peer/current decision/deadline/payload expiry and stop. A failed/throwing cleanup enqueue refuses. Cleanup callback cancellation, stop, peer replacement or expiry cannot return an affirmative decision. RED `phaseB-R2-red-run.log` SHA256 `164E55728AE70BEAB631140C2B41D02D6E2E6429D0C9C3D250CCD01B5F7321AC`: unchanged extracted finish-before-cleanup sequence returned an affirmative despite cleanup invalidation; build exit 0/run exit `-1073740791`. GREEN `phaseB-R2-green-{build,run}.log` exits 0. Tests deliberately re-enter broker.peer inside the external sink to catch sink calls under its lock.

Nonbreaking teacher-wait cancellation: the existing code observer binds a per-attempt transport stop_source to the broker; the wrapper passes its token into unchanged pair_with_server and relays caller cancellation. Cancelling, ending, disconnecting, replacing or superseding an already confirmed broker attempt captures its stop_source under the mutex, invalidates state there, and calls request_stop only AFTER unlocking. That wakes/cancels the existing bounded teacher-wait loop without adding a network command or changing crypto/issuance. RED `phaseB-stop-relay-red-run.log` SHA256 `E295D0E62BF6A701E3BE7220AC84FE3E717F4853E9F21DBB9F23F87CF9CBF1DB`: confirmed-state invalidation did not request transport stop; build exit 0/run exit `-1073740791`. GREEN `phaseB-stop-relay-green-{build,run}.log` exits 0. Stop callbacks also re-enter broker.peer, proving the relay avoids executing external callbacks under locks. Ordinary pre-confirmation refusal preserves its existing outcome; the transport relay is for revoking already confirmed state during the subsequent wait.

Handoff linearization and recovery boundary: local authority is consumed under the broker lock after the last external cleanup callback; a later asynchronous disconnect/cancel is an ensuing recovery event, not a guarantee of indefinite connectivity. Deadline expiry is refused at handoff; no new background deadline watcher or remote issuance policy was introduced. The underlying teacher loop retains its own bounded approval timeout. If the server already approved and issued a key before a late local abort/expiry, this client-side adapter cannot roll that issuance back. It clears the local returned config, returns failure/timeout, and does not authorize persistence. The existing separately human-confirmed re-pair path rotates a key for the same device identity; no automatic orphan-key rollback/recovery or new key-custody authority is claimed. C must test the real positive crypto path through actual teacher visibility, including these late-cleanup failures with truthful server-key accounting; no hidden teacher SAS success was used here.

Final exact-source scoped verification:

```text
cmake --build build/mingw --target nstu_service_pairing_confirmation_tests nstu_client_pairing_tests nstu_ui_pairing_tests nstu_client_ipc_tests nstu_pairing_transport_tests -j 4
build exit 0, zero warnings
ctest --test-dir build/mingw --output-on-failure -R 'nstu\.(service_pairing_confirmation|client_pairing|ui_pairing|client_ipc|pairing_transport)$'
1/5 nstu.pairing_transport .............. Passed 1.56 sec
2/5 nstu.client_ipc ..................... Passed 0.15 sec
3/5 nstu.client_pairing ................. Passed 1.42 sec
4/5 nstu.ui_pairing ..................... Passed 20.45 sec
5/5 nstu.service_pairing_confirmation ... Passed 3.04 sec
100% tests passed, 0 tests failed out of 5
Total Test time (real) = 26.68 sec
ctest exit 0
```

Logs: `build/pair01-evidence/phaseB-repair-final-build.log`, SHA256 `3DBC297550437930B477EDB70CA4EA7E1A15BFFDAE93F6C2CDCA4E627E45E8AE`; `phaseB-repair-final-tests.log`, SHA256 `6826F4B2CD5B857986EF87564FAAF43F6A79DEE0B3737BF65AB3A08BF1B66303`. Source/test object compiles with C++20 `-O2 -Wall -Wextra -Wpedantic -Werror`, both exit 0; empty `phaseB-repair-strict-compile.log` SHA256 `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`. `git diff --check` exit 0. No removed assertions, suppressions, global gates or policy changes.

Status: repaired B source awaits independent acceptance. No C writes/import until root coordinates accepted Focus bytes, preserves these dirty hashes and re-pins the actual predecessor for C review. Full gates/Windows CI/real native human comparison remain NOT TESTED; no incomplete flow is integrated.

## B accepted; Focus import and C preparation (2026-11-07)

Root confirmed independent corrected-B `CODE_ACCEPT_PHASE_B`, reviewer receipt SHA256 `90ba7cf9190d266856d842ebc9b352bc03363fcf2a65be4adab5ee144ca97279`. Root then explicitly authorized PREPARE C ONLY: import accepted Focus commit without staging/committing/resetting/stashing A/B, verify exact preservation, and rebind C for fresh review.

Preflight verified HEAD `edb79e872339e372de9ee662ed71e9a0cacef154`, empty index, all eight accepted A/B product hashes, all seven private plan hashes, and four Focus paths disjoint from every dirty path. Flushed immutable `build/pair01-evidence/focus-import-pre.json` SHA256 `18F3F758A1D3238D7A5528AE1AF35368812C417EB9BE65DB601C75A397A9E2FB` records those bindings. Ordinary `git -c user.name=khoasoma -c user.email=songtu438@gmail.com cherry-pick c797503192a22ce659aa4960de18d18b8f45adbd` exited 0, importing only already accepted Focus main/header/test/plan. No partial A/B was committed.

Imported current HEAD `09a677bb8c3df786140d92ff41cbad061c3bb2fa`, parent baseline, tree `3515382dcf5f5cc7b9d2ba1fabf9fa7abd5d854e` exactly matches the accepted Focus component tree. Author and committer are exclusively the required human identity, without attribution. Post-check verified every pre-recorded A/B source/private-plan hash unchanged, identical dirty path status and empty index. Main SHA256 is required `10F126FF697C8C76AD3BF0D324F07CC780FB2053D0088742E3E79F19A55F1229`; Focus header/test SHA256 `FF4D021035AF1AD1B952AD5C4DEA3CC52B05036C28D34E7EA83C5691C16DBCBB` / `093543F7BC4583951F890A091EE80C391CC6DBF0BEC5948B73F102105D3FFCD9`. Flushed immutable post receipt `focus-import-post.json` SHA256 `813525A6FD9A06BE72E963DA8939BE54738DD728D5C625B0E0EE84C4C95A2900` records the verification.

C source remains unedited: control header/source `4DE76F40CD668952FD8E77CD7ED6ED9EBC77A07E568DDBF902D47FAD0DF3339F` / `47884F9CACCC4305C3FAF8D7E3D7199177C1791E1D6508DB786C38E6B1BC5EE8`; new pairing view helper ABSENT; pairing test predecessor `8C0328C57CC85B3C2B87F9EAC421CA41F05D256DC2EBF82D92220C15CB11DC44`. Updated C plan binds current HEAD and exact five-path scope; actual product writes wait fresh root/nonauthor PLAN_ACCEPT. Future C preserves all accepted Focus sections. No full gates, additional source changes, A/B commits or publication occurred during preparation.

## PAIR01C — C_CODE_READY (2026-11-07)

Root confirmed fresh C PLAN_ACCEPT, plan SHA256 `0CD11B47D1EF8EB7D1912236B23DED2B6419FFFC2F5B3AFD582B8AB2E0C388FB`, reviewer receipt `0593167209a291ec4821d57b083107ab01191904e6e0e5ff12b97e1a4e67cf4f`. Current HEAD remains imported Focus `09a677bb8c3df786140d92ff41cbad061c3bb2fa`; A/B/C are uncommitted. Exactly five C product files changed:

| Product file | Current SHA256 |
|---|---|
| `server/include/nstu/control_plane.hpp` | `AAFC49D4550BB5D96B72F2FA98EB0CEBD1171910DFCE36CBABC17FA1E2E37A41` |
| `server/src/control_plane.cpp` | `4034327003A7399621520D2CF487AA98A668FDDEEC3E848BEEAE6F6013903FBC` |
| `server/app/pairing_request_view.hpp` | `9FBD7269B98EBCE83747A10591225382B613B3329EB2F40059C5353B4946641B` |
| `server/app/main.cpp` | `D273088E1415FEFC636B4816C98027783C3D46BDDD579CEA2AD9EA6000AC813E` |
| `tests/service_pairing_confirmation_tests.cpp` | `9A8633D0347E09EF89269DA6BA5B9B7C7A89B64CC478092AC8AFC4407ABFAE96` |

Additive `pairing_requests()` returns sorted, bounded, read-only offered metadata/SAS with explicit awaiting-client-confirmation or awaiting-teacher-approval stage. It omits expired records and exposes no secret keys/tags. The existing confirmed-only pending query and approval/issuance logic are unchanged. Names/addresses remain unverified claims; receipt of a client cryptographic tag is not independent attestation of physical student interaction.

Actual teacher `draw_pairing_requests` now consumes this view, shows localized stage text and applies the shared tested action gate plus ImGui disabled state before approval. The existing independent server-side client-confirmation/expiry guard remains mandatory. Existing styling/scaled metrics and rejection path are retained. No operator roles, auth/publisher/instructor keys, network protocol, persistence, UIA, Studio, Focus or host-security policy changed.

Behavioral RED: real offer reached the production student observer, but existing teacher pending_pairings returned no row, failing `rows.size() == 1`. Build exit 0, assertion run exit `-1073740791`; `build/pair01-evidence/phaseC-early-view-red-run.log` SHA256 `8E517CC9E5E8962E0E6139655AF9625C9081FD11BB2F4506DBEE16E0A5DF6FB6`. GREEN uses the new real read API and checks early stage/SAS, legacy pending empty, direct premature approval false, no key issuance, and teacher rejection removing the offer.

Real positive crypto coverage now reads actual teacher API SAS into the explicit student input boundary, observes legacy pending only after the client tag, checks student-only decision leaves zero server keys, and explicitly approves at the teacher afterward. Success returns a verified key. Late cleanup stop/peer replacement/queue failure returns no local credentials while truthfully retaining one server-issued key. Late cleanup expiry returns timed_out with no local key and retained issued server key. Disconnect while teacher approval is still pending uses B's stop relay and issues zero server keys. No hidden test SAS or local unchecked consent is used for positive enrollment. Concurrent real offers are sorted; closing the pairing window removes both and issues no keys. Stage helper tests reject early/unknown/zero-time/zero-id approval.

Final native target + scoped verification:

```text
cmake --build build/mingw --target nstu-server nstu_service_pairing_confirmation_tests nstu_client_pairing_tests nstu_ui_pairing_tests nstu_client_ipc_tests nstu_pairing_transport_tests nstu_focus_stream_observer_tests -j 2
[100%] Built target nstu-server
build exit 0, zero compiler warnings
ctest --test-dir build/mingw --output-on-failure -R 'nstu\.(service_pairing_confirmation|client_pairing|ui_pairing|client_ipc|pairing_transport|focus_stream_observer)$'
1/6 nstu.pairing_transport .............. Passed 0.38 sec
2/6 nstu.client_ipc ..................... Passed 0.02 sec
3/6 nstu.client_pairing ................. Passed 1.39 sec
4/6 nstu.ui_pairing ..................... Passed 20.42 sec
5/6 nstu.service_pairing_confirmation ... Passed 7.58 sec
6/6 nstu.focus_stream_observer .......... Passed 0.04 sec
100% tests passed, 0 tests failed out of 6
Total Test time (real) = 29.85 sec
ctest exit 0
```

Logs: `phaseC-final-build.log` SHA256 `2499E560E1F80D0859ADED911758005A47B231D7E2B7D475E84D7782FC56C78F`; `phaseC-final-tests.log` SHA256 `0315B69AAE4CE8F9ED11EAF46492678F5FBBC0E3212856DA232DBCB1E7581164`. Direct source/test object compile with C++20 `-O2 -Wall -Wextra -Wpedantic -Werror`, current generated server include response/defines and enabled test assertions, both exit 0; empty `phaseC-strict-compile.log` SHA256 `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`. This is scoped optimized warning verification, not MSVC or the full strict gate. `git diff --check` exit 0.

Preservation proofs: `phaseC-preservation-pre.json` SHA256 `F56CE66A8F6E9A1F278CE1460027158879D743B4F9D183F9B6B894CCDD6AD0E5` bound the three exact Focus main sections plus nine protected A/B/Focus files before C edits. Post proof `phaseC-preservation-post.json` SHA256 `E20FABB003A48E32E0909FE5C2A05E0B8B52F0DFF2060DB25EB9DDF7B7A323F5` verified all protected file hashes and exact raw Focus fields/loop/shutdown sections unchanged; normalized source text of existing pending_pairings and approve_pairing bodies also matches the accepted predecessor. No A/B test assertion was removed. Root-owned source/checkpoints/builds remain untouched.

Limits: the actual native teacher binary was compiled but not launched against host operational state; visual multi-DPI/Vietnamese/physical human UI and UIA tests are NOT TESTED. NSTU ImGui guidance was applied proportionately to the functional stage label/action guard; referenced private design-system/catalog files are absent from this isolated tree, so existing approved styling was preserved. The long 180-second server-offer expiry was not separately timed; its existing expiry predicate is retained in the new read view, while actual client/broker expiry, cleanup, reject/window-close and disconnect paths were tested. Full combined gates and Windows CI remain root-owned/pending. No D service/agent source changes, partial commit/publication/integration or production operations occurred.

D fresh design/source pins are prepared in `autonomous-pair01d-wiring.md` against this exact C predecessor; they require C CODE_ACCEPT and new D admission before edits. The D plan explicitly preserves the SAS while waiting for teacher approval and uses the existing cancelled local reply for current-token revocation during that wait; duplicate affirmatives cannot restore authority, and no codec bytes or issuance policy change.

## PAIR01D — CODE_READY (2026-11-08; cumulative review and full gates pending)

Root resumed admission r1 explicitly transferred implementation ownership to `/root/pairing_resume` after confirming the predecessor author was absent and all dirty bytes preserved. Private writer claim and immutable implementation intent are under `_agents/autonomous-resume/`; those private files are untracked and must never be staged. HEAD remains `09a677bb8c3df786140d92ff41cbad061c3bb2fa`, index empty. No partial A/B/C/D commit, publication, integration, installed service launch or machine policy operation occurred. Independent C CODE_ACCEPT/D PLAN_ACCEPT review SHA256 `7102FA64DA71E532AEFA3CCDCBE6702BB353F44463F0917F0908FEFEE426D782`; exact D plan remains `ACAB1BCEB4A18C8CBA8A16C819D3A57171290932684315736F715EEF272BD182`.

Only D's five product files changed. Every accepted read-only A/C/Focus/CMake/agent-model file (11 distinct paths) matches its admission hash. A reconstruction of every original A/B/C test body/assertion and original main sequence matches the exact accepted test predecessor `9A8633D0347E09EF89269DA6BA5B9B7C7A89B64CC478092AC8AFC4407ABFAE96`; added tests append D behavior and preserve the original assertions. C approval/issuance/late-server-key accounting remains unchanged.

Production service `attempt_pairing` now calls `pair_with_interactive_agent`, binds enqueue/drain to the validated image/WTS session and connection generation, revalidates that current pipe at reply/cancellation admission, filters expired/superseded requests, and purges only pairing traffic on replacement/disconnect/stop. Queue capacity failure refuses without evicting unrelated messages. Existing broker final expiry, external-cleanup-before-final-consumption and stop guards remain. Current-token cancelled6 after the one affirmative revokes it and requests transport stop outside locks; duplicate/stale affirmatives cannot restore it. The service rechecks stop immediately before config persistence and clears local credentials on failure.

Actual native UI pipe dispatch receives the model without acknowledging presentation. Its UI thread draws formatted six-digit SAS and teacher name, creates an empty labeled native teacher-code edit plus explicit Codes match/Codes differ/Reject, and focuses the edit. The shared production dispatcher reads actual `GetWindowTextW` input. Successful current SAS paint, visible usable controls, UI thread and the token bound to the actual empty controls are required before presentation. An intervening new token with repeated digits cannot use an old edit before its posted update. Match retains the SAS and cancellation token, disables repeat approval/input, and waits for independent teacher comparison. Close/Escape/Reject/destruction/expiry/end/pipe loss refuse or revoke; older end tokens cannot hide newer prompts. Existing native theme/DPI/keyboard conventions are retained, without a UI redesign or automatic confirmation.

Final D source binding:

| Product file | SHA256 |
|---|---|
| `client/service/main.cpp` | `6200ABC0018CA5A7C749C5413174858CD253E7C2D04CBC9F406B01BA634A1473` |
| `client/agent/main.cpp` | `960640C8F6CBE8D260314B31C5FBF233BE0306E9530EF0F240F43A98E1D5A9C6` |
| `client/include/nstu/service_pairing_confirmation.hpp` | `5316C37DBBAF3C237154AEEFCBF795A4AA67A602194A2A6EFE3F2A2A31599311` |
| `client/src/service_pairing_confirmation.cpp` | `A61BC50E7358B9A99758FA37149D5A3E283D9656F16B0687901F768365D3F11A` |
| `tests/service_pairing_confirmation_tests.cpp` | `4C5E9098C833FF562D737678D5294EC4DC04EBE2D2EB26F04E77B9CBF8EA10B7` |


Final exact-source verification:

```text
cmake --build build/mingw --target nstu-service nstu-agent nstu_service_pairing_confirmation_tests nstu_client_pairing_tests nstu_ui_pairing_tests nstu_client_ipc_tests nstu_pairing_transport_tests nstu_focus_stream_observer_tests -j 4
build exit 0; actual service and agent targets built; zero warnings
nstu_service_pairing_confirmation_tests.exe --native-only
exit 0; Pairing native dispatch and queue: PASS
ctest --test-dir build/mingw --output-on-failure -R 'nstu\.(service_pairing_confirmation|client_pairing|ui_pairing|client_ipc|pairing_transport|focus_stream_observer)$'
1/6 nstu.pairing_transport .............. Passed 36.39 sec
2/6 nstu.client_ipc ..................... Passed 0.16 sec
3/6 nstu.client_pairing ................. Passed 1.06 sec
4/6 nstu.ui_pairing ..................... Passed 20.34 sec
5/6 nstu.service_pairing_confirmation ... Passed 37.93 sec
6/6 nstu.focus_stream_observer .......... Passed 0.02 sec
100% tests passed, 0 tests failed out of 6
Total Test time (real) = 95.92 sec; CTest exit 0
```

The confirmation suite includes a process/time/sequence-unique test pipe, actual production queue and broker dispatch, image/WTS revalidation, real wrapper/crypto/TCP, actual native edit/GDI presentation seam and the real early teacher API. Student-only and premature teacher approval issue zero keys; no presentation/mismatch/reject/disconnect refuse local credentials. Current-token cancellation during teacher wait issues zero keys. Deliberately held final cleanup lets an actual pipe cancellation after server issuance clear local credentials while truthfully asserting one retained server key. No installed control pipe or operational credentials are used.

Fresh C++20 O2 `-Wall -Wextra -Wpedantic -Werror` compiles of service main, agent main, shared helper and complete confirmation tests each exit 0 with empty diagnostics. `git diff --check` exits 0. Earlier D focused/strict snapshots predate the final controls-token guard and are historical only; these final logs and final-D receipts bind the current source.

RED evidence: before broker repair, the new current-token cancelled6-after-affirmative test compiled0 and failed its admission assertion (Git Bash run exit127, Windows assertion termination). Final evidence-only mutations of actual production helper source all compile/link0: removing presentation acknowledgment fails the native presentation assertion; removing displayed-controls-token guards fails the fresh repeated-SAS supersession assertion; removing the wrapper student callback fails the unique-pipe live-prompt assertion. The three native/pipe mutation subprocesses each exit3221226505 (0xC0000409). These omission checks are regression sensitivity evidence, not a preimplementation baseline for the new dispatcher. No product mutation, test weakening or assertion removal occurs for these REDs. An initial test typo used `PairingRequest.code`; the logged compile failure was corrected to its actual `short_authentication_string` member before final GREEN.

Exact final log hashes (all relative to own `build/pair01-evidence/`):

| Evidence | SHA256 |
|---|---|
| `phaseD-revocation-red-build.log` | `A7B49AFCC8651578D887525C87FC4C665AADE05FC01293F1CACF8C48CF9255E3` |
| `phaseD-revocation-red-run.log` | `4827F3F0977BA2D77C47C2C3634C7752B9431C222A6A8320CA752B419E46E690` |
| `phaseD-final-build.log` | `4E1E15328A3A5FA8349A876E96B3C3FF9F8817B6B4838EA98C32C99194E59C74` |
| `phaseD-final-native-run.log` | `DFC08B22E4FA3DB9EBF03A604D9D263CB97C6067A019A49249A65826F315D1E3` |
| `phaseD-final-tests.log` | `F7E69FB8D547DE8289D3AD71BB31AA1B52243DFDE122A7B3F38CE167038668D0` |
| `phaseD-preserved-ABC-tests.cpp` | `9A8633D0347E09EF89269DA6BA5B9B7C7A89B64CC478092AC8AFC4407ABFAE96` |
| `final-D/phaseD-scoped-strict-omission-result.json` | `9CE2512239EF01AFCEF8EAACCF9155D26DF3AC63E73201BFD578429BC6BD95B1` |
| `final-D/phaseD-presentation-omitted-build.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/phaseD-presentation-omitted-link.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/phaseD-presentation-omitted-run.log` | `ABC26ABBA5F89E335650EE8E9D8883C4BDCFB6623D3C89E57F0589C5CA758BB3` |
| `final-D/dispatch-omission-compile-result.json` | `4A0F62A6E7B619797C351258454A51BE4D16B6124C02FA684B15B7211C5669E2` |
| `final-D/token-guard-omitted-compile.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/token-guard-omitted-link.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/token-guard-omitted-run.log` | `F3FCBC133616BB2A945A13A2E5741ED6C19295B2C7EF5D73FD525F5792E7D8DB` |
| `final-D/wrapper-callback-omitted-compile.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/wrapper-callback-omitted-link.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/wrapper-callback-omitted-run.log` | `FF5BCE4D8BBF812B63C7F301B1638FD03633503B834BC6C80E26B121F4D3D71A` |
| `final-D/wrapper-callback-omitted-runtime-result.json` | `FF87500B47602B876B699BB9A9B8D7E87B1EE935B1FE4B9D0C0FFF270ABE5DE3` |
| `final-D/phaseD-main-nstu-agent-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/phaseD-main-nstu-service-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/phaseD-service_pairing_confirmation-nstu_service_pairing_confirmation_tests-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `final-D/phaseD-service_pairing_confirmation_tests-nstu_service_pairing_confirmation_tests-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |


Limits: CODE_READY is an author handoff, not independent CODE_ACCEPT or complete delivery. Full fast/strict gates, cumulative nonauthor review, exact Windows CI and root-owned integration remain pending. The coordinator's earlier E01 gate belongs to a different tree and is not counted for PAIR. The generated service/agent binaries were built, not launched as installed production components. Offscreen native control/GDI tests prove shared dispatch mechanics; actual two-machine human comparison, native window visual/keyboard acceptance, NVDA, DPI and IME qualification remain NOT TESTED. Late cancellation cannot undo an independently approved server key; ordinary later disconnect after final authority consumption is a recovery event. No exam-safety claim is made.

## PAIR01D D-R1 — scoped repair CODE_READY (2026-11-08; re-review/shared verification pending)

Independent cumulative review SHA256 `801E872FE3482A1C96C1CED5F3622F92D3726FEECFDCCDFCAD45972E46386FB3` rejected only unchecked native edit clearing before new displayed-token binding. Root's scoped repair admission is `pair01d-reset-repair-f03-gate-authority-milestone-r1.json`. Exact repair amendment plan SHA256 `56C5B3C158D424350ECF3F879759C877EF60FDFAB7CAA294F4E7B3BFEA547896`. HEAD/index are unchanged; no commit/push/integration occurred. All 11 A/C/Focus/CMake/agent-model read-only pins and D service main remain unchanged. Removing only the new reset fixture/helper test/main additions reproduces the prior entire A/B/C/D confirmation test file exactly at SHA256 `4C5E9098C833FF562D737678D5294EC4DC04EBE2D2EB26F04E77B9CBF8EA10B7`.

Actual agent refresh now calls shared production `reset_pairing_native_input`. It unbinds before any synchronous native reset; requires a live UI-thread-owned control, `SetWindowTextW(empty)` success, then both actual length/text reads proving empty on the still-live control before binding. Reset failure or retained text leaves no displayed token; the real agent callsite sends no_ui and hides rather than acknowledging presentation. Existing current token/peer/deadline/stop/cancel/final-handoff and already-issued-server-key behavior is unchanged.

Behavioral RED used the extracted actual rejected two-statement reset/bind sequence in the shared production seam and actual agent callsite. An owned EDIT procedure receives real WM_SETTEXT(empty), returns FALSE without forwarding it, and retains old123456 while the request has a new token and repeated SAS123456. Build exits0; `--reset-only` fails `!reset_pairing_native_input(...)` with native process exit3221226505 (0xC0000409). The RED helper snapshot/log/result are immutable evidence-only files, not alternate product paths. This observed native reset failure establishes the regression without operating an installed app or production pipe.

GREEN exercises that same real reset/bind seam: before-reset callback observes token unbound; both actual failed reset and claimed-success-with-retained-text refuse binding/presentation/affirmative while actual native text remains123456. Successful reset is independently read as empty, empty positive input refuses, then fresh typing plus paint permits one affirmative and explicit cancellation. Null/destroyed controls remain unbound. All earlier native/queue tests pass unchanged. Actual service, agent and confirmation-test target build exit0 with zero warnings; `--reset-only` exit0 and `--native-only` exit0. Four O2 C++20 `-Wall -Wextra -Wpedantic -Werror` source/test objects compile0 with empty diagnostics. `git diff --check` exits0.

Repaired five-file envelope (only agent main/shared header/shared source/tests changed during this repair):

| Product file | SHA256 |
|---|---|
| `client/service/main.cpp` | `6200ABC0018CA5A7C749C5413174858CD253E7C2D04CBC9F406B01BA634A1473` |
| `client/agent/main.cpp` | `9DEF1D4C1460ABEDC6B42F1628831E7F91B9B153937B40BEE6E4434174DFDF5A` |
| `client/include/nstu/service_pairing_confirmation.hpp` | `A47AB6AA4DF21EC55748B88D7E0A5E88929480511E2285A22309204676EC5B7B` |
| `client/src/service_pairing_confirmation.cpp` | `3EAF958ECF8D01377CF345D0BC1759F104E55CFC2C5DF917AC240F5F568DAA61` |
| `tests/service_pairing_confirmation_tests.cpp` | `C8B8B83F571FF33DDA2D3318ED4EA03BCD3E4C8424F36D2595BE66AD03AD4516` |

Exact evidence under own `build/pair01-evidence/D-R1/`:

| Evidence | SHA256 |
|---|---|
| `red-build.log` | `B5C9CE5CB5007FB3F764F3D485A61D6B0BC89FA33217BEE713FB279BAABC0779` |
| `red-run.log` | `E6CF9D58C3C80B75574E2E3BB61F5983B85045F431AC39179A3DEA1DA0EED51D` |
| `red-result.json` | `AC0695539F9C841EA2A5B8D8FDB0000E3D9CB3F3259A2C46CCCB795785BD37B6` |
| `red-production-reset-source.cpp` | `15AD15CDD6E3DC760AA9856F16E1192D9AD1ABC0D241321B65E89233BB8F5BFD` |
| `green-build.log` | `62B59FF765CEEE05ED0B02F68801F0978E03FA213C3D1D430C5E06378D1C8DCA` |
| `green-reset-run.log` | `F518099306E1C5CBB8E80BFBD8EB20169B330B5ACA26242B9C923EF052146573` |
| `green-native-run.log` | `DFC08B22E4FA3DB9EBF03A604D9D263CB97C6067A019A49249A65826F315D1E3` |
| `preserved-pre-repair-tests.cpp` | `4C5E9098C833FF562D737678D5294EC4DC04EBE2D2EB26F04E77B9CBF8EA10B7` |
| `strict-result.json` | `70B30FFBDDF7D40EB8135BFC53358B842A66F5C1E2D1F14C933D7056EC3814A4` |
| `main-nstu-agent-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `main-nstu-service-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `service_pairing_confirmation-nstu_service_pairing_confirmation_tests-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| `service_pairing_confirmation_tests-nstu_service_pairing_confirmation_tests-strict.log` | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |


Scope qualification: earlier six focused/unique-pipe receipts precede this reset repair and do not certify its changed source hashes. Fresh shared six suites/unique-pipe and full fast/strict gates have NOT RUN while root GATE01 holds the exclusive shared/full/raw lease. They remain required after review/lease release. This scoped CODE_READY is for nonauthor re-review, not CODE_ACCEPT or full delivery. Exact Windows CI, physical two-machine comparison, actual installed native window, NVDA/DPI/IME and production acceptance remain NOT TESTED. The native fixture is owned/offscreen and opens no network or operational pipe. C server main remains read-only; F03 and other owners' trees are untouched.

## PAIR01 cumulative final local verification — PASS (2026-11-08)

Independent nonauthor reset re-review SHA256 `6DF96BADEB86ADA76B67576EA6616DA50F8EC6D4F2FF759217268FA2BE55837A` returned ACCEPT_D_R1 / CODE_ACCEPT_PHASE_D_CONDITIONAL_VERIFICATION with no remaining source fixes. Root granted the exclusive PAIR final full/raw lease in `gate01-combined-result-pair01-final-lease-r1.json`. Every reviewed product14/protected11 source pin, amended plan `56C5B3C158D424350ECF3F879759C877EF60FDFAB7CAA294F4E7B3BFEA547896`, HEAD `09a677bb8c3df786140d92ff41cbad061c3bb2fa` and empty index were verified before and after each operation. Per-command immutable pre/dispatch/PID/result receipts are under private `_agents/autonomous-resume/operations/`. No product edits, staged files, partial commits, publication or integration occurred during verification.

Fresh post-R1 native/real-pipe/focused results:

```text
Actual nstu-service + nstu-agent + six focused target build: exit 0, zero warnings
nstu_service_pairing_confirmation_tests.exe --reset-only: exit 0
Pairing native input reset boundary: PASS
nstu_service_pairing_confirmation_tests.exe --native-only: exit 0
Pairing native dispatch and queue: PASS
nstu_service_pairing_confirmation_tests.exe --pipe-only: exit 0
Pairing real pipe bilateral dispatch: PASS
Six focused CTests: 100% tests passed, 0 tests failed out of 6
Total Test time (real) = 32.43 sec; exit 0
```

Current repository fast gate and strict gate each completed successfully, unpiped through Git Bash with UCRT64 first. Verbatim decisive output in each gate:

```text
PASS  exam web recovery tests: 63 passed
exam first-option presentation regressions: 2 passed
exam save/finalization precedence regressions: 2 passed
PASS  build succeeded
PASS  zero compiler warnings
PASS  100% tests passed, 0 tests failed out of 75
LOCAL GATE: PASS
```

Fast gate exit0; strict gate exit0. Each verifies all75 current C++ tests and all67 browser cases with zero compiler warnings. Strict cache confirms Release + NSTU_ENABLE_WERROR=ON; the unchanged gate retains only its documented GCC libstdc++ false-positive `-Wno-free-nonheap-object`. No threshold/assertion/target/gate weakening was made. Full CTest transcripts and exact configured caches were captured before later operations could replace them. Final `git diff --check` exits0. All owned processes/sessions have completed and the exclusive shared/full/raw lease is released to root.

Cumulative14 source binding:

| Product path | SHA256 |
|---|---|
| `client/include/nstu/agent_protocol.hpp` | `900590EDC0649AAB5A0B86033429B0625B2C47FCC4B273E1FD6C43D6AAC4D142` |
| `client/src/agent_protocol.cpp` | `CE10D93995D698D996CEE37E0A895479877FA8B9A2D86C65D88D5643EA18063F` |
| `tests/CMakeLists.txt` | `35D54A293BB4BB25BAD173CC32872012793FE7218F6E8A0E26B53E21DB94855F` |
| `client/include/nstu/agent_pairing_confirmation.hpp` | `969ED29C02FCDAF27C03DBE0486937AC2FAB88A146D4CF62D230DC0567D5B893` |
| `client/CMakeLists.txt` | `AF09AF201D46C134B5F5A0A7347AE459135A5D0AF9C988B7B1CC1DB6FE3A0F31` |
| `server/include/nstu/control_plane.hpp` | `AAFC49D4550BB5D96B72F2FA98EB0CEBD1171910DFCE36CBABC17FA1E2E37A41` |
| `server/src/control_plane.cpp` | `4034327003A7399621520D2CF487AA98A668FDDEEC3E848BEEAE6F6013903FBC` |
| `server/app/main.cpp` | `D273088E1415FEFC636B4816C98027783C3D46BDDD579CEA2AD9EA6000AC813E` |
| `server/app/pairing_request_view.hpp` | `9FBD7269B98EBCE83747A10591225382B613B3329EB2F40059C5353B4946641B` |
| `client/service/main.cpp` | `6200ABC0018CA5A7C749C5413174858CD253E7C2D04CBC9F406B01BA634A1473` |
| `client/agent/main.cpp` | `9DEF1D4C1460ABEDC6B42F1628831E7F91B9B153937B40BEE6E4434174DFDF5A` |
| `client/include/nstu/service_pairing_confirmation.hpp` | `A47AB6AA4DF21EC55748B88D7E0A5E88929480511E2285A22309204676EC5B7B` |
| `client/src/service_pairing_confirmation.cpp` | `3EAF958ECF8D01377CF345D0BC1759F104E55CFC2C5DF917AC240F5F568DAA61` |
| `tests/service_pairing_confirmation_tests.cpp` | `C8B8B83F571FF33DDA2D3318ED4EA03BCD3E4C8424F36D2595BE66AD03AD4516` |

Final evidence under own `build/pair01-evidence/final-verification/`:

| Evidence | SHA256 |
|---|---|
| `build.log` | `4F75BFDB38D9A81D1E2DE180309FA644D8492B7970C835BED1000A6E7EDFA013` |
| `fast-gate-CMakeCache.txt` | `192D7CF7292B11A270B91BE21D136E851DA6BB2437AA5E3AEDF7A5BD701DCC2D` |
| `fast-gate-LastTest.log` | `B7FE1C694441995AE4A45D131C6F44856D1A692CE3F0AA1875C879A1E7A3EB82` |
| `fast-gate.log` | `A19F9911176B9FC959B996B7C7C8EFBAE8AB19A339B02FBD5AFE2D72232B4F27` |
| `focused-six.log` | `68B7FEA16738A90BCA6A84DE282AC44171AE3BC63883A689ADF25B6BCA22E183` |
| `native-dispatch.log` | `DFC08B22E4FA3DB9EBF03A604D9D263CB97C6067A019A49249A65826F315D1E3` |
| `native-reset.log` | `F518099306E1C5CBB8E80BFBD8EB20169B330B5ACA26242B9C923EF052146573` |
| `result.json` | `54EFC23802F628ED48D0E25663B56083178615FBE8F0B1FABD9EF1FB8FEA1190` |
| `strict-gate-CMakeCache.txt` | `F7D7CAF2EC47B556DD09C4E354626886CFD097A9D7AB8ED8F88CECA86D4B6AFB` |
| `strict-gate-LastTest.log` | `C3F298EB9067CDC6E09C6BD09E21094CB3FF2C175D8E3624832D8D08B232CAB0` |
| `strict-gate.log` | `FD25CAC607E1F206DFAF87B2D4A7406CAB4BA4EDC4B2974A8C14800C81BDB3D8` |
| `unique-pipe.log` | `CA9CC0DD5772429461FD9EAF31F19CBACE1CB26CA61BCDD1A349231836431704` |


These results qualify only this PAIR baseline plus complete A/B/C/D/R1 sources. This checkout lacks the coordinator's newer Studio/package/GATE changes; its75-test/67-browser gates do not certify that later combined tree. Root final CodeAccept and the complete14-product/owned-plan commit still precede publication; integrated-tree verification and exact Windows MSVC CI remain required. No source-role/authority proposal is approved or physically qualified by these tests. Actual two-machine human comparison, installed native window behavior, NVDA/DPI/IME and production acceptance remain OPEN. Native fixtures use owned offscreen controls and dedicated process-unique pipes; no installed service, live machine policy or operational credential mutation occurred. Late client cancellation still cannot roll back an already-issued independently approved server key.
