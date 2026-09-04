param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Client", "Server")]
    [string]$Role,
    [string]$InstallRoot = "$PSScriptRoot\..\.."
)

$ErrorActionPreference = "Stop"
$resolvedRoot = [IO.Path]::GetFullPath($InstallRoot)
$service = Get-Service -Name "nstu-service" -ErrorAction SilentlyContinue
$clientBinary = Join-Path $resolvedRoot "client\nstu-service.exe"
$agentBinary = Join-Path $resolvedRoot "client\nstu-agent.exe"
$serverBinary = Join-Path $resolvedRoot "server\nstu-server.exe"

if ($Role -eq "Server") {
    if ($null -ne $service -or (Test-Path -LiteralPath $clientBinary) -or
        (Test-Path -LiteralPath $agentBinary)) {
        throw "NSTU Server cannot be installed while an NSTU Client is present. Uninstall the client and restart Windows first."
    }
} else {
    if (Test-Path -LiteralPath $serverBinary) {
        throw "NSTU Client cannot be installed while an NSTU Server is present. Uninstall the server first."
    }
}

Write-Host "NSTU $Role installation role check passed."
