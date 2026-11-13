$ErrorActionPreference = 'Stop'
foreach ($name in @('cs-policy-functions.ps1', 'prepare-cs-policy.ps1', 'verify-cs-policy.ps1')) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Policy source must parse: $name" }
    foreach ($definition in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
        . ([scriptblock]::Create($definition.Extent.Text))
        if ($definition.Name -ceq 'Get-CsExistingPath') { $preparationPathAst = $definition }
    }
}
function Assert-CsRejected {
    param([scriptblock]$Action, [string]$Message)
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw $Message }
}
$sid = 'S-1-5-21-111-222-333-1001'
$hash = 'A' * 64
function New-CsFixtureRule {
    param([string]$Type)
    '<AppLockerPolicy Version="1"><RuleCollection Type="' + $Type + '" EnforcementMode="NotConfigured"><FileHashRule Id="' + [guid]::NewGuid().ToString() + '" Name="approved" Description="" UserOrGroupSid="' + $sid + '" Action="Allow"><Conditions><FileHashCondition><FileHash Type="SHA256" Data="0x' + $hash + '" SourceFileName="approved" SourceFileLength="1" /></FileHashCondition></Conditions></FileHashRule></RuleCollection></AppLockerPolicy>'
}
$xml = New-CsPolicyDocument -StudentSid $sid -GeneratedXml @((New-CsFixtureRule 'Exe'), (New-CsFixtureRule 'Dll'))
$digest = Get-CsPolicyDigest -PolicyXml $xml
if ($digest -notmatch '^[a-f0-9]{64}$') { throw 'Canonical policy digest must be SHA256.' }
$settings = [pscustomobject]@{ DeploymentMethod='GroupPolicy'; PolicyDigest=$digest; StudentSid=$sid; PhysicalQualified=1 }
$result = Test-CsPolicyReadiness -Settings $settings -ServiceStatus 'Running' -PolicyXml $xml
if ($result.Code -cne 'policy_verified') { throw 'Reviewed enforced fixture was not verified.' }
if ((Get-CsPolicyDigest ($xml.Replace('><', ">`r`n  <"))) -cne $digest) { throw 'Formatting must not change canonical digest.' }
$reordered = Read-CsPolicyXml $xml
foreach ($collection in @($reordered.DocumentElement.ChildNodes)) {
    foreach ($rule in @($collection.ChildNodes | Sort-Object { $_.GetAttribute('Id') } -Descending)) { [void]$collection.AppendChild($rule) }
}
foreach ($collection in @($reordered.DocumentElement.ChildNodes | Sort-Object { $_.GetAttribute('Type') } -Descending)) { [void]$reordered.DocumentElement.AppendChild($collection) }
if ((Get-CsPolicyDigest $reordered.OuterXml) -cne $digest) { throw 'GP rule ordering must not change canonical digest.' }
$packageRule = '<AppLockerPolicy Version="1"><RuleCollection Type="Appx" EnforcementMode="NotConfigured"><FilePublisherRule Id="' + [guid]::NewGuid().ToString() + '" Name="approved package" Description="" UserOrGroupSid="' + $sid + '" Action="Allow"><Conditions><FilePublisherCondition PublisherName="CN=Reviewed Publisher" ProductName="Reviewed.IME" BinaryName="*"><BinaryVersionRange LowSection="1.2.3.4" HighSection="1.2.3.4" /></FilePublisherCondition></Conditions></FilePublisherRule></RuleCollection></AppLockerPolicy>'
$packaged = New-CsPolicyDocument $sid @((New-CsFixtureRule 'Exe'), (New-CsFixtureRule 'Dll'), $packageRule)
$packageSettings = [pscustomobject]@{ DeploymentMethod='GroupPolicy'; PolicyDigest=(Get-CsPolicyDigest $packaged); StudentSid=$sid; PhysicalQualified=1 }
if ((Test-CsPolicyReadiness $packageSettings 'Running' $packaged).Code -cne 'policy_verified') { throw 'Exact approved package identity rejected.' }
foreach ($badPackage in @($packaged.Replace('PublisherName="CN=Reviewed Publisher"', 'PublisherName="*"'), $packaged.Replace('HighSection="1.2.3.4"', 'HighSection="*"'))) {
    if ((Test-CsPolicyReadiness $packageSettings 'Running' $badPackage).Code -cne 'policy_invalid') { throw 'Broad student packaged rule accepted.' }
}
foreach ($path in @('*', '%WINDIR%\*', '%PROGRAMFILES%\*', '%USERPROFILE%\*', 'C:\Users\*\*')) {
    $broad = Read-CsPolicyXml $xml
    $student = $broad.SelectSingleNode('/AppLockerPolicy/RuleCollection[@Type="Exe"]/FileHashRule')
    $rule = $broad.CreateElement('FilePathRule')
    foreach ($attribute in $student.Attributes) { $rule.SetAttribute($attribute.Name, $attribute.Value) }
    $rule.InnerXml = '<Conditions><FilePathCondition Path="' + [Security.SecurityElement]::Escape($path) + '" /></Conditions>'
    [void]$student.ParentNode.ReplaceChild($rule, $student)
    if ((Test-CsPolicyReadiness $settings 'Running' $broad.OuterXml).Code -cne 'policy_invalid') { throw 'Broad student executable path accepted.' }
}
foreach ($case in @(
    @{ Xml=$xml.Replace('EnforcementMode="Enforced"', 'EnforcementMode="AuditOnly"'); Code='policy_invalid' },
    @{ Xml=$xml.Replace($sid, 'S-1-1-0'); Code='policy_invalid' },
    @{ Xml=$xml.Replace('Data="0x' + $hash, 'Data="0x' + ('B' * 64)); Code='policy_digest_mismatch' },
    @{ Xml=$xml.Replace('Type="Dll"', 'Type="Exe"'); Code='policy_invalid' },
    @{ Xml=$xml.Replace('Path="*"', 'Path="%WINDIR%\\*"'); Code='policy_invalid' },
    @{ Xml='<!DOCTYPE p [<!ENTITY leak SYSTEM "file:///private">]>' + $xml; Code='policy_invalid' }
)) {
    $result = Test-CsPolicyReadiness $settings 'Running' $case.Xml
    if ($result.Code -cne $case.Code) { throw "Unsafe policy fixture accepted: $($case.Code)" }
}
if ((Test-CsPolicyReadiness $settings 'Stopped' $xml).Code -cne 'appid_not_running') { throw 'Stopped AppIdsvc must fail closed.' }
foreach ($qualification in @($null, 0, 2, '1', $true)) {
    $settings.PhysicalQualified = $qualification
    if ((Test-CsPolicyReadiness $settings 'Running' $xml).Code -cne 'physical_not_qualified') { throw 'Only explicit DWORD physical qualification may permit readiness.' }
}
$settings.PhysicalQualified = 1
$validDigest = $settings.PolicyDigest
$settings.PolicyDigest = @($digest, $digest)
if ((Test-CsPolicyReadiness $settings 'Running' $xml).Code -cne 'configuration_invalid') { throw 'Non-string digest configuration accepted.' }
$settings.PolicyDigest = $validDigest
$settings.DeploymentMethod = 'CSP'
if ((Test-CsPolicyReadiness $settings 'Running' $xml).Code -cne 'deployment_method_invalid') { throw 'CSP cannot be attested by GP cmdlets.' }
$settings.DeploymentMethod = 'GroupPolicy'; $settings.PolicyDigest = 'invalid'
if ((Test-CsPolicyReadiness $settings 'Running' $xml).Code -cne 'configuration_invalid') { throw 'Invalid digest must fail closed.' }
$settings.PolicyDigest = $digest; $settings.StudentSid = 'S-1-5-32-544'
if ((Test-CsPolicyReadiness $settings 'Running' $xml).Code -cne 'configuration_invalid') { throw 'Admin SID must not be a student.' }
if (Test-CsStudentSid 'S-1-5-21-9999999999-222-333-1001') { throw 'SID subauthority overflow must be rejected.' }
$acl = [pscustomobject]@{ Owner='S-1-5-18'; AreAccessRulesProtected=$true; Rules=@([pscustomobject]@{ Sid='S-1-5-32-544'; Allow=$true; Rights=2 }) }
if (-not (Test-CsTrustedAcl $acl 2 -RequireProtected)) { throw 'Protected administrator configuration rejected.' }
$acl.Rules += [pscustomobject]@{ Sid='S-1-1-0'; Allow=$true; Rights=2 }
if (Test-CsTrustedAcl $acl 2 -RequireProtected) { throw 'Student writable readiness configuration accepted.' }
$acl.Rules = @(); $acl.AreAccessRulesProtected = $false
if (Test-CsTrustedAcl $acl 2 -RequireProtected) { throw 'Inherited readiness configuration accepted.' }
$acl.AreAccessRulesProtected = $true
if (-not (Test-CsRegistryProtection @($acl,$acl,$acl,$acl))) { throw 'Trusted registry ancestry rejected.' }
$replaceable = [pscustomobject]@{ Owner='S-1-5-18'; AreAccessRulesProtected=$true; Rules=@([pscustomobject]@{ Sid='S-1-1-0'; Allow=$true; Rights=65536 }) }
if (Test-CsRegistryProtection @($acl,$acl,$replaceable,$acl)) { throw 'Replaceable registry ancestry accepted.' }
if (Test-CsRegistryProtection @($acl,$acl,$acl)) { throw 'Incomplete registry ancestry accepted.' }
$maskAssignment = @($preparationPathAst.Body.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$mask'
}, $true))[0]
# Evaluate only the enum mask expression; never invoke live path or ACL operations.
$ancestryMask = & ([scriptblock]::Create($maskAssignment.Right.Extent.Text))
if (Test-CsTrustedAcl $replaceable $ancestryMask) { throw 'Student DELETE on a preparation ancestor must be rejected.' }
Assert-CsRejected { New-CsPolicyDocument $sid @((New-CsFixtureRule 'Exe').Replace('FileHashRule', 'FilePathRule')) } 'Student path rules must be rejected.'
Assert-CsRejected { Get-CsApprovedInventory ([pscustomobject]@{ version=1; dependenciesReviewed=$false; files=@(); packages=@() }) } 'Unreviewed dependencies must stop preparation.'
$files = foreach ($role in @('nstu_agent','webview2','os','ime','accessibility','runtime')) {
    $name = if ($role -ceq 'nstu_agent') { 'nstu-agent.exe' } elseif ($role -ceq 'webview2') { 'msedgewebview2.exe' } else { $role + '.dll' }
    [pscustomobject]@{ path=('C:\\approved\\' + $name); contentSha256=$hash; role=$role }
}
$inventory = [pscustomobject]@{ version=1; dependenciesReviewed=$true; files=@($files); packages=@() }
if ((Get-CsApprovedInventory $inventory).files.Count -ne 6) { throw 'Explicit dependency inventory rejected.' }
$inventory.files = @($files | Where-Object { $_.role -cne 'ime' })
Assert-CsRejected { Get-CsApprovedInventory $inventory } 'Missing IME approval must stop preparation.'
$package = [pscustomobject]@{ packageFullName='Reviewed.IME_1.2.3.4_x64__reviewed'; publisher='CN=Reviewed Publisher'; name='Reviewed.IME'; version='1.2.3.4'; role='ime' }
$inventory.packages = @($package)
if ((Get-CsApprovedInventory $inventory).packages.Count -ne 1) { throw 'Explicit packaged IME inventory rejected.' }
$package.name = '*'
Assert-CsRejected { Get-CsApprovedInventory $inventory } 'Wildcard approved package identity must stop preparation.'
$package.name = 'Reviewed.IME'; $package.version = '1.2.3'
Assert-CsRejected { Get-CsApprovedInventory $inventory } 'Incomplete approved package version must stop preparation.'
$package.version = '1.2.3.4'; $inventory.packages = @($package, $package)
Assert-CsRejected { Get-CsApprovedInventory $inventory } 'Duplicate approved package identity must stop preparation.'
$inventory.packages = @()
$inventory.files = @($files); $inventory.files[0].path = 'C:\\approved\\*.exe'
Assert-CsRejected { Get-CsApprovedInventory $inventory } 'Wildcards must not select approved files.'
Write-Host 'CS POLICY TESTS: PASS (pure inventory, generated XML, canonical hash, physical qualification and registry ancestry fixtures; no enforcement exercised)'
