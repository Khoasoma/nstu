[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
    Write-Host "PASS: $Message"
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$templatePath = Join-Path $scriptDir "nstu-installer.nsi.in"

Assert-Condition (Test-Path -LiteralPath $templatePath) `
    "Installer template exists at $templatePath"

$content = Get-Content -LiteralPath $templatePath -Raw

# 1. TCP Inbound Rule Verification
Assert-Condition ($content -match 'netsh\.exe advfirewall firewall add rule name="NSTU Server \(TCP-In\)"') `
    "Installer provisions 'NSTU Server (TCP-In)' rule"

Assert-Condition ($content -match 'name="NSTU Server \(TCP-In\)" dir=in action=allow program="\$INSTDIR\\server\\nstu-server\.exe" protocol=TCP localport=47001 profile=private,domain enable=yes') `
    "TCP rule matches exact least-privilege specification (port 47001, private/domain profiles, scoped to nstu-server.exe)"

# 2. UDP Discovery Inbound Rule Verification
Assert-Condition ($content -match 'netsh\.exe advfirewall firewall add rule name="NSTU Server Discovery \(UDP-In\)"') `
    "Installer provisions 'NSTU Server Discovery (UDP-In)' rule"

Assert-Condition ($content -match 'name="NSTU Server Discovery \(UDP-In\)" dir=in action=allow program="\$INSTDIR\\server\\nstu-server\.exe" protocol=UDP localport=47001 profile=private,domain enable=yes') `
    "UDP rule matches exact least-privilege specification (port 47001, private/domain profiles, scoped to nstu-server.exe)"

# 3. Least-Privilege Profile Check: 'public' profile must NEVER be granted
Assert-Condition ($content -notmatch 'name="NSTU Server.*profile=.*public') `
    "Public profile is never permitted for inbound firewall rules"

# 4. Rollback Cleanup Verification
Assert-Condition ($content -match 'nstu_server_abort:[\s\S]*?delete rule name="NSTU Server \(TCP-In\)"[\s\S]*?delete rule name="NSTU Server Discovery \(UDP-In\)"') `
    "Server install abort/rollback cleans up TCP and UDP firewall rules"

# 5. Uninstall Cleanup Verification
Assert-Condition ($content -match 'nstu_uninstall_server:[\s\S]*?delete rule name="NSTU Server \(TCP-In\)"[\s\S]*?delete rule name="NSTU Server Discovery \(UDP-In\)"') `
    "Server uninstaller deletes TCP and UDP firewall rules"

Write-Host "All installer firewall provisioning assertions passed successfully."

