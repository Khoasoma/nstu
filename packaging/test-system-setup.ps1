param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Client", "Server")]
    [string]$Role,
    [Parameter(Mandatory = $true)]
    [string]$DataRoot,
    [int]$ControlPort = 47001,
    [int]$VideoPort = 47000,
    [string]$ExpectedServerBinary = ""
)

$ErrorActionPreference = "Stop"
$failures = [Collections.Generic.List[string]]::new()
$warnings = [Collections.Generic.List[string]]::new()

function Test-ExpectedServerProcess {
    param([uint32]$ProcessId)

    if ([string]::IsNullOrWhiteSpace($ExpectedServerBinary) -or
        $ProcessId -eq 0) {
        return $false
    }
    try {
        $expected = [IO.Path]::GetFullPath($ExpectedServerBinary)
        $process = Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId" `
            -ErrorAction Stop
        if ($null -eq $process -or
            [string]::IsNullOrWhiteSpace($process.ExecutablePath)) {
            return $false
        }
        $actual = [IO.Path]::GetFullPath($process.ExecutablePath)
        return [string]::Equals(
            $actual, $expected, [StringComparison]::OrdinalIgnoreCase)
    } catch {
        return $false
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $failures.Add("NSTU setup requires Administrator privileges.")
}
if (-not [Environment]::Is64BitOperatingSystem) {
    $failures.Add("NSTU requires 64-bit Windows 10 or Windows 11.")
}
$version = [Environment]::OSVersion.Version
if ($version.Major -lt 10) {
    $failures.Add("Unsupported Windows version: $version")
}
if ($ControlPort -lt 1 -or $ControlPort -gt 65535 -or
    $VideoPort -lt 1 -or $VideoPort -gt 65535) {
    $failures.Add("ControlPort and VideoPort must be valid TCP/UDP ports.")
}

$resolvedDataRoot = ""
try {
    $resolvedDataRoot = [IO.Path]::GetFullPath($DataRoot)
    $driveRoot = [IO.Path]::GetPathRoot($resolvedDataRoot)
    if ([string]::IsNullOrWhiteSpace($driveRoot) -or
        $resolvedDataRoot.TrimEnd('\') -eq $driveRoot.TrimEnd('\')) {
        throw "DataRoot must be an absolute subdirectory."
    }
    $drive = Get-Volume -DriveLetter $driveRoot.Substring(0, 1) -ErrorAction Stop
    if ($drive.FileSystem -notin @("NTFS", "ReFS")) {
        $failures.Add("NSTU DataRoot must use NTFS or ReFS for protected ACLs; found $($drive.FileSystem).")
    }
    New-Item -ItemType Directory -Path $resolvedDataRoot -Force | Out-Null
    $probe = Join-Path $resolvedDataRoot ".nstu-setup-$PID.tmp"
    [IO.File]::WriteAllText($probe, "NSTU setup probe")
    Remove-Item -LiteralPath $probe -Force
} catch {
    $failures.Add("NSTU DataRoot is not usable: $($_.Exception.Message)")
}

$firewallService = Get-Service -Name MpsSvc -ErrorAction SilentlyContinue
if ($null -eq $firewallService -or $firewallService.Status -ne "Running") {
    $failures.Add("Windows Defender Firewall service must be running.")
}
$disabledProfiles = @(Get-NetFirewallProfile -ErrorAction SilentlyContinue |
    Where-Object { -not $_.Enabled } |
    Select-Object -ExpandProperty Name)
if ($disabledProfiles.Count -gt 0) {
    $warnings.Add("Firewall profiles disabled: $($disabledProfiles -join ', ').")
}

if ($Role -eq "Server") {
    $tcpConflict = Get-NetTCPConnection -State Listen -LocalPort $ControlPort `
        -ErrorAction SilentlyContinue
    if ($null -ne $tcpConflict) {
        $unexpectedTcp = @($tcpConflict | Where-Object {
            -not (Test-ExpectedServerProcess -ProcessId $_.OwningProcess)
        })
        if ($unexpectedTcp.Count -gt 0) {
            $failures.Add("TCP control port $ControlPort is already in use by another application.")
        } else {
            $warnings.Add("The installed NSTU server is using TCP $ControlPort. Close it before launching the upgraded server.")
        }
    }
    $udpConflict = Get-NetUDPEndpoint -LocalPort $VideoPort `
        -ErrorAction SilentlyContinue
    if ($null -ne $udpConflict) {
        $unexpectedUdp = @($udpConflict | Where-Object {
            -not (Test-ExpectedServerProcess -ProcessId $_.OwningProcess)
        })
        if ($unexpectedUdp.Count -gt 0) {
            $failures.Add("UDP video port $VideoPort is already in use by another application.")
        } else {
            $warnings.Add("The installed NSTU server is using UDP $VideoPort. Close it before launching the upgraded server.")
        }
    }
    $allowRules = @(Get-NetFirewallRule -Enabled True -Direction Inbound `
        -Action Allow -ErrorAction SilentlyContinue | ForEach-Object {
            $rule = $_
            Get-NetFirewallPortFilter -AssociatedNetFirewallRule $rule `
                -ErrorAction SilentlyContinue | Where-Object {
                    ($_.Protocol -eq "TCP" -and $_.LocalPort -eq "$ControlPort") -or
                    ($_.Protocol -eq "UDP" -and $_.LocalPort -eq "$VideoPort")
                }
        })
    if ($allowRules.Count -eq 0) {
        $warnings.Add("No enabled inbound allow rule was found for TCP $ControlPort or UDP $VideoPort. Add rules scoped to the classroom VLAN before deployment.")
    }
}

$winlogon = Get-ItemProperty `
    -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" `
    -ErrorAction SilentlyContinue
if ($winlogon.AutoAdminLogon -eq "1") {
    $autoUser = "$($winlogon.DefaultUserName)"
    if ($autoUser -match '^(Administrator|.*\\Administrator)$') {
        $failures.Add("Automatic logon must not use the built-in Administrator account. Use a dedicated standard classroom account.")
    } else {
        $warnings.Add("Automatic logon is enabled for '$autoUser'. Confirm that it is a dedicated standard user with no local administrator membership.")
    }
} else {
    $warnings.Add("Automatic standard-user logon is not configured. This is optional; NSTU does not enable or store autologon credentials.")
}

Write-Host "NSTU setup role: $Role"
Write-Host "NSTU data root: $resolvedDataRoot"
$warnings | ForEach-Object { Write-Warning $_ }
if ($failures.Count -ne 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Host "NSTU system setup checks passed."
