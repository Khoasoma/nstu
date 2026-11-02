# Task Result: Release Installer Provisioning — Scoped Firewall Rules & Lifecycle

- **Task Identifier**: `INSTALLER-FIREWALL-PROVISIONING-1`
- **Stream**: Roadmap P1.1 (Release Installer & Compatibility Preflight) / Task 8
- **Status**: **COMPLETE & INTEGRATED TO MAIN**
- **Feature Commit**: `420dacbf2e3e9c05c97bbc79c5f44f8ac72f6832`
- **Main Commit**: `420dacbf2e3e9c05c97bbc79c5f44f8ac72f6832`
- **Feature CI Run**: `41430779354` (4/4 jobs green)
- **Main Release CI Run**: `41431740676` (5/5 jobs green)

---

## 1. Summary of Changes

Addressed the evidenced installer provisioning gap identified in `REVISED_ROADMAP.md` (P1.1 / Task 8) and `packaging/test-system-setup.ps1`:

### Key Implementation Details:
1. **Server Inbound Firewall Rule Creation**:
   - `packaging/nstu-installer.nsi.in`: during Server role installation (`Section -Server`), provisions two least-privilege Windows Defender Firewall rules via `netsh.exe advfirewall firewall add rule`:
     - `"NSTU Server (TCP-In)"`: `dir=in action=allow program="$INSTDIR\server\nstu-server.exe" protocol=TCP localport=47001 profile=private,domain enable=yes`
     - `"NSTU Server Discovery (UDP-In)"`: `dir=in action=allow program="$INSTDIR\server\nstu-server.exe" protocol=UDP localport=47001 profile=private,domain enable=yes`
   - Explicitly excludes `public` profile, ensuring classroom traffic is admitted only on managed school LANs or domain-joined networks.
2. **Uninstall and Rollback Teardown**:
   - Server uninstall (`nstu_uninstall_server:`): automatically deletes both rules via `netsh.exe advfirewall firewall delete rule`, preventing orphaned entries.
   - Server installation abort / rollback (`nstu_server_abort:`): automatically deletes both rules if staging or registration encounters an error.
3. **Automated Verification Test**:
   - Created `packaging/test-installer-firewall-provisioning.ps1` to assert rule presence, precise port/executable scoping, lack of public profiles, and rollback/uninstall deletions.
   - Integrated into the CI asset validation step in `.github/workflows/windows.yml`.

---

## 2. Verification Receipts

- **Local Fast Gate**: `scripts/verify-gate.sh` (0 warnings, 61/61 C++ tests, 63 web recovery tests passed).
- **Local Strict Gate**: `scripts/verify-gate.sh --strict` (MSVC / Release + `-Werror`, 0 warnings, 61/61 C++ tests passed).
- **Packaging Test**: `pwsh packaging/test-installer-firewall-provisioning.ps1` (5/5 assertions passed).
- **GitHub Actions Feature CI**: Run `41430779354` (DEV unprotected installer, WebView2-enabled host build, build-test, Internal VM test installer all passed).
- **GitHub Actions Main CI**: Run `41431740676` (All 4 build/test jobs plus Development release passed).
