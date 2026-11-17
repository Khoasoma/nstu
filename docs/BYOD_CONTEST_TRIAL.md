# Project nstu: local contest trial

Status: **INDEV**. This Windows trial provides an ordinary student window, explicit SAS-confirmed room joining, public package transfer, durable request handling and optional aggregate standings. It is not qualified for official examinations.

Build with WebView2 enabled. The teacher executable has a dedicated `--contests` mode; the student executable is `nstu-student.exe`. Neither trial folder installs a service, enrolls a managed computer, adds a firewall rule or configures automatic startup. A missing Linux judge must remain unavailable and block judged Begin.

Stage a new directory with PowerShell 7:

```powershell
./packaging/stage-contest-trial.ps1 -BuildRoot C:/path/to/build `
  -Destination C:/path/to/new-trial -WebView2Loader C:/path/to/WebView2Loader.dll `
  -PowerShellRoot C:/path/to/powershell7 -CompilerRuntimeRoot C:/msys64/ucrt64/bin
```

The last argument supplies the MinGW runtime DLLs for the local GCC build. Use `-Configuration Release` for an MSVC multi-configuration build; that distribution still needs separate compiler/runtime qualification. Staging copies one shared PowerShell runtime, the public extractor helper, native executables and loader. It excludes PowerShell profiles, examination archives, grading bundles and credentials. Keep the complete trial directory together. Run `test-contest-trial.ps1 -TrialRoot C:/path/to/new-trial` to check its hash inventory; this checks files, not judging or device safety.

Before launching, configure `NSTU_CONTEST_POWERSHELL_PATH` to the absolute staged `runtime/powershell/pwsh.exe` path and `NSTU_CONTEST_POWERSHELL_SHA256` to that executable's SHA256. These are trusted local deployment settings. Use the adjacent `stage-contest-package.ps1` helper and its recorded hash. Do not select runtime/helper paths from discovery replies or assessment pages. WebView2 Runtime must already be available on the computer.

The teacher's existing operator broker uses `NSTU_CS_BROKER_URL` and `NSTU_CS_BROKER_TOKEN`. Provision a fresh teacher owner ID and separate protected trial profile. Register the exact Studio public package/content pin and separate private grading bundle. Private grading stays on the operator/broker side. The public archive is validated again before serving. Start the teacher with `nstu-server.exe --contests`, then use the Contests workspace to open the room and compare each student's SAS before approval.

Students scan the local network, select a room, compare the SAS with the teacher and explicitly confirm. Discovery labels and teacher ID are untrusted hints until this confirmation. Manual IPv4:port discovery is available when broadcasts cannot cross the network. Wi-Fi client isolation can prevent both approaches. Joining ends at Begin; existing members can reconnect only to their original tuple. Closed rooms permit recovery and receipts, with no new Run or readiness authority.

Use fresh synthetic packages and two independent ordinary-user student profiles for the first peer rehearsal. Check rejection, actual public archive/native manifest verification and final preparation ACK, disconnect/reconnect, source preservation and teacher restart. Last-known standings must show pending/unavailable status honestly, and `teacher_only` rooms must never publish them. Do not substitute a fixture judge for actual execution. Real C++20/Python3 runs and private verdict/leaderboard acceptance require a separately qualified Linux Isolate judgehost.

Then run the teacher and two or three laptops on one peer-accessible WLAN for a twenty-minute synthetic round. Physical Vietnamese IME, keyboard focus, resizing/closing, storage failures, profile separation and restart need actual evidence. Unit tests and loopback fixtures do not establish these capabilities.

Keep source/recovery profiles until all work has been exported or reconciled. Closing the app does not delete drafts. To remove the portable trial, close its processes and delete only its explicitly chosen trial directory; profiles are separate and should be removed only after their contents are no longer needed. No system uninstall is necessary for this uninstalled trial.
