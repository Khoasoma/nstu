# Fixed installed script: the SYSTEM service must supply no candidate arguments.
param()
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'; $InformationPreference = 'SilentlyContinue'; $ProgressPreference = 'SilentlyContinue'
try {
    if ([IntPtr]::Size -ne 8) { throw '64-bit Windows PowerShell is required.' }
    . (Join-Path $PSScriptRoot 'cs-policy-functions.ps1')
    $key = 'HKLM:\SOFTWARE\NSTU\ComputerScience'
    $acls = @(foreach ($path in @('HKLM:\', 'HKLM:\SOFTWARE', 'HKLM:\SOFTWARE\NSTU', $key)) {
        ConvertTo-CsAclRecord (Get-Acl -LiteralPath $path -ErrorAction Stop)
    })
    if (-not (Test-CsRegistryProtection $acls)) {
        $result = [pscustomobject]@{ version=1; status='unavailable'; code='configuration_unprotected' }
    } else {
        $settings = Get-ItemProperty -LiteralPath $key -Name DeploymentMethod,PolicyDigest,StudentSid,PhysicalQualified -ErrorAction Stop
        $result = Test-CsPolicyReadiness $settings '' ''
        if ($result.code -ceq 'appid_not_running') {
            Import-Module AppLocker -ErrorAction Stop
            $service = Get-Service -Name AppIdsvc -ErrorAction Stop
            $policy = if ($service.Status.ToString() -ceq 'Running') { [string](Get-AppLockerPolicy -Effective -Xml -ErrorAction Stop) } else { '' }
            $result = Test-CsPolicyReadiness $settings $service.Status.ToString() $policy
        }
    }
} catch { $result = [pscustomobject]@{ version=1; status='unavailable'; code='probe_unavailable' } }
Write-Output ($result | ConvertTo-Json -Compress)
if ($result.code -ceq 'policy_verified') { exit 0 }
exit 1
