# WFP-IPV6-DEFAULT-DENY-HARDENING-1 Result Summary — November 1, 2026

## 1. Execution Summary

Task 18 (`WFP-IPV6-DEFAULT-DENY-HARDENING-1`) has been successfully executed, locally verified under fast and strict gates with zero warnings, fully qualified across all 4 Windows jobs on GitHub Actions CI, fast-forward merged to `main`, and qualified across all 5 jobs on `main` CI (including Development release).

- **Feature Branch**: `feature/wfp-ipv6-default-deny-hardening`
- **Feature Commit**: `65f13d4bf505ff3b2d9c4258ee2dd9831994074e`
- **Main Merge**: Fast-forward merge to `main` at `65f13d4bf505ff3b2d9c4258ee2dd9831994074e`
- **Author Identity**: `khoasoma <songtu438@gmail.com>` (zero attribution lines)
- **Feature CI Run**: `41361156780` (4/4 Windows jobs passed)
- **Main CI Run**: `41361711660` (5/5 jobs passed including Development release)

---

## 2. Changes Delivered

1. **Dual-Stack ALE Outbound Filtering**:
   - Added support for both `FWPM_LAYER_ALE_AUTH_CONNECT_V4` and `FWPM_LAYER_ALE_AUTH_CONNECT_V6`.
   - Introduced `Ipv6Address` struct supporting standard C++20 default comparison and spaceship operator (`<=>`).

2. **Default-Deny Policy with Weight 20 Permits**:
   - Weight 1: True default-deny block filters on both IPv4 and IPv6 layers when `default_deny = true`.
   - Weight 20: Explicit permit filters for:
     - Loopback (`127.0.0.0/8` and `::1/128`) ensuring WebView2 local virtual host (`https://nstu.exam/`) and local IPC remain functional.
     - Teacher server endpoint (`server_ipv4` / `server_ipv6`, with optional `server_port`).
     - Core network infrastructure: DNS (remote port 53 UDP/TCP) and DHCP (v4 remote port 67 UDP, v6 remote port 547 UDP).
     - Declared exam document origins (`allowed_ipv4` and `allowed_ipv6`).

3. **Fail-Safe Session-Bound Teardown**:
   - Omitted `FWPM_FILTER_FLAG_PERSISTENT` (setting `filter.flags = 0`) to prevent permanent machine lockout on unhandled crash.
   - Implemented destructor auto-clearing and RAII move construction/assignment.
   - Enhanced `WfpWebsiteAllowlist::clear()` to deterministically clean all fixed keys, dynamic IPv4/IPv6 permit keys, sublayer, and provider.

4. **Exam Lockdown Controller Dual-Stack Integration**:
   - Implemented `is_disallowed_ipv6` to reject loopback, link-local, unspecified, multicast, site-local, IPv4-mapped disallowed, discard prefix, and documentation prefix.
   - Expanded `ExamLockdownArmParams` to support `allowed_ipv6`, `server_ipv4`, `server_ipv6`, `server_port`, `default_deny`, `permit_loopback`, and `permit_dns_dhcp`.

5. **Unit & Integration Test Coverage**:
   - Authored `tests/wfp_allowlist_tests.cpp` with 6 exhaustive test groups (struct ordering, default configs, IPv4/IPv6 classification, non-admin runtime safety, and controller dual-stack arming).
   - Registered `nstu_wfp_allowlist_tests` in `tests/CMakeLists.txt`.

---

## 3. Verification Evidence

- **Fast Verification Gate** (`./scripts/verify-gate.sh`):
  - Zero compiler warnings.
  - 57/57 C++ unit and integration tests passed.
  - 63 browser state recovery tests passed.
- **Strict Verification Gate** (`./scripts/verify-gate.sh --strict`):
  - Release build with `-Werror` passed with zero compiler warnings.
  - 57/57 tests passed.
- **GitHub Actions CI (MSVC `/W4 /WX` Release)**:
  - Feature run `41361156780`: 4/4 jobs passed (`build-test`, `WebView2-enabled host build`, `Internal VM test installer`, `DEV unprotected installer`).
  - Main run `41361711660`: 5/5 jobs passed (`DEV unprotected installer`, `Internal VM test installer`, `WebView2-enabled host build`, `build-test`, `Development release`).
