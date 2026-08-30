param(
    [string]$InstallRoot = "$PSScriptRoot"
)

$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
$serviceBinary = Join-Path $InstallRoot "nstu-service.exe"

if (-not (Test-Path -LiteralPath $serviceBinary)) {
    throw "NSTU service binary was not found: $serviceBinary"
}

$escapedBinary = '"' + $serviceBinary.Replace('"', '\"') + '"'
$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($null -ne $existing) {
    & sc.exe stop $serviceName | Out-Null
    & sc.exe delete $serviceName | Out-Null
}

& sc.exe create $serviceName binPath= $escapedBinary start= auto DisplayName= "NSTU Client Service" | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "sc.exe create failed with exit code $LASTEXITCODE"
}
& sc.exe description $serviceName "NSTU classroom client service" | Out-Null
& sc.exe start $serviceName | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "sc.exe start failed with exit code $LASTEXITCODE"
}

Write-Host "NSTU client service installed and started."
