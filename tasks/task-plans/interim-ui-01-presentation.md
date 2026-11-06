# Interim UI 01: shared native presentation

The approved interim UI handoff defines a shared Light/Dark semantic palette,
Segoe UI typography and96-DPI geometry. Implement this presentation foundation
before moving owning screen renderers. UI-09/10 are component/theme infrastructure,
not new application destinations. Existing style already resets before scaling;
preserve that invariant and the Vietnamese/DX11 DPI reload lifecycle.

Allowed product files: server/app/interim_presentation.hpp, server/app/main.cpp,
tests/interim_presentation_tests.cpp and append-only tests/CMakeLists.txt.

Acceptance: exact handoff tokens; presentation-only adapter; body14/section16/title22/
caption12; absolute one-pass scale; actual adapter tests for repeated theme switching
and contrast; nstu-server build and focused/full gates; actual non-author review.
Preserve original assertions, handlers, roles/privacy/SAS/backend contracts and all
unrelated work. Native visual/runtime/VM acceptance remains separate. No main merge.

Verification: configure build/mingw with the frozen ImGui1.92.0 source; tests-first
missing-header compile RED, then nstu_interim_presentation_tests and nstu-server;
ctest for interim_presentation/operator_role/classroom_roster; existing fast/strict
gates unpiped with UCRT64-first PATH and D: temp/log/build output. Full gate count is
72 only once the appended target is admitted. Reviewed feature commits and PR only.

Exact private source/command and review freeze: SLICE_01_PRESENTATION_PLAN_R1.json
in the UI implementation operation. This plan is implementation scope, not a new
design package. Remaining owning screens proceed in separately reviewed slices.
