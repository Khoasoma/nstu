# USED ONLY FOR TESTING LAB

This NSTU development candidate is for supervised testing in a physical Windows lab.

**Release status: INDEV — LAB TESTING ONLY.** Production and official assessment qualification remain open.

Qualification uses the owner's physical lab. VM testing is omitted at the owner's request. Automated build and regression results cover only the tested revision; record physical test results separately.

For each lab run, record the Git revision, installer SHA256, Windows versions, machine count and network layout. Exercise installation and upgrade, six-digit pairing, Focus start/stop, question import/export, submission and reconnect recovery, keyboard access and Vietnamese input. Preserve existing data during upgrade and rollback testing.

Report defects with the revision and reproduction steps, using synthetic content and sanitized diagnostics. Physical lab results are required before considering a beta release.

## Human physical lab review

Use a supervised test machine with a recoverable backup and synthetic assessments. Each unchecked item is **not tested**. After performing an item, mark the checkbox and append **PASS**, **FAIL**, **BLOCKED** or **N/A**, with an evidence reference. A checked box alone is not a passing result. Explain every N/A; repeat failed cases after a repair on the new revision.

### Run record

| Field | Reviewer entry |
| --- | --- |
| Reviewer and date | |
| Full Git revision and Actions run | |
| Installer name, channel and SHA256 | |
| Teacher/student Windows editions and builds | |
| Teacher/student machine count and hardware | |
| Network topology, Wi-Fi/Ethernet, IPv4/IPv6 | |
| Account privilege, display resolution and scaling | |
| Backup/recovery reference and previous installed version | |

### Installation and recovery

- [ ] Confirm the installer belongs to the recorded revision and selected lab channel; retain the downloaded artifact and SHA256.
- [ ] Fresh install of the Teacher and Student roles completes on designated test machines; service and agent appear in their intended sessions.
- [ ] Standard-user startup and administrator consent/denial behave clearly; no student desktop process needs administrator privileges.
- [ ] Installation under a custom path and a path containing spaces/Vietnamese characters works without silent fallback.
- [ ] Upgrade preserves pairing, configuration and synthetic assessment/submission data; record before/after evidence.
- [ ] Reboot and service/agent restart restore expected state without granting commands to an unpaired teacher.
- [ ] On the recoverable test machine, interrupted upgrade and supported rollback preserve data and restore the expected version; record any manual recovery needed.
- [ ] Uninstall of the designated test installation reports retained/removed data accurately and leaves no active orphan service or unexpected firewall rule.

### Classroom trust and operation

- [ ] Pairing requires matching six-digit codes confirmed by humans on both machines; denial, timeout and wrong code do not establish trust.
- [ ] An unpaired or previously revoked teacher cannot issue protected commands; reconnect does not silently approve new trust.
- [ ] Roster identity, online/offline status and checked targets match the actual machines; selection stays accurate across reconnects.
- [ ] Teacher/technician permissions reject disallowed maintenance operations; protected actions do not bypass an active assessment.
- [ ] Chat reaches only intended targets and reports failure/disconnection accurately.
- [ ] Focus start, target change, stop and owner handover show the correct machine and terminate the previous stream as expected.
- [ ] Measure Focus frame rate, latency, CPU/network use and reconnect time at the intended lab size; retain measured values and acceptable limits agreed by the lab owner.
- [ ] A stale snapshot or disconnected session is labelled accurately and never presented as a fresh live frame.

### Assessment and Studio

- [ ] Open, edit, Save, import, export and reload a synthetic assessment; compare title, metadata, question identities, points and answer content.
- [ ] Cancelled or rejected import leaves the existing draft intact; malformed/oversized content produces a useful error.
- [ ] A failed file write or inaccessible destination reports failure and preserves recoverable prior data.
- [ ] Single-choice and matching Preview controls work with the keyboard; navigation and File disclosure preserve sensible focus.
- [ ] Compact windows, resize, 100%/150%/200% scaling and long question text keep controls reachable without clipping.
- [ ] Vietnamese typing, accents and input-method composition survive editing, navigation, saving and export.
- [ ] Screen-reader/keyboard review identifies labels, current selection, errors and focus order; record remaining native accessibility gaps.
- [ ] Invalid package signatures or digests are rejected; an imported server-local package is not misreported as ready on student machines.
- [ ] Begin requires actual readiness and correct targets; end/close preserves submissions and does not report an unacknowledged save as complete.
- [ ] Network loss, application restart and reconnect retain recoverable answers and reconcile submission acknowledgements without duplicate finalization.
- [ ] Sanitized diagnostics can reproduce a failed case without publishing real student content, credentials or private signing material.

### Limits and release decision

- [ ] Record network-lockdown behavior on IPv4/IPv6 and non-web ports, including permitted loopback, DNS/DHCP, allowed-origin paths and heartbeat-loss recovery. Use only benign lab connectivity probes; official exam safety is not qualified.
- [ ] Record whether every required case passed and every critical data-loss, trust or recovery defect is resolved; retain blocker IDs.
- [ ] Lab owner signs the decision: **continue lab testing**, **reject candidate**, or **request separate beta qualification**. This checklist does not itself authorize production or official assessment use.

Decision, reviewer signature, unresolved defects and evidence location: ____________________
