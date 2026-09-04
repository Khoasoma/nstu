# NSTU Setup Guide

[English README](../README.md) | [Tiếng Việt](../README.vi.md)

This guide separates three different things that are often confused:

1. The NSTU installers, which install binaries and invoke their own lifecycle
   scripts.
2. `nstu-setup.exe`, the interactive administrator bootstrapper.
3. PowerShell deployment scripts, which are shipped as files and can be run
   manually for enrollment or validation.

## What each download contains

| Download | `nstu-setup.exe` | Deployment scripts | Automatic action |
| --- | --- | --- | --- |
| Full server installer | Yes, under `setup\\` | Yes, under `docs\\deployment\\` | Validates the server and configures the protected data root |
| Full client installer | No | Yes; client lifecycle helpers under `client\\`, shared scripts under `docs\\deployment\\` | Registers `nstu-service` and requests a reboot |
| Standalone `nstu-server.exe` | No | No | Nothing; start it manually |
| Standalone client EXEs | No | No | Nothing; use the full client installer for service registration |

The scripts are therefore available after installing the complete package. A
GitHub release asset that is only an EXE is not a complete package and cannot
be used as the source for the commands in the enrollment guide. A source
checkout is an alternative; its scripts are in `packaging\\`.

## `nstu-setup.exe` interface

`nstu-setup.exe` is a manual, elevated administrator tool. It does not run as a
service, does not start the client service, and does not replace the NSIS
installer. With no arguments it opens a role selector:

```powershell
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe"
```

Choose one of `Client`, `Server`, or `Both` for prerequisite auditing. `Both`
combines the audit panels; it does not install both product roles on one
machine. The same choice can be supplied for a controlled launch:

```powershell
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" --target=server
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" --target=client
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" --target=both
```

`--graphics-debug` requests the Direct3D 11 debug layer and records whether
the layer was available. It can be combined with a target, for example:

```powershell
& "$env:ProgramFiles\\NSTU\\setup\\nstu-setup.exe" `
  --target=server --graphics-debug
```

The setup window provides:

- Role-specific prerequisite panels. Server checks show display and hardware
  H.264 encoder readiness; client checks show network/display-session context.
- Network adapter names, operational state, and link speed.
- Policy audit for Task Manager, Command Prompt, Control Panel, and Drive C:
  visibility.
- An opt-in IPv4 website allowlist panel. Applying it changes WFP policy and
  must be done only by an administrator who intends to enforce that policy.
- A `Diagnostics` popup with DXGI adapter/vendor names, D3D feature level,
  Desktop Duplication status, hardware/WARP device mode, and bounded recent
  HRESULT events.

The setup tool does not silently apply lockdown or WFP rules. Those operations
require an explicit button press. It also does not configure autologon,
install Deep Freeze, or enroll a client by itself.

## Installer and script relationship

The installer invokes lifecycle scripts automatically:

- The server package runs `test-system-setup.ps1` and
  `configure-data-root.ps1` during installation, after a role-conflict check
  confirms that no NSTU client is installed.
- The client package runs `client\\install-client-service.ps1`; that helper
  is guarded by the same role-conflict check, invokes `test-system-setup.ps1`
  and `configure-data-root.ps1`, registers the service as `start= auto`, and
  requests a reboot.
- The client uninstaller runs `client\\uninstall-client-service.ps1`, which
  force-terminates NSTU-owned agent/service processes, stops/removes the
  service, and requires a reboot. Any package file that is still locked is
  registered with Windows `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` for
  deletion during the next boot. It fails closed if the recognized Deep Freeze
  services indicate that protection is still active.

The server uninstaller runs `docs\\deployment\\uninstall-server.ps1`,
force-terminates the installed `nstu-server.exe`, removes the server/setup
files, schedules locked files for next-boot deletion, and sets the NSIS reboot
flag.

The enrollment secret is a separate administrator operation. After the server
installer has completed, run `new-enrollment-secret.ps1` from
`docs\\deployment\\` on the server, then use the installed
`client\\nstu-provision.exe` on each client. See the
[computer-room enrollment section](../README.md#connecting-a-computer-room)
for the exact commands.

## Build versus runtime flags

`NSTU_BUILD_SETUP=ON` is a CMake configure option that decides whether the
`nstu-setup` executable is built. It is not a command-line argument to the
finished executable. There is currently no `--setup` runtime flag. The runtime
flags are `--target=client|server|both` and `--graphics-debug`.

To build the tool from source:

```powershell
cmake -S . -B build-setup -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release -DNSTU_BUILD_SETUP=ON `
  -DNSTU_BUILD_CLIENT=OFF -DNSTU_BUILD_SERVER=OFF `
  -DNSTU_BUILD_VIDEO=OFF -DNSTU_BUILD_TESTS=OFF
cmake --build build-setup --target nstu-setup
```
