param(
    [string]$InstallRoot = "$PSScriptRoot",
    [string]$DataRoot = (Join-Path $env:ProgramData "NSTU")
)

$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
$serviceBinary = Join-Path $InstallRoot "nstu-service.exe"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "NSTU client installation requires Administrator privileges."
}

if (-not (Test-Path -LiteralPath $serviceBinary)) {
    throw "NSTU service binary was not found: $serviceBinary"
}

& (Join-Path $PSScriptRoot "configure-data-root.ps1") -DataRoot $DataRoot

$escapedBinary = '"' + $serviceBinary.Replace('"', '\"') + '"'
$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($null -ne $existing) {
    & sc.exe stop $serviceName | Out-Null
    & sc.exe delete $serviceName | Out-Null
    Start-Sleep -Milliseconds 500
}

try {
    & sc.exe create $serviceName binPath= $escapedBinary start= auto DisplayName= "NSTU Client Service" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe create failed with exit code $LASTEXITCODE"
    }
    & sc.exe description $serviceName "NSTU classroom client service" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe description failed with exit code $LASTEXITCODE"
    }
    & sc.exe failure $serviceName reset= 86400 actions= restart/5000/restart/15000/restart/60000 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe failure configuration failed with exit code $LASTEXITCODE"
    }
    & sc.exe failureflag $serviceName 1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe failureflag configuration failed with exit code $LASTEXITCODE"
    }
    $serviceSddl = "D:P(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)" +
        "(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)" +
        "(A;;LCLORC;;;AU)"
    & sc.exe sdset $serviceName $serviceSddl | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe sdset failed with exit code $LASTEXITCODE"
    }
} catch {
    & sc.exe delete $serviceName | Out-Null
    throw
}

Write-Host "NSTU client service registered. Restart Windows to activate it."
