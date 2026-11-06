# Interim web01: preserve exported runner bytes

Owner: root, the sole product writer/build owner on feature/interim-default-ui.
The independently reviewed web plan is WEB_UI_IMPLEMENTATION_SLICES_PLAN_R1.json (31e348a3554cc0303d89d274c4c0d905ba0f9d006ed1d8c00ac802c93b5024b7); PLAN_ACCEPT review is WEB_UI_INDEPENDENT_PLAN_REVIEW_R1.json (1e2b48469427828c1c8986920dfb0e48da73ae3b9fa6ea937bb1799bbfcc26a8).

Allowed files: this plan, scripts/sync-exam-runner-template.mjs, tests/exam_runner_template_tests.mjs and exam/studio/template.js. The three canonical exam/web sources, Studio Export/ZIP implementation, host, package validation and backend contracts remain read-only.

Baseline template SHA256: 73f20c021314227b631dd8d81a6fef6f51e5c93a044ed30c52ccff8107bf9cee. Canonical sources: index.html 9e9d69c97b66960cac08ad5adf85575f2946475f81889d3bca3053333c995046; styles.css 37b44040055e4054e1d08504023617b181f02787592714ad86a609e7d4ff326a; app.js 5f8be95db4ea4ffe5b79e557f98474959e0e10b1f3d837d15069633fb1d87b5d.

Acceptance: deterministic plain-string template values must reproduce the canonical UTF8 bytes exactly, including BOM/newlines; --check must report drift without writing. The existing real Export/Download must include manifest plus three nonempty, byte-equal runner files. No changed ZIP/digest/staging semantics or new capabilities.

Verification: first reproduce metadata-object failure with a Node parity test; regenerate and check exact bytes; drive actual Studio Export/Download using bundled Playwright and preinstalled Chrome in a fresh isolated context; independently parse downloaded ZIP with Python zipfile; run existing Node recovery and repository fast/strict gates. Root captured the actual pre-fix Download: all three runtime entries were zero bytes. Independent code review precedes commit. Desktop Computer Use was stopped by the user; this slice uses isolated browser testing only.
