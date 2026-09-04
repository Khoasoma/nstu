$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
$deepFreezeServiceNames = @("DFServ", "DeepFrz")
$installRoot = [IO.Path]::GetFullPath($PSScriptRoot)

if (-not ("NstuNativeMethods" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NstuNativeMethods {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileEx(string existingFileName,
        string newFileName, int flags);
}
"@
}

function Stop-NstuProcesses {
    param([string[]]$Names)

    foreach ($name in $Names) {
        $processes = @(Get-CimInstance Win32_Process -Filter "Name='$name.exe'" `
            -ErrorAction SilentlyContinue)
        foreach ($process in $processes) {
            $path = $process.ExecutablePath
            if ([string]::IsNullOrWhiteSpace($path) -or
                [string]::Equals([IO.Path]::GetFullPath($path),
                    (Join-Path $installRoot "$name.exe"),
                    [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
            }
        }
    }
    Start-Sleep -Milliseconds 250
}

function Schedule-DeleteAtReboot {
    param([string]$Path)

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        if (-not [NstuNativeMethods]::MoveFileEx($Path, $null, 4)) {
            Write-Warning "Could not schedule deletion at reboot: $Path"
        }
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "NSTU client uninstallation requires Administrator privileges."
}

$activeDeepFreezeServices = @(
    Get-Service -Name $deepFreezeServiceNames -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped -or
            $_.StartType -ne [System.ServiceProcess.ServiceStartMode]::Disabled
        }
)
if ($activeDeepFreezeServices.Count -gt 0) {
    $detected = ($activeDeepFreezeServices | Select-Object -ExpandProperty Name) -join ", "
    throw "Deep Freeze protection appears active ($detected). Boot the computer Thawed, disable Deep Freeze protection, restart Windows, and then run the NSTU uninstaller again."
}

Stop-NstuProcesses @("nstu-agent", "nstu-service")

$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($null -ne $existing) {
    & sc.exe stop $serviceName | Out-Null
    try {
        $existing.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Stopped,
            [TimeSpan]::FromSeconds(15))
    } catch {
        Write-Warning "NSTU service did not stop within 15 seconds; removal will complete after restart."
    }
    Stop-NstuProcesses @("nstu-service")
    & sc.exe delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete failed with exit code $LASTEXITCODE"
    }
}

$pendingFiles = [Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $installRoot -File -Recurse -ErrorAction SilentlyContinue |
    ForEach-Object {
        try {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop
        } catch {
            Schedule-DeleteAtReboot $_.FullName
            $pendingFiles.Add($_.FullName)
        }
    }

if ($pendingFiles.Count -gt 0) {
    Write-Host "NSTU files are locked and have been scheduled for deletion at the next Windows restart."
} else {
    Write-Host "NSTU client files and service removed. Restart Windows to finalize removal."
}
