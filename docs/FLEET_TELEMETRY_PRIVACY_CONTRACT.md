# Fleet Health Telemetry Privacy Contract & Retention Specification

**Specification Version**: 1.0  
**Phase**: Priority 3 / P3.1 Fleet Health and Diagnostics  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1 & Cross-Cutting Privacy Gates) & `tasks/product-execution-plan.md` (`FLEET-01/02`)  
**Scope**: Client-to-server fleet health telemetry, diagnostic metadata, operational monitoring, and retention guarantees.

---

## 1. Executive Summary & Policy Objectives

The Network School Teaching Utility (NSTU) platform is designed for classroom instruction, lab administration, and assessment lockdown. In accordance with NSTU privacy-by-design principles, fleet diagnostics exist strictly to maintain equipment reliability, detect software version drift, and troubleshoot network bottlenecks.

This document establishes the binding privacy contract and retention rules governing all fleet health and diagnostics telemetry across client and server systems.

---

## 2. Field Inventory, Purpose, and Collection Trigger

Fleet health monitoring collects only the justified, low-frequency, change-driven operational metrics required for classroom administration.

| Field Name | Type / Bounds | Primary Operational Purpose | Collection Condition & Frequency |
|---|---|---|---|
| `hostname` | ASCII string ($\le 255$ bytes) | Computer workstation identification in physical classroom topology | Sent during initial handshake and periodic connection heartbeat ($\ge 5\text{s}$) |
| `address` | IPv4 string ($\le 45$ bytes) | Local network routing, subnet segmentation, unicast fallback | Derived from established TCP socket endpoint; updated on reconnection |
| `delivery` | Enum (`multicast`, `unicast`) | Real-time video stream transport mode optimization | Reported during stream negotiation or dynamic fallback |
| `latency_ms` | `uint32_t` (milliseconds) | Network round-trip latency monitoring to detect congestion | Computed over active control channel ping/heartbeat loop |
| `packet_loss_per_mille` | `uint32_t` ($0 \dots 1000$) | Network reliability and switch buffer overflow diagnosis | Sampled over 10-second rolling loss windows |
| `streaming` / `snapshotting` / `viewing_broadcast` | Booleans | Active classroom screen sharing and monitoring state | Event-driven status change or heartbeat state reflection |
| `uwf` (phase, overlay, outcome) | Enum & bounded string ($\le 256$ bytes) | Disk write filter protection verification for exam and lab sanitization | Event-driven report following configuration request or boot detection |
| `last_seen` | Monotonic `time_point` | Stale-data detection; flags unresponsive clients after 30 seconds | Evaluated locally on server upon packet receipt |
| `client_version` | ASCII string ($\le 32$ bytes) | Workstation client build version to detect software skew and outdated agents | Sent once upon authenticated connection establishment |
| `agent_connected` | Boolean | Verification that the student user-session agent process is responsive | Event-driven update on named-pipe IPC connection or disconnection |
| `mac_address` | String (6-byte hex format) | Wake-on-LAN target identification for bulk boot actions | Reported once upon network adapter discovery |
| `wol_support` | Enum (`supported`, `unsupported`, etc.) | Prevents broadcasting ineffective Wake-on-LAN magic packets | Detected locally via NDIS query on client service startup |

---

## 3. Strict Negative Invariants (Non-Collected Data)

NSTU fleet diagnostics explicitly exclude invasive client-side inspection. The following categories of data are **strictly prohibited** from fleet telemetry, codecs, and server memory:

1. **Zero Raw Keystroke / Input Logging**: Fleet telemetry never records, transmits, or logs keyboard strokes, modifier history, or raw input timings outside authorized locked assessment scoring pipelines.
2. **Zero Clipboard Inspection**: Fleet telemetry never accesses or inspects the Windows system clipboard.
3. **Zero Web Browsing / URL Tracking**: Fleet telemetry does not inspect user browser history, cookies, active tabs, or search queries outside active WFP network boundary lockdown.
4. **Zero Personal / Student Identity**: Fleet telemetry tracks workstations (`hostname`, MAC, IP), never student identities, email addresses, usernames, or account credentials.
5. **Zero Process Listing / Enumeration**: Fleet telemetry does not transmit the list of running processes or background applications.
6. **Zero CPU / RAM / Disk Resource Polling**: Fleet telemetry avoids high-frequency resource utilization polling.

---

## 4. Role-Based Access Control (RBAC) & Viewer Authorization

Access to fleet diagnostics is governed by the `OperatorRole` matrix (`server/include/nstu/operator_role.hpp`):

| Role | Fleet Surface Permission | Scope of Access |
|---|---|---|
| **Teacher** (`OperatorRole::teacher`) | **Denied** | Limited strictly to active classroom screen wall (`room_screens`), student help requests, and focused demonstration modes. Disruptive maintenance actions are hidden and disabled. |
| **Technician** (`OperatorRole::technician`) | **Permitted** (`OperatorAction::view_fleet_health`) | Full access to Technician Fleet Health dashboard (`fleet_health`), stale-data indicators, network telemetry, version audit, and UWF protection arming. |
| **Developer** (`OperatorRole::developer`) | **Permitted** (`OperatorAction::view_fleet_health`) | Full access to Technician Fleet Health dashboard and raw graphics/protocol diagnostics. |
| **Unauthenticated / Student** | **Denied** | Zero access to fleet health tables or remote workstation telemetry. |

---

## 5. Retention & Purge Guarantees

1. **Volatile In-Memory Lifetime**:
   - Fleet health metadata is stored exclusively in volatile server memory within `ClientRegistry`.
   - Fleet telemetry is **never written to disk, journals, or persistent databases**.
2. **Immediate Session Termination Purge**:
   - When a client disconnects, its active session telemetry is marked stale immediately and purged upon registry eviction.
   - When the teacher server shuts down, all fleet telemetry resides in volatile memory and is instantly discarded without leaving local disk residue.
3. **Isolation from Exam Telemetry**:
   - Fleet health metadata is completely isolated from discrete exam assessment telemetry (`exam_telemetry_journal`), preventing any cross-channel contamination or long-term storage of operational health metrics.

---

## 6. Support Bundle Sanitization Guidelines

In compliance with `REVISED_ROADMAP.md` (P3.1: "Support bundles require preview/consent and tested sanitization"):
- Any future diagnostic export or technician support bundle must present an interactive preview with explicit operator consent.
- All exported support bundles must automatically sanitize and redact pre-shared keys, session keys, enrollment tokens, local network secrets, student filenames, and private hostnames.

