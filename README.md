# Project NSTU

[English](README.md) | [Tiếng Việt](README.vi.md) | [Development guide](docs/DEVELOPMENT.md)

[![Windows CI](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

NSTU is a free and open-source classroom and computer-lab management project
for Windows. It is designed around a centralized teacher server, lightweight
student clients, authenticated control commands, chat, and low-latency screen
broadcasting over a local network.

> **Development status:** NSTU is currently an engineering MVP, not a
> production release. Persisted enrollment, multi-client dispatch, authenticated
> control routing, packetization, and device recovery are implemented and tested.
> Decoded end-to-end live-screen delivery and the physical production validation
> matrix are not complete. Do not deploy the current nightly build as a security
> control in a real school.

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
- Broadcast a teacher screen using H.264 and UDP multicast so LAN bandwidth
  does not grow linearly with the client count.
- Fall back to unicast when multicast health is persistently poor.
- Show a responsive wall of all client screens with an adjustable 5-15 FPS
  refresh target, plus focused telemetry, controls, and chat for one client.
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
telemetry, stream controls, and chat for one workstation. The room-screen
refresh target is adjustable from 5 to 15 FPS; 5 FPS is the prudent starting
   point for a 50-client room until full decoder and network soak testing is
   complete. The live control plane is connected; decoded-frame routing is still
   being integrated.

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
- Video datagrams can be authenticated, but screen content is not yet
  encrypted. Do not test real sensitive screens on an untrusted LAN.
- Authenticode automation is available, but nightly artifacts remain unsigned;
  a production release requires the real certificate-backed workflow.
- Independent review, fuzzing, decoded live-screen integration, Deep Freeze
  edition validation, and the hardware/network evidence matrix remain production
  blockers.
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
