# Interim web02: keep compact references reachable

Owner: /root/ui_exam_audit is the sole product writer for this admitted slice; /root owns all tests, builds, browser evidence, review and commits.

Freeze: WEB_02_SOURCE_WRITER_FREEZE_R1.json; source HEAD 5f14103c3450f2d3b5af79fa9542a9458861aef5. Accepted web plan SHA256 31e348a3554cc0303d89d274c4c0d905ba0f9d006ed1d8c00ac802c93b5024b7.

Allowlist: exam/web/styles.css, exam/studio/template.js, and this task plan. All HTML, IDs, data attributes, app.js handlers, exam state, storage, ACK/recovery, telemetry, native sources and Git are read-only.

Baselines:
- styles.css: 37b44040055e4054e1d08504023617b181f02787592714ad86a609e7d4ff326a
- template.js: 2cce0e59f7fc0756db3f6399cf4c23873f443f423965d21a870c2547138be0e4
- generator: 1d77a79e03572171cd1ae4117445ea4486e87adcf9aa5367b0823cf4e31cdf69
- index.html: 9e9d69c97b66960cac08ad5adf85575f2946475f81889d3bca3053333c995046
- app.js: 5f8be95db4ea4ffe5b79e557f98474959e0e10b1f3d837d15069633fb1d87b5d
- existing recovery test: 91c4357fe0642bfc5392acd41bba97ab5d298a4cc9edf5823a20a3abc36593c4

RED: root recorded actual production candidate source at 960x640 with referencesVisible=false and no JS errors in build/ui-receipts/web02-red-browser.json.

Change: preserve existing CSS palette/type conventions. Set desktop rail190/reference240 and flexible min-width0 question region. Below1100 stack the existing reference pane below questions; use an owned workspace scroll and nonoverlapping wrapped header/actions. Preserve scale100-130 and all reference tab behavior. Regenerate the existing embedded template from canonical web files.

Acceptance: PDF/Notes visible and focusable at960x640 and130%; no document horizontal overflow at960/1100/1280/1440; retained notes/independent answers through reference/layout changes; unchanged app.js/HTML; exact updated CSS bytes in exported ZIP.

Writer execution allowed: node scripts/sync-exam-runner-template.mjs only. The writer runs no browser, test, build, native app, install or Git mutation.

Root verification: actual isolated Chrome DOM notes/PDF switching and retained data, real Download ZIP parity, template --check and targeted parity test, existing Node recovery63, fast/strict gates, independent non-author exact-source review. Native WebView2 acceptance remains root-owned.

Root correction after writer release: actual browser inspection at130% exposed overlapping compact rows. The question surface extended438pixels past the reference panel's top. Explicit max-content grid rows retain each section's intrinsic height in the owned scroll viewport. Verification now requires the compact reference panel to start after the full question surface, using actual DOM bounds. This remains a CSS-only correction; HTML, handlers and durable state are unchanged.
