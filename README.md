# Project NSTU

[English](README.md) | [Tiếng Việt](README.vi.md) | [Development guide](docs/DEVELOPMENT.md)

[![Windows CI](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/Khoasoma/nstu/actions/workflows/windows.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

NSTU is a free and open-source classroom and computer-lab management project
for Windows. It is designed around a centralized teacher server, lightweight
student clients, authenticated control commands, chat, and low-latency screen
broadcasting over a local network.

> **Development status:** NSTU is currently an engineering MVP, not a
> production release. The installers can be built and the user interfaces can
> be tested, but live enrollment, multi-client dispatch, control routing, and
> end-to-end video delivery are not complete. Do not deploy the current nightly
> build as a security control in a real school.

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
- Show client status, latency, packet loss, a 15 FPS preview, and chat in the
  server application.
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
6. Keep Windows Firewall enabled. The current MVP does not yet define stable,
   user-configurable production ports, so do not create broad permanent allow
   rules based on guessed port numbers.

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

The current dashboard displays demonstration clients. `Start stream`, `Lock`,
`Request keyframe`, and chat are UI states only until the live control plane is
connected.

### Client

1. Download and install `nstu-client-<version>.exe` as an administrator.
2. Open an elevated PowerShell window and run:

   ```powershell
   Set-Location "$env:ProgramFiles\NSTU\client"
   .\install-client-service.ps1
   Get-Service nstu-service
   ```

3. The service attempts to start `nstu-agent.exe` in the active desktop
   session. The tray icon and local chat window can be used to inspect the
   client UI.
4. Before uninstalling the client, remove the service:

   ```powershell
   Set-Location "$env:ProgramFiles\NSTU\client"
   .\uninstall-client-service.ps1
   ```

## Connecting a computer room

There is no honest end-user connection procedure for the current nightly:
persisted enrollment and the live multi-client dispatcher are still roadmap
items. A finished production flow will require all of the following before a
client is shown as trusted:

```text
Install client
  -> provision a unique client identity and protected enrollment credential
  -> authenticate to the server over TCP
  -> register the device and receive room policy
  -> receive an authenticated video-group configuration
  -> join multicast, measure loss, and use bounded unicast fallback if needed
```

Connection preambles only reject obviously invalid peers quickly. Device
identity is accepted only after the cryptographic handshake succeeds. The
project will publish exact ports, multicast group policy, firewall rules, and
the enrollment UI when those interfaces become stable.

## Deep Freeze deployments

- Install binaries in the normal protected Windows location.
- Reserve a thawed, ACL-restricted location for enrolled identity, protected
  key material, configuration, audit logs, and update state.
- Never store PSKs, certificates, dumps, screen captures, or runtime secrets in
  the repository.
- Do not freeze a production image before enrollment persistence, service
  recovery, upgrade, and rollback behavior have been tested.

Named pipes and memory-mapped files reduce temporary disk activity but do not
replace persistent protected storage. Deep Freeze will discard unthawed state
after a reboot.

## Security notes

- Current control sessions use mutual HMAC authentication and replay
  protection.
- Video datagrams can be authenticated, but screen content is not yet
  encrypted. Do not test real sensitive screens on an untrusted LAN.
- Code signing, authenticated enrollment transport, persisted keyrings,
  device-loss recovery, independent review, and fuzzing remain production
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
