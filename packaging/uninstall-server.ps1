$ErrorActionPreference = "Stop"
$installRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))

if (-not ("NstuNativeMethods" -as [type])) {
    Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class NstuNativeMethods {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileEx(string existingFileName,
        string newFileName, int flags);
}
"@
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "NSTU server uninstallation requires Administrator privileges."
}

$serverBinary = [IO.Path]::GetFullPath((Join-Path $installRoot "server\nstu-server.exe"))
$processes = @(Get-CimInstance Win32_Process -Filter "Name='nstu-server.exe'" `
    -ErrorAction SilentlyContinue)
foreach ($process in $processes) {
    if ([string]::IsNullOrWhiteSpace($process.ExecutablePath) -or
        [string]::Equals([IO.Path]::GetFullPath($process.ExecutablePath),
            $serverBinary, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Milliseconds 250

$pendingFiles = [Collections.Generic.List[string]]::new()
foreach ($directory in @("server", "setup")) {
    $path = Join-Path $installRoot $directory
    Get-ChildItem -LiteralPath $path -File -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object {
            try {
                Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop
            } catch {
                if (-not [NstuNativeMethods]::MoveFileEx($_.FullName, $null, 4)) {
                    Write-Warning "Could not schedule deletion at reboot: $($_.FullName)"
                }
                $pendingFiles.Add($_.FullName)
            }
        }
}

if ($pendingFiles.Count -gt 0) {
    Write-Host "NSTU server files are locked and scheduled for deletion at the next Windows restart."
} else {
    Write-Host "NSTU server process and package files removed. Restart Windows to finalize removal."
}
