param(
    [switch]$KeepFiles
)

$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($null -ne $existing) {
    & sc.exe stop $serviceName | Out-Null
    & sc.exe delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete failed with exit code $LASTEXITCODE"
    }
}

if (-not $KeepFiles) {
    Write-Host "Service removed. Installer files can now be uninstalled from Apps."
} else {
    Write-Host "Service removed; client files were retained."
}
