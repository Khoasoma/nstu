[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StudentSid,
    [Parameter(Mandatory = $true)][string]$ApprovedInventoryPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cs-policy-functions.ps1')

function Get-CsApprovedInventory {
    param($Inventory)
    if ($null -eq $Inventory -or $Inventory.version -isnot [int] -or $Inventory.version -ne 1 -or $Inventory.dependenciesReviewed -isnot [bool] -or
        -not $Inventory.dependenciesReviewed -or $Inventory.files -isnot [array] -or $Inventory.packages -isnot [array] -or
        $Inventory.files.Count -lt 1 -or $Inventory.files.Count -gt 4096 -or $Inventory.packages.Count -gt 128) { throw 'A complete reviewed dependency inventory is required.' }
    foreach ($name in $Inventory.PSObject.Properties.Name) {
        if ($name -cnotin @('version','dependenciesReviewed','files','packages')) { throw 'Unknown inventory field.' }
    }
    $roles = @('nstu_agent','webview2','os','ime','accessibility','runtime')
    $seenRoles = @{}; $seenPaths = @{}; $seenPackages = @{}
    foreach ($file in $Inventory.files) {
        if (@($file.PSObject.Properties).Count -ne 3 -or $file.path -isnot [string] -or
            $file.path -cnotmatch '^[A-Za-z]:\\' -or $file.path -match '[*?\[\]]' -or $file.path.Substring(2).Contains(':') -or
            $file.contentSha256 -cnotmatch '^[A-Fa-f0-9]{64}$' -or $file.role -cnotin $roles -or $seenPaths.ContainsKey($file.path)) { throw 'Invalid approved file.' }
        foreach ($name in $file.PSObject.Properties.Name) { if ($name -cnotin @('path','contentSha256','role')) { throw 'Unknown file field.' } }
        if ([IO.Path]::GetExtension($file.path).ToLowerInvariant() -cnotin @('.exe','.com','.dll','.ocx','.ps1','.bat','.cmd','.vbs','.js','.msi','.msp','.mst')) { throw 'Unsupported approved file type.' }
        if (($file.role -ceq 'nstu_agent' -and [IO.Path]::GetFileName($file.path) -ine 'nstu-agent.exe') -or
            ($file.role -ceq 'webview2' -and [IO.Path]::GetFileName($file.path) -ine 'msedgewebview2.exe')) { throw 'Required executable identity missing.' }
        $seenPaths[$file.path] = $true; $seenRoles[$file.role] = $true
    }
    foreach ($package in $Inventory.packages) {
        if (@($package.PSObject.Properties).Count -ne 5 -or $package.role -cnotin $roles -or $seenPackages.ContainsKey($package.packageFullName)) { throw 'Invalid approved package.' }
        foreach ($name in @('packageFullName','publisher','name','version')) {
            if ($package.$name -isnot [string] -or [string]::IsNullOrWhiteSpace($package.$name) -or $package.$name -match '[*?\r\n]') { throw 'Packaged identity must be explicit.' }
        }
        foreach ($name in $package.PSObject.Properties.Name) { if ($name -cnotin @('packageFullName','publisher','name','version','role')) { throw 'Unknown package field.' } }
        if ($package.version -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' -or $package.role -cin @('nstu_agent','webview2')) { throw 'Invalid approved packaged version or role.' }
        $seenPackages[$package.packageFullName] = $true; $seenRoles[$package.role] = $true
    }
    foreach ($role in $roles) { if (-not $seenRoles.ContainsKey($role)) { throw "Missing approved dependency role: $role" } }
    return $Inventory
}

function Get-CsExistingPath {
    param([string]$Path, [switch]$Directory, [switch]$RequireTrusted)
    if ($Path -cnotmatch '^[A-Za-z]:\\' -or $Path -match '[*?\[\]]' -or $Path.Substring(2).Contains(':')) { throw 'An explicit local absolute path is required.' }
    $full = [IO.Path]::GetFullPath($Path)
    $current = [IO.Path]::GetPathRoot($full)
    foreach ($part in @('') + @($full.Substring($current.Length).Split('\'))) {
        if ($part.Length) { $current = Join-Path $current $part }
        $attributes = [IO.File]::GetAttributes($current)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse paths are not permitted.' }
        if ($RequireTrusted) {
            $acl = ConvertTo-CsAclRecord (Get-Acl -LiteralPath $current -ErrorAction Stop)
            $mask = [int]([Security.AccessControl.FileSystemRights]::Delete -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership)
            if ($current -ieq $full) { $mask = [int]([Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor $mask) }
            if (-not (Test-CsTrustedAcl $acl $mask)) { throw 'Preparation path and ancestors must be controlled by trusted Windows administrators.' }
        }
    }
    if (($Directory -and -not [IO.Directory]::Exists($full)) -or (-not $Directory -and -not [IO.File]::Exists($full))) { throw 'Required path does not exist.' }
    return $full
}

function New-CsPolicyDocument {
    param([string]$StudentSid, [string[]]$GeneratedXml)
    if (-not (Test-CsStudentSid $StudentSid)) { throw 'An individual standard-user exam SID is required.' }
    $document = Read-CsPolicyXml '<AppLockerPolicy Version="1" />'
    foreach ($type in @('Exe','Dll','Script','Msi','Appx')) {
        $collection = $document.CreateElement('RuleCollection')
        $collection.SetAttribute('Type', $type); $collection.SetAttribute('EnforcementMode', 'Enforced')
        [void]$document.DocumentElement.AppendChild($collection)
        foreach ($sid in @('S-1-5-18','S-1-5-32-544')) {
            $kind = if ($type -ceq 'Appx') { 'FilePublisherRule' } else { 'FilePathRule' }
            $rule = $document.CreateElement($kind)
            $rule.SetAttribute('Id', [guid]::NewGuid().ToString()); $rule.SetAttribute('Name', 'NSTU explicit recovery')
            $rule.SetAttribute('Description', 'SYSTEM and Administrators recovery only')
            $rule.SetAttribute('UserOrGroupSid', $sid); $rule.SetAttribute('Action', 'Allow')
            $conditions = $document.CreateElement('Conditions'); [void]$rule.AppendChild($conditions)
            if ($type -ceq 'Appx') {
                $condition = $document.CreateElement('FilePublisherCondition')
                foreach ($name in @('PublisherName','ProductName','BinaryName')) { $condition.SetAttribute($name, '*') }
                $range = $document.CreateElement('BinaryVersionRange')
                $range.SetAttribute('LowSection', '0.0.0.0'); $range.SetAttribute('HighSection', '*'); [void]$condition.AppendChild($range)
            } else { $condition = $document.CreateElement('FilePathCondition'); $condition.SetAttribute('Path', '*') }
            [void]$conditions.AppendChild($condition); [void]$collection.AppendChild($rule)
        }
    }
    foreach ($xml in $GeneratedXml) {
        $generated = Read-CsPolicyXml $xml
        if ($generated.DocumentElement.Name -cne 'AppLockerPolicy') { throw 'Invalid platform policy output.' }
        foreach ($sourceCollection in $generated.DocumentElement.ChildNodes) {
            $target = $document.DocumentElement.SelectSingleNode('RuleCollection[@Type="' + $sourceCollection.GetAttribute('Type') + '"]')
            if ($null -eq $target) { throw 'Unknown platform collection.' }
            foreach ($rule in $sourceCollection.ChildNodes) {
                if ($rule.GetAttribute('UserOrGroupSid') -cne $StudentSid) { throw 'Platform output has an unexpected SID.' }
                [void]$target.AppendChild($document.ImportNode($rule, $true))
            }
        }
    }
    Test-CsPolicyRules $document $StudentSid
    return $document.OuterXml
}

if (-not (Test-CsStudentSid $StudentSid)) { throw 'An individual standard-user exam SID is required.' }
$inventoryPath = Get-CsExistingPath $ApprovedInventoryPath -RequireTrusted
$outputPath = Get-CsExistingPath $OutputDirectory -Directory -RequireTrusted
if (([IO.FileInfo]::new($inventoryPath)).Length -gt 4194304) { throw 'Dependency inventory is too large.' }
$inventory = Get-CsApprovedInventory ([IO.File]::ReadAllText($inventoryPath) | ConvertFrom-Json)
$policyPath = Join-Path $outputPath 'cs-applocker-policy.xml'
$reviewPath = Join-Path $outputPath 'cs-applocker-review.json'
if ([IO.File]::Exists($policyPath) -or [IO.File]::Exists($reviewPath)) { throw 'Preparation outputs must be new files.' }
Import-Module AppLocker -ErrorAction Stop
$generated = [Collections.Generic.List[string]]::new()
foreach ($file in $inventory.files) {
    $path = Get-CsExistingPath $file.path
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $actualHash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
        if ($actualHash -ine $file.contentSha256) { throw 'Approved file content hash changed.' }
        $information = @(Get-AppLockerFileInformation -Path $path -ErrorAction Stop)
        if ($information.Count -ne 1 -or $information[0].AppX) { throw 'Exact approved file information unavailable.' }
        $generated.Add([string](New-AppLockerPolicy -FileInformation $information -RuleType Hash -User $StudentSid -Xml -ErrorAction Stop))
    } finally { $sha.Dispose(); $stream.Dispose() }
}
if ($inventory.packages.Count) {
    $installed = @(Get-AppxPackage -User $StudentSid -ErrorAction Stop)
    foreach ($approved in $inventory.packages) {
        $selected = @($installed | Where-Object { $_.PackageFullName -ceq $approved.packageFullName })
        if ($selected.Count -ne 1 -or $selected[0].Name -cne $approved.name -or $selected[0].Publisher -cne $approved.publisher -or
            $selected[0].Version.ToString() -cne $approved.version) { throw 'Approved installed package identity changed or missing.' }
        $information = @(Get-AppLockerFileInformation -Packages $selected -ErrorAction Stop)
        $packageXml = Read-CsPolicyXml ([string](New-AppLockerPolicy -FileInformation $information -RuleType Publisher -User $StudentSid -Xml -ErrorAction Stop))
        $rules = @($packageXml.SelectNodes('/AppLockerPolicy/RuleCollection[@Type="Appx"]/FilePublisherRule'))
        if ($rules.Count -ne 1) { throw 'Exact packaged rule unavailable.' }
        $condition = $rules[0].SelectSingleNode('Conditions/FilePublisherCondition')
        if ($null -eq $condition -or $condition.GetAttribute('PublisherName') -cne $approved.publisher -or $condition.GetAttribute('ProductName') -cne $approved.name) { throw 'Platform packaged identity mismatch.' }
        $condition.SetAttribute('BinaryName', '*')
        $range = $condition.SelectSingleNode('BinaryVersionRange')
        $range.SetAttribute('LowSection', $approved.version); $range.SetAttribute('HighSection', $approved.version)
        $generated.Add($packageXml.OuterXml)
    }
}
$policy = New-CsPolicyDocument $StudentSid $generated.ToArray()
$review = [ordered]@{ version=1; status='NOT_QUALIFIED'; deploymentMethod='GroupPolicy'; policyDigest=(Get-CsPolicyDigest $policy); studentSid=$StudentSid;
    dependenciesReviewed=$true; requiresPhysicalQualification=$true; inventory=$inventory;
    blockers=@('Review standard-user identity and every OS/IME/accessibility/runtime dependency', 'Qualify actual GP precedence and standard-user allow/block launches', 'Record physical IME/accessibility and recovery evidence before protected Begin') }
$encoding = [Text.UTF8Encoding]::new($false)
foreach ($entry in @(@{Path=$policyPath; Text=$policy}, @{Path=$reviewPath; Text=($review | ConvertTo-Json -Depth 8)})) {
    [void](Get-CsExistingPath $outputPath -Directory -RequireTrusted)
    $stream = [IO.File]::Open($entry.Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $bytes = $encoding.GetBytes($entry.Text); $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
}
Write-Output 'CS_POLICY_PREPARED_NOT_QUALIFIED'
