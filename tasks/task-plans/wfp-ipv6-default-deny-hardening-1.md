# WFP-IPV6-DEFAULT-DENY-HARDENING-1 Task Plan — November 1, 2026

## 1. Context and Objective

In the current codebase, `nstu::net::WfpWebsiteAllowlist` (`common/src/wfp_allowlist.cpp`) configures outbound packet filtering using the Windows Filtering Platform (WFP), but suffers from two significant security limitations identified in [REVISED_ROADMAP.md](file:///C:/Users/xkhoa/Documents/Codex/2026-08-30/im/work/nstu/REVISED_ROADMAP.md) and `client/include/nstu/exam_lockdown_controller.hpp`:
1. It binds only `FWPM_LAYER_ALE_AUTH_CONNECT_V4`; IPv6 outbound connections (`FWPM_LAYER_ALE_AUTH_CONNECT_V6`) are completely unmonitored and unblocked.
2. It blocks only remote ports 80 (HTTP) and 443 (HTTPS); other ports (e.g. 8080, 8443, non-HTTP tunnels) are permitted, and it does not implement true default-deny lockdown.
3. Filters use `FWPM_FILTER_FLAG_PERSISTENT`, risking permanent network lockouts if the service terminates unexpectedly.

Task 18 (`WFP-IPV6-DEFAULT-DENY-HARDENING-1`) hardens WFP lockdown into a true default-deny outbound firewall across both IPv4 and IPv6, while permitting essential exam infrastructure (loopback, teacher server, DNS, DHCP, and allowed document origins), and converting to dynamic session-bound filters for fail-safe recovery.

---

## 2. Technical Requirements & Architecture

### 2.1 Dual-Stack WFP Layers
- Add `FWPM_LAYER_ALE_AUTH_CONNECT_V6` (`kAleAuthConnectV6`) alongside `kAleAuthConnectV4`.
- In `WfpWebsiteAllowlist::apply`:
  - Open a dynamic WFP engine session (`FWPM_SESSION0` without `FWPM_SESSION_FLAG_DYNAMIC`, but filters omit `FWPM_FILTER_FLAG_PERSISTENT` so filters are automatically deleted by the OS kernel if the process dies).
  - Add sublayer `kSubLayerKey`.
  - Install a **default-deny block filter** at weight 1 on both IPv4 and IPv6 layers.
  - Install **permit filters** at weight 8 for:
    1. **Loopback Traffic**: Permit `127.0.0.1/8` on IPv4 and `::1/128` on IPv6 to ensure WebView2 virtual host `https://nstu.exam/` and local IPC remain functional.
    2. **Teacher Server Endpoint**: Permit outbound TCP to the configured teacher server IP address and control/video port.
    3. **Core Network Services**: Permit DNS (UDP/TCP port 53) and DHCP (IPv4 UDP 67/68, IPv6 UDP 546/547) so host network interfaces do not lose lease or name resolution.
    4. **Declared Exam Document Origins**: Permit outbound connections to IP addresses resolved from manifest online-document origins.

### 2.2 Data Structures & Config Schema
Update `WfpAllowlistConfig` in `common/include/nstu/wfp_allowlist.hpp`:
```cpp
struct Ipv6Address {
    std::array<std::uint8_t, 16> bytes{};
};

struct WfpAllowlistConfig {
    std::vector<std::uint32_t> allowed_ipv4;
    std::vector<Ipv6Address> allowed_ipv6;
    std::uint32_t server_ipv4 = 0;
    Ipv6Address server_ipv6{};
    std::uint16_t server_port = 0;
    bool default_deny = true;
    bool permit_loopback = true;
    bool permit_dns_dhcp = true;
};
```

### 2.3 Fail-Safe Teardown
- Ensure `WfpWebsiteAllowlist::clear()` cleanly enumerates and deletes all added filters, sublayer, and provider objects.
- Destructor automatically invokes `clear()`.
- On non-Windows platforms, provide functional stubs matching the existing cross-platform build behavior.

---

## 3. Allowlisted Product Files (5)

1. `common/include/nstu/wfp_allowlist.hpp`
2. `common/src/wfp_allowlist.cpp`
3. `client/include/nstu/exam_lockdown_controller.hpp`
4. `client/src/exam_lockdown_controller.cpp`
5. `tests/wfp_allowlist_tests.cpp`

---

## 4. Verification & Testing

1. **Unit Tests (`tests/wfp_allowlist_tests.cpp`)**:
   - Verify config structure, default settings, and IPv4/IPv6 parsing.
   - Verify key generation uniqueness across filter permit entries.
   - Verify non-Windows mock execution and Windows API return status handling.
2. **Local Gates**:
   - Fast gate: `./scripts/verify-gate.sh` passes 100% with zero compiler warnings.
   - Strict gate: `./scripts/verify-gate.sh --strict` (Release with `-Werror`) passes 100%.
3. **CI Validation**:
   - Feature branch CI passes 4/4 Windows jobs (`gh run watch`).
   - Main branch CI passes 5/5 jobs (4 Windows jobs + Development release).

---

## 5. Execution Sequence

1. Author operations receipts directory `_agents/autonomous-resume/operations/wfp-ipv6-default-deny-hardening-20261101/`.
2. Check out feature branch `feature/wfp-ipv6-default-deny-hardening`.
3. Implement `WfpAllowlistConfig` expansion and IPv6/default-deny logic.
4. Expand `tests/wfp_allowlist_tests.cpp` to cover IPv6 and default-deny conditions.
5. Run fast and strict local gates.
6. Commit with identity `khoasoma <songtu438@gmail.com>` (zero attribution lines).
7. Push feature branch, verify CI, merge fast-forward to `main`, and verify main CI.
