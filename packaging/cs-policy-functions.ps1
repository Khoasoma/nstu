# Shared by policy preparation and the fixed read-only readiness probe.
function Test-CsStudentSid {
    param([string]$StudentSid)
    if ($StudentSid -cnotmatch '^S-1-5-21-[0-9]{1,10}-[0-9]{1,10}-[0-9]{1,10}-[0-9]{4,10}$') { return $false }
    foreach ($value in $StudentSid.Split('-')[4..7]) { if ([uint64]$value -gt [uint32]::MaxValue) { return $false } }
    return [uint64]($StudentSid.Split('-')[-1]) -ge 1000
}

function Test-CsTrustedAcl {
    param($Acl, [int]$WriteMask, [switch]$RequireProtected)
    $trusted = @('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
    if ($Acl.Owner -cnotin $trusted -or ($RequireProtected -and -not $Acl.AreAccessRulesProtected)) { return $false }
    foreach ($rule in $Acl.Rules) {
        if ($rule.Allow -and ($rule.Rights -band $WriteMask) -ne 0 -and $rule.Sid -cnotin $trusted) { return $false }
    }
    return $true
}

function ConvertTo-CsAclRecord {
    param($Acl)
    return [pscustomobject]@{
        Owner=$Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
        AreAccessRulesProtected=$Acl.AreAccessRulesProtected
        Rules=@($Acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | ForEach-Object {
            $rights = if ($null -ne $_.PSObject.Properties['RegistryRights']) { [int]$_.RegistryRights } else { [int]$_.FileSystemRights }
            [pscustomobject]@{ Sid=$_.IdentityReference.Value; Allow=($_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow); Rights=$rights }
        })
    }
}

function Test-CsRegistryProtection {
    param([object[]]$Acls)
    if ($Acls.Count -ne 4) { return $false }
    $mask = [int]([Security.AccessControl.RegistryRights]::SetValue -bor [Security.AccessControl.RegistryRights]::CreateSubKey -bor [Security.AccessControl.RegistryRights]::Delete -bor [Security.AccessControl.RegistryRights]::ChangePermissions -bor [Security.AccessControl.RegistryRights]::TakeOwnership)
    for ($index = 0; $index -lt $Acls.Count; $index++) {
        if (-not (Test-CsTrustedAcl $Acls[$index] $mask -RequireProtected:($index -eq 3))) { return $false }
    }
    return $true
}

function Read-CsPolicyXml {
    param([string]$PolicyXml)
    if ([string]::IsNullOrWhiteSpace($PolicyXml) -or $PolicyXml.Length -gt 4194304) { throw 'Invalid policy size.' }
    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $settings.IgnoreWhitespace = $true
    $settings.MaxCharactersInDocument = 4194304
    $reader = [Xml.XmlReader]::Create([IO.StringReader]::new($PolicyXml), $settings)
    try {
        $document = [Xml.XmlDocument]::new()
        $document.XmlResolver = $null
        $document.Load($reader)
        return ,$document
    } finally { $reader.Dispose() }
}

function Test-CsPolicyRules {
    param([Xml.XmlDocument]$Document, [string]$StudentSid)
    if (-not (Test-CsStudentSid $StudentSid)) { throw 'Invalid student SID.' }
    $root = $Document.DocumentElement
    foreach ($element in $Document.SelectNodes('//*')) { if ($element.NamespaceURI -cne '') { throw 'Unexpected policy namespace.' } }
    if ($root.Name -cne 'AppLockerPolicy' -or $root.NamespaceURI -cne '' -or $root.GetAttribute('Version') -cne '1' -or
        $root.Attributes.Count -ne 1 -or $root.ChildNodes.Count -ne 5) { throw 'Invalid policy root.' }
    $types = @('Exe','Dll','Script','Msi','Appx')
    $seenTypes = @{}; $seenIds = @{}
    foreach ($collection in $root.ChildNodes) {
        $type = $collection.GetAttribute('Type')
        if ($collection.Name -cne 'RuleCollection' -or $type -cnotin $types -or $seenTypes.ContainsKey($type) -or
            $collection.GetAttribute('EnforcementMode') -cne 'Enforced' -or $collection.Attributes.Count -ne 2) { throw 'Invalid rule collection.' }
        $seenTypes[$type] = $true
        $recovery = @{}; $studentRules = 0
        foreach ($rule in $collection.ChildNodes) {
            $ruleSid = $rule.GetAttribute('UserOrGroupSid')
            $id = $rule.GetAttribute('Id'); $guid = [guid]::Empty
            if (-not [guid]::TryParse($id, [ref]$guid) -or $seenIds.ContainsKey($id) -or $rule.Attributes.Count -ne 5 -or
                $rule.GetAttribute('Action') -cne 'Allow' -or $rule.ChildNodes.Count -ne 1 -or
                $rule.FirstChild.Name -cne 'Conditions' -or $rule.FirstChild.Attributes.Count -ne 0 -or
                $rule.FirstChild.ChildNodes.Count -ne 1) { throw 'Invalid rule.' }
            foreach ($attribute in $rule.Attributes) {
                if ($attribute.Name -cnotin @('Id','Name','Description','UserOrGroupSid','Action')) { throw 'Unexpected rule attribute.' }
            }
            $seenIds[$id] = $true
            $condition = $rule.FirstChild.FirstChild
            if ($ruleSid -cin @('S-1-5-18','S-1-5-32-544')) {
                if ($recovery.ContainsKey($ruleSid)) { throw 'Duplicate recovery rule.' }
                $recovery[$ruleSid] = $true
                if ($type -cne 'Appx') {
                    if ($rule.LocalName -cne 'FilePathRule' -or $condition.Name -cne 'FilePathCondition' -or
                        $condition.GetAttribute('Path') -cne '*' -or $condition.Attributes.Count -ne 1 -or $condition.ChildNodes.Count -ne 0) { throw 'Invalid recovery path rule.' }
                    continue
                }
            } elseif ($ruleSid -ceq $StudentSid) {
                $studentRules++
                if ($type -cne 'Appx') {
                    if ($rule.LocalName -cne 'FileHashRule' -or $condition.Name -cne 'FileHashCondition' -or
                        $condition.Attributes.Count -ne 0 -or $condition.ChildNodes.Count -lt 1) { throw 'Student rules must use file hashes.' }
                    foreach ($fileHash in $condition.ChildNodes) {
                        if ($fileHash.Name -cne 'FileHash' -or $fileHash.GetAttribute('Type') -cne 'SHA256' -or
                            $fileHash.GetAttribute('Data') -cnotmatch '^0x[A-Fa-f0-9]{64}$' -or $fileHash.Attributes.Count -ne 4 -or
                            -not $fileHash.HasAttribute('SourceFileName') -or $fileHash.GetAttribute('SourceFileLength') -cnotmatch '^[0-9]+$' -or $fileHash.ChildNodes.Count -ne 0) { throw 'Invalid file hash.' }
                    }
                    continue
                }
            } else { throw 'Unreviewed rule SID.' }
            if ($rule.LocalName -cne 'FilePublisherRule' -or $condition.Name -cne 'FilePublisherCondition' -or
                $condition.Attributes.Count -ne 3 -or $condition.ChildNodes.Count -ne 1 -or
                $condition.FirstChild.Name -cne 'BinaryVersionRange' -or $condition.FirstChild.Attributes.Count -ne 2 -or
                $condition.FirstChild.ChildNodes.Count -ne 0 -or $condition.GetAttribute('BinaryName') -cne '*') { throw 'Invalid packaged-app rule.' }
            $range = $condition.FirstChild
            if ($ruleSid -ceq $StudentSid) {
                foreach ($attributeName in @('PublisherName','ProductName')) {
                    $value = $condition.GetAttribute($attributeName)
                    if ([string]::IsNullOrWhiteSpace($value) -or $value -match '[*?]') { throw 'Packaged identity must be exact.' }
                }
                if ($range.GetAttribute('LowSection') -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' -or
                    $range.GetAttribute('LowSection') -cne $range.GetAttribute('HighSection')) { throw 'Packaged version must be exact.' }
            } elseif ($condition.GetAttribute('PublisherName') -cne '*' -or $condition.GetAttribute('ProductName') -cne '*' -or
                $range.GetAttribute('LowSection') -cne '0.0.0.0' -or $range.GetAttribute('HighSection') -cne '*') { throw 'Invalid packaged recovery rule.' }
        }
        if ($recovery.Count -ne 2 -or ($type -cin @('Exe','Dll') -and $studentRules -eq 0)) { throw 'Missing required student or recovery rules.' }
    }
}

function Get-CsPolicyDigest {
    param([string]$PolicyXml)
    $document = Read-CsPolicyXml $PolicyXml
    # GP may reorder collections/rules; preserve all actual rule content, IDs and attributes.
    foreach ($collection in @($document.DocumentElement.ChildNodes)) {
        foreach ($rule in @($collection.ChildNodes | Sort-Object { $_.GetAttribute('Id') })) { [void]$collection.AppendChild($rule) }
    }
    foreach ($collection in @($document.DocumentElement.ChildNodes | Sort-Object { $_.GetAttribute('Type') })) { [void]$document.DocumentElement.AppendChild($collection) }
    if (-not ('Security.Cryptography.Xml.XmlDsigC14NTransform' -as [type])) { Add-Type -AssemblyName System.Security -ErrorAction Stop }
    $transform = [Security.Cryptography.Xml.XmlDsigC14NTransform]::new()
    $transform.LoadInput($document)
    $stream = $transform.GetOutput([IO.Stream])
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Test-CsPolicyReadiness {
    param($Settings, [string]$ServiceStatus, [string]$PolicyXml)
    $code = 'configuration_invalid'
    if ($null -ne $Settings) {
        if ($Settings.DeploymentMethod -isnot [string] -or $Settings.DeploymentMethod -cne 'GroupPolicy') { $code = 'deployment_method_invalid' }
        elseif ($Settings.PolicyDigest -is [string] -and $Settings.PolicyDigest -cmatch '^[A-Fa-f0-9]{64}$' -and
            $Settings.StudentSid -is [string] -and (Test-CsStudentSid $Settings.StudentSid)) {
            $code = 'physical_not_qualified'
            if ($Settings.PhysicalQualified -isnot [int] -or $Settings.PhysicalQualified -ne 1) {
                return [pscustomobject]@{ version=1; status='unavailable'; code=$code }
            }
            $code = 'appid_not_running'
            if ($ServiceStatus -ceq 'Running') {
                $code = 'policy_invalid'
                try {
                    $document = Read-CsPolicyXml $PolicyXml
                    Test-CsPolicyRules $document $Settings.StudentSid
                    $code = 'policy_digest_mismatch'
                    if ((Get-CsPolicyDigest $PolicyXml) -ceq $Settings.PolicyDigest.ToLowerInvariant()) { $code = 'policy_verified' }
                } catch { $code = 'policy_invalid' }
            }
        }
    }
    return [pscustomobject]@{ version=1; status=$(if ($code -ceq 'policy_verified') { 'verified' } else { 'unavailable' }); code=$code }
}
