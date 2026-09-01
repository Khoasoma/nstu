# Project NSTU

[English](README.md) | [Tiếng Việt](README.vi.md) | [Development guide](docs/DEVELOPMENT.md)

[![Windows CI](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

NSTU is a free and open-source classroom and computer-lab management project
for Windows. It is designed around a centralized teacher server, lightweight
student clients, authenticated control commands, chat, low-bandwidth client
screen snapshots, and teacher-screen broadcast over a local network.

![NSTU Server dashboard in English light mode and Vietnamese dark mode](docs/assets/server-dashboard-preview.png)

*Server dashboard: English light mode and Vietnamese dark mode.*

> **Development status:** NSTU is currently an engineering MVP, not a
> production release. Persisted enrollment, multi-client dispatch, authenticated
> control routing, periodic JPEG snapshots, screen annotation, teacher-screen
> snapshot broadcast, packetization, and device recovery are implemented and
> tested. Continuous decoded H.264 delivery and the physical production
> validation matrix are not complete. Do not deploy the current nightly build
> as a security control in a real school.

## Why NSTU exists

NSTU grew from regret at seeing schools rely on unlicensed classroom software.
Software obtained from untrusted sources can be modified, exploited, or used to
turn lab computers into cryptocurrency-mining infrastructure without the
school's knowledge. At the same time, stronger copyright enforcement in
Vietnam can make it harder for schools with limited budgets to equip and manage
computer rooms legally.

We are building NSTU as a practical alternative: free to obtain, open for
inspection, easy to install, and designed with security and low-resource
hardware in mind. NSTU uses the MIT License. Schools, teachers, businesses, and
individuals may use, study, modify, and redistribute it, including for
commercial purposes, as long as the copyright and license notice is retained.

Open source does not automatically make software secure. NSTU therefore keeps
its threat model and unfinished production work public in
[SECURITY.md](docs/SECURITY.md) and [ROADMAP.md](docs/ROADMAP.md).

## Intended capabilities

- Manage 50 or more Windows clients from one teacher workstation.
- Monitor a room through bounded JPEG snapshots captured every 5-10 seconds,
  avoiding a continuous per-client video load on the classroom switch.
- Lock or unlock clients, draw on a selected student's screen through a
  click-through overlay, and broadcast periodic teacher-screen snapshots.
- Retain H.264 multicast/unicast foundations for a future optional continuous
  broadcast mode without making it the default monitoring path.
- Show a responsive wall of the latest client snapshots, plus focused
  telemetry, controls, and chat for one client.
- Run a small Windows service and native Win32 tray/chat agent on each client.
- Authenticate control and video traffic without JSON, XML, Electron, or a
  custom kernel driver.
- Keep secrets, dumps, captures, and runtime state local and outside Git.

## System requirements

The following values are project targets and still require validation on a
real 50-client lab.

| Role | Baseline target | Network |
| --- | --- | --- |
| Server | Intel Core i5-6400, 8 GB RAM, 512 MB free disk, Windows 10/11 x64 | Wired Gigabit Ethernet recommended |
| Client | Intel Core i5-6400, 8 GB RAM, 512 MB free disk, Windows 10/11 x64 | Wired Ethernet recommended |
| Router/switch | UDP multicast support, IGMPv2 or IGMPv3, IGMP snooping, and an IGMP querier | One controlled LAN/VLAN for the first deployment |

For a server supervising 50 or more devices, 16 GB RAM and an SSD are prudent
until the 8 GB target has passed long-duration hardware testing. Intel HD
Graphics 530 is a baseline hardware-acceleration target, not a guarantee across
all driver versions.

## Local resource benchmark

The following measurements are an indicative developer-machine check, not the
required 50-client production benchmark. They were recorded on September 1,
2026 from commit `f183e42`, using `build-verify/server/nstu-server.exe` with an
empty client registry and no snapshot or broadcast traffic.

Test machine: AMD Ryzen 7 5700X (8 cores/16 logical processors), 15.9 GiB RAM,
SSD storage, NVIDIA GeForce RTX 2060 SUPER, and Windows 11 Pro build 26200.

| Scenario | CPU, total machine capacity | Working set | Private memory | Method |
| --- | ---: | ---: | ---: | --- |
| Dashboard visible, idle | 0.725% average, 2.336% p95 | 53.54 MiB average, 54.50 MiB maximum | 117.95 MiB average, 118.83 MiB maximum | 120 one-second samples after a 10-second warm-up |
| Window closed to tray, idle | 0.023% average, 0.289% maximum | 52.48 MiB average, 53.00 MiB maximum | 120.60 MiB average, 121.06 MiB maximum | 60 one-second samples after a 5-second warm-up |

The empty-room run produced no application snapshot payload, so it is not a
live bandwidth benchmark. The table below is the worst-case JPEG payload model
from the implemented 60 KiB frame cap. It assumes every frame reaches that cap
and excludes authenticated-command, TCP/IP, Ethernet, retransmission, and other
control-traffic overhead.

| Snapshot traffic | 5 second interval | 7 second interval | 10 second interval |
| --- | ---: | ---: | ---: |
| One client, one direction | 0.098 Mbps | 0.070 Mbps | 0.049 Mbps |
| 50-client room monitoring, inbound | 4.92 Mbps | 3.51 Mbps | 2.46 Mbps |
| Teacher broadcast to 50 clients, current per-client TCP path | 4.92 Mbps | 3.51 Mbps | 2.46 Mbps |
| Monitoring and teacher broadcast together | 9.83 Mbps | 7.02 Mbps | 4.92 Mbps |

These values are ceilings for the current periodic snapshot mode, not measured
switch throughput. Actual JPEGs can be smaller, while wire overhead makes each
transmission slightly larger. The future continuous H.264 path requires its own
measured rate-control and multicast/unicast benchmark.

Ten launches in the same Windows session reached a responsive top-level window
in 191.8 ms median and 215.4 ms average. The minimum was 187.4 ms, the maximum
was 435.1 ms, and the first measured launch was 435.1 ms. This is application
launch latency with the OS already running; it is not a Windows reboot-to-ready
measurement.

For context, project-owner-supplied figures for other classroom-management
applications on the same Ryzen 7 5700X configuration are 11-12% average CPU
with peaks up to 20%, approximately 820 MB-1.1 GB RAM with peaks up to 1.3 GB,
30-40 Mbps while streaming, and about 32 seconds for boot and manager startup.
Those figures were not independently reproduced during this run, so differences
in workload and measurement method still prevent a strict apples-to-apples
comparison. The supplied systems were also described as removable by students.
NSTU's removal behavior was not tested in this resource benchmark; it remains
part of the administrator-policy, reboot, and Windows deployment validation
matrix.

### Reported i5-6400 follow-up

The project owner also reported an approximate NSTU run on the target Intel
Core i5-6400 system. These values did not include raw samples, duration, client
count, or workload details, so they are preliminary rather than definitive.

| Metric | NSTU on i5-6400 | Comparison and reduction |
| --- | ---: | --- |
| CPU | 4-12% | No same-i5 CPU baseline for the other application was supplied. Against its earlier 11-12% Ryzen average only as context, NSTU's 4% low end is 7-8 percentage points, or 63.6-66.7%, lower; the 12% high end overlaps the reference, so no guaranteed CPU reduction can be claimed. |
| RAM | Approximately similar to the NSTU Ryzen run | Results were similar across machines, but no definitive i5 memory figure was recorded. |
| Integrated GPU | 11% | The other application used 34% on the integrated GPU. NSTU was 23 percentage points lower, a 67.6% relative reduction, using about 32.4% of the comparison application's GPU load. |

### Reported client measurements at Vung Tau Junior High School

Teachers at Vung Tau Junior High School conducted and permitted the following
client-side checks on three devices with the same Intel Core i5-6400
configuration. These are approximate averages from three tests, not a
definitive production benchmark; the workload, capture scene, drivers, and
network can change the result.

| Client state | Approx. RAM | CPU | GPU / capture detail |
| --- | ---: | ---: | --- |
| Snapshot capture and running service | 44 MB | no more than 7% | 11% iGPU / 6% CPU in the paired GPU/CPU sample |
| Watching a server stream with overlay (1080p/60 fps) | 96 MB | 11% | 27% iGPU / 19% CPU |
| Drawing directly on the student machine | 72 MB | not recorded | Overlay drawing workload |
| Streaming the student screen to the server (720p/30 fps) | 65 MB | not recorded | 25% iGPU / 16% CPU |

The client results complement the server measurements above. They do not claim
that every i5-6400 machine or every classroom workload will match these values.

### Deep Freeze removal observation

The NSTU client removal test at Vung Tau indicates that normal Deep Freeze
administration is required. The machine must be booted Thawed, the Deep Freeze
console opened normally, protection disabled, and Windows restarted before the
client can be completely removed; the uninstaller then requires its own restart
to finish stopping the service and deleting files. Without opening Deep Freeze
and disabling protection, removal is effectively blocked. This remains an
observational field report, not a claim that every Deep Freeze edition behaves
identically.

### Cooperation and test sites

The following schools are cooperating with NSTU as project partners or are being
evaluated for cooperation. Additional test results are still being collected.

#### Verified partner (current)

<table>
  <tr>
    <td align="center" valign="top" width="180">
      <img src="docs/assets/partners/vung-tau-junior-high.png" alt="Vung Tau Junior High School logo" width="112"><br>
      <sub><b>Vung Tau Junior High School</b><br>Testing completed; teacher-conducted and permitted</sub>
    </td>
  </tr>
</table>

#### Pending verified partners

<table>
  <tr>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/vo-truong-toan-junior-high.png" alt="Vo Truong Toan Junior High School logo" width="112"><br><sub><b>Vo Truong Toan Junior High School</b><br>Cooperation agreed; testing pending</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/dinh-tien-hoang-high-school.png" alt="Dinh Tien Hoang High School logo" width="112"><br><sub><b>Dinh Tien Hoang High School</b><br>Cooperation agreed; testing pending</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/le-quy-don-gifted-high-school.png" alt="Le Quy Don High School for the Gifted logo" width="112"><br><sub><b>Le Quy Don High School for the Gifted</b><br>Cooperation agreed; testing pending</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ptnk-vnu-hcm.png" alt="VNU-HCM High School for the Gifted logo" width="112"><br><sub><b>VNU-HCM High School for the Gifted (PTNK)</b><br>Cooperation agreed; testing pending</sub></td>
    <td align="center" valign="top" width="180"><img src="docs/assets/partners/ben-cat-high-school.png" alt="Ben Cat High School logo" width="112"><br><sub><b>Ben Cat High School</b><br>Under review; partnership not confirmed</sub></td>
  </tr>
</table>

The logo files in `docs/assets/partners/` are background-cleaned copies of the
marks supplied for this report.

Use `tools/production/collect-benchmarks.ps1` for longer runs. Results with 5,
7, and 10 second snapshot intervals, real clients, network traffic, raw CSV,
and a documented workload on the target i5-6400 hardware are still required
before making production resource claims.

## Recommended network layout

```text
Teacher PC (NSTU Server)
          |
     Gigabit Ethernet
          |
Managed switch / router with IGMP snooping + one IGMP querier
     |            |             |
 Client 01     Client 02      Client 50+
```

Before a production deployment:

1. Put the server and clients on the same trusted VLAN or subnet for the first
   rollout.
2. Enable IGMP snooping on managed switches and ensure exactly one router or
   Layer-3 switch acts as the IGMP querier for that VLAN.
3. Do not expose NSTU control or video traffic directly to the internet.
4. Prefer wired Ethernet. If Wi-Fi is used for testing, disable access-point
   client isolation and confirm multicast is not rate-limited to legacy data
   rates.
5. Avoid unmanaged switches for a large room. Without IGMP snooping, multicast
   may be flooded to every port. If multicast is blocked, the planned unicast
   fallback increases server and switch bandwidth with every client.
6. Keep Windows Firewall enabled. TCP `47001` is the default authenticated
   control port and UDP `47000` is reserved for the video transport. Limit rules
   to the classroom VLAN and the required executable; do not create broad
   internet-facing rules.

Cross-VLAN multicast requires intentionally configured multicast routing. It
should not be enabled merely to make discovery work.

## Install the current test build

Nightly installers are available on the
[Releases page](https://github.com/Khoasoma/nstu/releases). They are unsigned
development artifacts and may trigger Microsoft Defender SmartScreen. Verify
the release origin and SHA-256 digest before running them:

```powershell
Get-FileHash .\nstu-server-*.exe -Algorithm SHA256
Get-FileHash .\nstu-client-*.exe -Algorithm SHA256
```

### Server

1. Download `nstu-server-<version>.exe` from the latest pre-release.
2. Run the installer and accept the Windows elevation prompt if requested.
3. Start:

   ```powershell
   & "$env:ProgramFiles\NSTU\server\nstu-server.exe"
   ```

The dashboard does not inject demonstration records. It starts with an empty
client registry and displays only records supplied by the runtime registry.
Teachers can switch between `Room screens`, which presents every visible client
in a responsive screen wall, and `Selected client`, which concentrates
telemetry, snapshot controls, screen annotation, lock/unlock, and chat for one
workstation. The snapshot interval is adjustable from 5 to 10 seconds. Each
JPEG is bounded to 60 KiB, and only the newest pending snapshot is retained to
avoid stale queue growth. Teacher-screen broadcast uses the same bounded
snapshot path. The dashboard includes English/Vietnamese and light/dark mode
switches. Minimizing or closing the window keeps the server available in the
Windows notification area; use the tray menu to reopen it or exit. Continuous
H.264/UDP preview remains optional future work.

Vietnamese and dark mode can also be selected at startup:

```powershell
& "$env:ProgramFiles\NSTU\server\nstu-server.exe" --language=vi --dark
```

### Client

1. Download and run `nstu-client-<version>.exe` as an administrator.
2. The installer registers `nstu-service` for automatic startup and configures
   service recovery. It deliberately does not start the service inside the
   installer session.
3. Restart Windows when setup requests it. On the next boot the service starts
   automatically and launches one `nstu-agent.exe` instance in the active user
   session on logon or unlock.
4. After restart, an administrator can verify the service with:

   ```powershell
   Get-Service nstu-service
   ```

Use Windows **Installed apps** or the NSTU uninstaller to remove the client.
The uninstaller stops the agent, deletes the service, removes package files,
and requires another restart. Direct manual service removal is not a supported
deployment workflow.

## Connecting a computer room

The current engineering enrollment flow is command-line based. Run the server
installer, configure a protected data root, and create a one-time enrollment
secret from an elevated PowerShell prompt:

```powershell
& "$env:ProgramFiles\NSTU\docs\deployment\configure-data-root.ps1" `
  -DataRoot "$env:ProgramData\NSTU"
& "$env:ProgramFiles\NSTU\docs\deployment\new-enrollment-secret.ps1" `
  -ExportPath "D:\SecureTransfer\nstu-enrollment.bin"
```

Restart `nstu-server.exe` so it loads the protected enrollment secret. On each
client, while the server is running, use a unique 128-bit identity and key ID:

```powershell
$clientId = [guid]::NewGuid().ToString("N")
& "$env:ProgramFiles\NSTU\client\nstu-provision.exe" `
  192.168.10.10 47001 $clientId 1 "D:\SecureTransfer\nstu-enrollment.bin"
```

The tool authenticates the enrollment transcript, derives the installed PSK
without sending it, and stores the client configuration with machine-scope
DPAPI. Delete every copy of the one-time export after enrollment, then restart
the service or Windows. A trusted client follows this path:

```text
Install client
  -> provision a unique client identity and protected enrollment credential
  -> authenticate to the server over TCP
  -> register the device and receive room policy
  -> receive an authenticated video-group configuration
  -> join multicast, measure loss, and use bounded unicast fallback if needed
```

Connection preambles only reject obviously invalid peers quickly. Device
identity is accepted only after the cryptographic handshake succeeds. A teacher
enrollment UI and the final multicast/group-key workflow remain release work.

## Deep Freeze deployments

- Install binaries in the normal protected Windows location.
- Reserve a thawed, ACL-restricted location for enrolled identity, protected
  key material, configuration, audit logs, and update state.
- Configure that location before enrollment, for example:

  ```powershell
  & "$env:ProgramFiles\NSTU\docs\deployment\configure-data-root.ps1" `
    -DataRoot "D:\NSTUData"
  ```
- Never store PSKs, certificates, dumps, screen captures, or runtime secrets in
  the repository.
- Do not freeze a production image before enrollment persistence, service
  recovery, upgrade, and rollback behavior have been tested.
- Boot the workstation Thawed and disable Deep Freeze protection before
  installing, upgrading, or uninstalling NSTU. The client uninstaller checks
  recognized `DFServ`/`DeepFrz` services and aborts while their protection is
  still active; this conservative check must be validated against the exact
  Deep Freeze edition used by the school.

Named pipes and memory-mapped files reduce temporary disk activity but do not
replace persistent protected storage. Deep Freeze will discard unthawed state
after a reboot.

## Security notes

- Current control sessions use mutual HMAC authentication and replay
  protection.
- JPEG snapshots and video datagrams are authenticated, but screen content is
  not encrypted. Do not test real sensitive screens on an untrusted LAN.
- Authenticode automation is available, but nightly artifacts remain unsigned;
  a production release requires the real certificate-backed workflow.
- Independent review, fuzzing, Deep Freeze edition validation, real certificate
  signing, and the hardware/network evidence matrix remain production blockers.
- A green CI build proves compilation and automated tests, not security or
  reliability on real school hardware.

## Contributing and development

Build instructions, repository structure, CMake options, testing, and fork/PR
steps are in the [development guide](docs/DEVELOPMENT.md). Contributions must
remain compatible with permissive licensing; GPL dependencies are not accepted.

## Contributors

- **Bùi Hồng Hải Đăng (`yanij`)**: project ideation, test-hardware support, and
  contributions to building NSTU.

## License

Project NSTU is released under the [MIT License](LICENSE). You may use, copy,
modify, publish, distribute, sublicense, and sell copies subject to the license
notice requirements. Third-party notices are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
