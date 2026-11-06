# Interim UI-02 native shell

Implement the independently accepted UI-01 shell plan on feature/interim-default-ui after accepted palette/font commit de82b4eabe1625e14d1cdd0e4668df7be1412b3f.

## Source binding

- main.cpp SHA256: 49e19da221b932cfe4c6c31a70598eb71eae1fcdd5cbea88caf63dba03868f8b
- interim_presentation.hpp SHA256: dede11a2ad21f58021dc0aa4ddc28de682cc7d48b1e5a59af82469d4382edcee
- Writer freeze SHA256: cffd9de4074452d12d01038f2bc74d257158e2a028f4578c60950df4e4488147
- Accepted plan SHA256: 7a37d4ae5e30da5b752fd3ed99671dd586d8371ba041ffd90e1491f3db21ace7

## Exact allowlist

server/app/main.cpp: shell composition, menu/ribbon/rail/status/filter-strip renderers, shell-only measured helpers, obsolete icon/ribbon presentation wrappers, four utility trigger preambles, IO keyboard flag and root WindowPadding scope. Existing popup bodies, owner renderers, command helpers, state/role/protocol/stream/Remote loops remain unchanged. server/app/interim_presentation.hpp: optional1100DIP compact breakpoint only. This task plan is the only new product document.

## Acceptance

One200/176DIP sidebar,64+ header,44+ context toolbar,28footer, four real destinations with source role fallback. All whole-room/checked/selected/broadcast commands and shared modal hosts stay reachable. DEV marker, SAS/beacon lifetime, consent/drafts, Follow-Along input-lock guard, native keyboard activation and stable IDs preserved. No fictional destination or readiness/ACK claim.

## Verification ownership

Native profile03 reproduced edge-flush text: ImGui1.92 ignores borderless child padding by default. Root correction explicitly enables AlwaysUseWindowPadding on shell-header, shell-commands, shell-sidebar, workspace-content and shell-status; shell-body and shell-workbench remain unpadded. This changes presentation only and requires rebuilding and re-inspecting the actual native shell before code review.

Writer performs frozen dependency and outside-region/allowlist checks only. Root is sole build/review/commit owner: nstu-server build, existing relevant tests, fast/strict gates, independent review, actual safe-profile native keyboard/EN-VI/100-125-150-200%DPI/theme/resize/role/popup/scope evidence. No build/test/native/Git execution by writer. No main authorization.
