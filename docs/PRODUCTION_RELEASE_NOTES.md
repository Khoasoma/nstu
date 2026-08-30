# Project NSTU Production Release

This release contains signed Windows server and client installers. The release
must be published only after the production validation record in
`docs/PRODUCTION_VALIDATION.md` has evidence for every required environment.

Verify the Authenticode signatures and the accompanying `SHA256SUMS.txt` before
deployment. Client installation and removal require an administrator and a
Windows restart. Deep Freeze systems must be booted Thawed for installation,
enrollment, upgrade, validation, and removal.
