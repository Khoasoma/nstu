# Task Plan: Release Installer Provisioning — Scoped Firewall Rules & Lifecycle

- **Task Identifier**: `INSTALLER-FIREWALL-PROVISIONING-1`
- **Stream**: Roadmap P1.1 (Release Installer & Compatibility Preflight) / Task 8
- **Component**: `packaging`, `setup`, `ci`
- **Allowlisted Files**:
  - `packaging/nstu-installer.nsi.in`
  - `packaging/test-installer-firewall-provisioning.ps1`
  - `.github/workflows/windows.yml`
  - `tasks/task-plans/installer-firewall-provisioning-1.md`

---

## 1. Context & Motivation

Per `REVISED_ROADMAP.md` (P1.1 / Task 8):
> *"Verify what the current release installer already provisions, including server startup model, data ACLs, firewall scope, role selection, client service lifecycle, upgrade/repair, rollback and uninstall retention."*
> *"Make only the verified missing provisioning step self-contained in the signed release artifact. Limit firewall rules and privileged actions to the required executable/profile/scope."*

### Evidenced Gap:
In the current release installer (`packaging/nstu-installer.nsi.in`):
- `nstu-server.exe` listens on TCP port 47001 (control plane) and UDP port 47001 (discovery beacon receiver).
- The NSIS installer does not configure any Windows Defender Firewall rules for the server. As a result, fresh installations are blocked by default on Windows unless a human manually adds rules or answers a prompt.
- `packaging/test-system-setup.ps1` detects this missing configuration and emits warnings:
  - *"No enabled inbound TCP 47001 allow rule was found. Add a rule scoped to the classroom VLAN and NSTU server executable."*
  - *"No enabled inbound UDP 47001 discovery rule was found. Add a rule scoped to the classroom VLAN and NSTU server executable."*

---

## 2. Technical Design

### A. Least-Privilege Rule Construction
The installer runs with `RequestExecutionLevel admin` and will invoke `netsh.exe advfirewall firewall` commands during Server installation:
1. **TCP Control Plane Inbound Rule**:
   - Rule Name: `"NSTU Server (TCP-In)"`
   - Direction: `in`
   - Action: `allow`
   - Program: `"$INSTDIR\server\nstu-server.exe"`
   - Protocol: `TCP`
   - LocalPort: `47001`
   - Profile: `private,domain` (public profile is strictly prohibited)
   - Enable: `yes`
2. **UDP Discovery Inbound Rule**:
   - Rule Name: `"NSTU Server Discovery (UDP-In)"`
   - Direction: `in`
   - Action: `allow`
   - Program: `"$INSTDIR\server\nstu-server.exe"`
   - Protocol: `UDP`
   - LocalPort: `47001`
   - Profile: `private,domain`
   - Enable: `yes`

### B. Lifecycle & Rollback Cleanup
1. **Uninstall**:
   - When `Uninstall.exe` executes for the Server role (`nstu_uninstall_server:`), it deletes both rules:
     - `netsh.exe advfirewall firewall delete rule name="NSTU Server (TCP-In)"`
     - `netsh.exe advfirewall firewall delete rule name="NSTU Server Discovery (UDP-In)"`
2. **Installation Rollback / Abort**:
   - If server file promotion or registration fails (`nstu_server_abort:`), the rules are deleted to ensure no orphaned firewall rules survive a failed install.

### C. Automated Validation
- A new automated test `packaging/test-installer-firewall-provisioning.ps1` verifies:
  1. Presence and correctness of TCP and UDP inbound rules in the NSIS installer template.
  2. Strict enforcement of `profile=private,domain` (ensuring `public` is never enabled).
  3. Program scope pointing strictly to `$INSTDIR\server\nstu-server.exe`.
  4. Mirroring deletion of both rules in the uninstaller.
  5. Rollback deletion on server abort.
- Integrated into `Validate installer and exam assets` step in `.github/workflows/windows.yml`.

---

## 3. Baseline Source Hashes

- `packaging/nstu-installer.nsi.in`: `87dc82d50f8b13b3498695c804b4205c644ad64b8ca935df428ffd04261052ad`
- `packaging/test-system-setup.ps1`: `5bed99b3606083bd3790709d182a208c431894b6da9feee3ac88285c5bee23bd`
- `.github/workflows/windows.yml`: `14f5a034405bb85deadc08ae42f2193609d22594b59da836fee72c0646927535`

---

## 4. Verification Protocol

1. `scripts/verify-gate.sh` (fast gate): 0 compiler warnings, all 61 C++ tests, 63 web tests pass.
2. `scripts/verify-gate.sh --strict`: 0 warnings, all 61 C++ tests pass.
3. NSIS syntax check: `makensis /WX build/mingw/nstu-installer.nsi`.
4. Automated firewall test: `pwsh packaging/test-installer-firewall-provisioning.ps1`.
5. Feature branch push & GitHub Actions 4-job CI monitoring.
6. Fast-forward merge to `main` & GitHub Actions 5-job release workflow monitoring.

