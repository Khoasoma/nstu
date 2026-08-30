$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
$deepFreezeServiceNames = @("DFServ", "DeepFrz")

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

$agentProcesses = Get-Process -Name "nstu-agent" -ErrorAction SilentlyContinue
if ($null -ne $agentProcesses) {
    $agentProcesses | Stop-Process -Force
}

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
    & sc.exe delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete failed with exit code $LASTEXITCODE"
    }
}

Write-Host "NSTU client service removed. Restart Windows to complete uninstallation."
