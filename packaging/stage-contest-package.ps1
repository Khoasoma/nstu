[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ArchivePath,
    [Parameter(Mandatory=$true)][string]$ExpectedArchiveSha256,
    [Parameter(Mandatory=$true)][string]$ExpectedContentSha256,
    [ValidateSet('Inspect','Publish')][string]$Mode = 'Publish',
    [string]$PackagesRoot = ''
)
$ErrorActionPreference = 'Stop'
# This unprivileged extractor handles only the public CS asset surface. Native
# inspect_exam_package must still validate the full manifest before Prepared.
$allowedFiles = @('manifest.json','manifest.p7s','exam/web/index.html','exam/web/app.js','exam/web/styles.css','exam/shared/programming.js','exam/web/assets/nstu-brand.png')
$requiredFiles = @('manifest.json','exam/web/index.html','exam/web/app.js','exam/web/styles.css','exam/shared/programming.js')
$allowedDirectories = @('exam/','exam/web/','exam/shared/','exam/web/assets/')
$maxArchive = [UInt64]268435456
$maxExpanded = [UInt64]134217728
$maxFile = [UInt64]67108864

function Normalize-Pin([string]$value) {
    if ($value -notmatch '^[a-fA-F0-9]{64}$' -or $value -match '^0{64}$') { throw 'A nonzero SHA256 pin is required.' }
    return $value.ToLowerInvariant()
}
function Full-Path([string]$value) {
    if (-not [IO.Path]::IsPathFullyQualified($value)) { throw 'Paths must be absolute.' }
    $full = [IO.Path]::GetFullPath($value)
    if ($full -eq [IO.Path]::GetPathRoot($full)) { return $full }
    return $full.TrimEnd('\','/')
}
function Assert-NoReparse([string]$value) {
    $cursor = $value
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse-backed paths are forbidden.' }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Assert-OwnedAcl([Security.AccessControl.FileSystemSecurity]$acl, [bool]$protectedRoot) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $identity.User.Value -or
        ($protectedRoot -and -not $acl.AreAccessRulesProtected)) { throw 'Package ownership is not private.' }
    foreach ($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            $rule.IdentityReference.Value -notin @($identity.User.Value,'S-1-5-18','S-1-3-4')) { throw 'Package grants an unrelated principal.' }
    }
}
function Assert-AncestorAcl([Security.AccessControl.FileSystemSecurity]$acl) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $trusted = @($identity.User.Value,'S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin $trusted) { throw 'Package ancestor has an unrelated owner.' }
    # Creating siblings cannot replace an existing root; deleting its child or
    # changing the ancestor's DACL/owner can. Inherit-only grants do not apply here.
    $replaceRights = [int][Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
        [int][Security.AccessControl.FileSystemRights]::Delete -bor
        [int][Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [int][Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if (($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -ne 0) { continue }
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            ([int]$rule.FileSystemRights -band $replaceRights) -ne 0 -and
            $rule.IdentityReference.Value -notin ($trusted + 'S-1-3-4')) { throw 'Package ancestor permits unrelated replacement.' }
    }
}
function Assert-SafeAncestors([string]$path) {
    $cursor = [IO.Path]::GetDirectoryName($path)
    while ($cursor) {
        Assert-NoReparse $cursor
        Assert-AncestorAcl (Get-Acl -LiteralPath $cursor)
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Assert-PrivateRoot([string]$path) {
    Assert-NoReparse $path
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw 'Private package root is unavailable.' }
    Assert-OwnedAcl (Get-Acl -LiteralPath $path) $true
    Assert-SafeAncestors $path
}
function Set-PrivateStage([string]$path) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetOwner($identity.User)
    $acl.SetAccessRuleProtection($true,$false)
    foreach ($sid in @($identity.User,[Security.Principal.SecurityIdentifier]::new('S-1-5-18'))) {
        $rule = [Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
        [void]$acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $path -AclObject $acl
    Assert-PrivateRoot $path
}
function Hash-Block($hash,[byte[]]$bytes) { if ($bytes.Length) { [void]$hash.TransformBlock($bytes,0,$bytes.Length,$bytes,0) } }
function Hash-Length($hash,[UInt64]$length) { Hash-Block $hash ([BitConverter]::GetBytes($length)) }
function Finish-Hash($hash) {
    [void]$hash.TransformFinalBlock([byte[]]::new(0),0,0)
    return ([BitConverter]::ToString($hash.Hash)).Replace('-','').ToLowerInvariant()
}
function Digest-Folder([string]$root) {
    Assert-NoReparse $root
    $files = [Collections.Generic.SortedDictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($item in Get-ChildItem -LiteralPath $root -Recurse -Force) {
        Assert-NoReparse $item.FullName
        Assert-OwnedAcl (Get-Acl -LiteralPath $item.FullName) $false
        if ($item.PSIsContainer) { continue }
        if (-not ($item -is [IO.FileInfo])) { throw 'Nonregular package file.' }
        $name = $item.FullName.Substring($root.Length).TrimStart('\','/').Replace('\','/')
        if ($name -cnotin $allowedFiles -or $files.ContainsKey($name)) { throw 'Unexpected public package file.' }
        $files.Add($name,$item)
    }
    foreach ($required in $requiredFiles) { if (-not $files.ContainsKey($required)) { throw 'Public asset is missing.' } }
    $hash = [Security.Cryptography.SHA256]::Create()
    [UInt64]$total = 0
    try {
        foreach ($pair in $files.GetEnumerator()) {
            $size = [UInt64]$pair.Value.Length
            if ($size -gt $maxFile -or $size -gt $maxExpanded-$total) { throw 'Published package exceeds quota.' }
            $total += $size
            $name = [Text.Encoding]::UTF8.GetBytes($pair.Key)
            Hash-Length $hash $name.Length; Hash-Block $hash $name; Hash-Length $hash $size
            $input = [IO.File]::Open($pair.Value.FullName,'Open','Read','Read')
            try {
                $buffer = [byte[]]::new(65536)
                [UInt64]$observed = 0
                while (($read = $input.Read($buffer,0,$buffer.Length)) -gt 0) {
                    if ([UInt64]$read -gt $size-$observed) { throw 'Published file grew during hashing.' }
                    [void]$hash.TransformBlock($buffer,0,$read,$buffer,0); $observed += [UInt64]$read
                }
                if ($observed -ne $size) { throw 'Published file changed during hashing.' }
            } finally { $input.Dispose() }
        }
        return Finish-Hash $hash
    } finally { $hash.Dispose() }
}

$archivePin = Normalize-Pin $ExpectedArchiveSha256
$contentPin = Normalize-Pin $ExpectedContentSha256
$archiveFull = Full-Path $ArchivePath
Assert-NoReparse $archiveFull
$archiveStream = $null; $zip = $null; $stage = $null; $publishLock = $null
try {
    $archiveStream = [IO.File]::Open($archiveFull,'Open','Read','Read')
    if ($archiveStream.Length -le 0 -or [UInt64]$archiveStream.Length -gt $maxArchive) { throw 'Archive size exceeds quota.' }
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $observed = ([BitConverter]::ToString($hash.ComputeHash($archiveStream))).Replace('-','').ToLowerInvariant() } finally { $hash.Dispose() }
    if ($observed -ne $archivePin) { throw 'Archive pin mismatch.' }
    $archiveStream.Position = 0
    $zip = [IO.Compression.ZipArchive]::new($archiveStream,[IO.Compression.ZipArchiveMode]::Read,$true)
    if ($zip.Entries.Count -gt 4096) { throw 'Archive entry quota exceeded.' }
    $entries = [Collections.Generic.SortedDictionary[string,object]]::new([StringComparer]::Ordinal)
    $allNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [UInt64]$expanded = 0
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName
        if (-not $allNames.Add($name)) { throw 'Duplicate or aliased entry.' }
        $attributes = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$entry.ExternalAttributes),0)
        $unixType = ($attributes -shr 16) -band 0xf000
        if ($unixType -notin @(0,0x8000,0x4000) -or ($attributes -band 0x400) -ne 0) { throw 'Archive links and special files are forbidden.' }
        if ($name.EndsWith('/')) {
            if ($name -cnotin $allowedDirectories -or $entry.Length -ne 0) { throw 'Unexpected archive directory.' }
            continue
        }
        if ($name -cnotin $allowedFiles -or $unixType -eq 0x4000) { throw 'Unknown/private/unsafe archive entry.' }
        $size = [UInt64]$entry.Length
        if ($size -gt $maxFile -or $size -gt $maxExpanded-$expanded -or ($name -eq 'manifest.json' -and $size -gt 131072)) { throw 'Expanded entry quota exceeded.' }
        if ($size -gt 0 -and ($entry.CompressedLength -le 0 -or $size -gt [UInt64]$entry.CompressedLength*1000)) { throw 'Compression ratio exceeds quota.' }
        $expanded += $size
        $entries.Add($name,$entry)
    }
    foreach ($required in $requiredFiles) { if (-not $entries.ContainsKey($required)) { throw 'Public asset is missing.' } }
    if ($Mode -eq 'Publish') {
        $packagesFull = Full-Path $PackagesRoot
        Assert-PrivateRoot $packagesFull
        $publishLock = [IO.File]::Open((Join-Path $packagesFull ($contentPin + '.lock')),'OpenOrCreate','ReadWrite','None')
        $stage = Join-Path $packagesFull ('.stage-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $stage | Out-Null
        Set-PrivateStage $stage
    }
    $contentHash = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($pair in $entries.GetEnumerator()) {
            $name = [Text.Encoding]::UTF8.GetBytes($pair.Key)
            Hash-Length $contentHash $name.Length; Hash-Block $contentHash $name; Hash-Length $contentHash ([UInt64]$pair.Value.Length)
            $input = $pair.Value.Open(); $output = $null
            try {
                if ($stage) {
                    Assert-PrivateRoot $stage
                    $outputPath = Join-Path $stage $pair.Key.Replace('/','\')
                    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($outputPath)) -Force | Out-Null
                    Assert-NoReparse $outputPath
                    $output = [IO.File]::Open($outputPath,'CreateNew','Write','None')
                }
                $buffer = [byte[]]::new(65536); [UInt64]$total = 0
                while (($read = $input.Read($buffer,0,$buffer.Length)) -gt 0) {
                    if ([UInt64]$read -gt [UInt64]$pair.Value.Length-$total) { throw 'Entry expanded beyond declared size.' }
                    [void]$contentHash.TransformBlock($buffer,0,$read,$buffer,0)
                    if ($output) { $output.Write($buffer,0,$read) }
                    $total += [UInt64]$read
                }
                if ($total -ne [UInt64]$pair.Value.Length) { throw 'Entry size mismatch.' }
                if ($output) { $output.Flush($true) }
            } finally { $input.Dispose(); if ($output) { $output.Dispose() } }
        }
        if ((Finish-Hash $contentHash) -ne $contentPin) { throw 'Public content pin mismatch.' }
    } finally { $contentHash.Dispose() }
    $published = ''
    if ($stage) {
        Assert-PrivateRoot $packagesFull
        Assert-PrivateRoot $stage
        if ((Digest-Folder $stage) -ne $contentPin) { throw 'Extracted public content changed.' }
        $published = Join-Path $packagesFull $contentPin
        Assert-NoReparse $published
        if (Test-Path -LiteralPath $published) {
            Assert-PrivateRoot $published
            if ((Digest-Folder $published) -ne $contentPin) { throw 'Immutable package conflict; existing data retained.' }
        } else {
            [IO.Directory]::Move($stage,$published)
            $stage = $null
        }
    }
    [ordered]@{content_sha256=$contentPin;archive_sha256=$archivePin;file_count=$entries.Count;expanded_bytes=$expanded;package_root=$published;inspected_only=($Mode -eq 'Inspect');manifest_validated=$false} | ConvertTo-Json -Compress
} finally {
    if ($zip) { $zip.Dispose() }; if ($archiveStream) { $archiveStream.Dispose() }; if ($publishLock) { $publishLock.Dispose() }
    if ($stage -and (Test-Path -LiteralPath $stage)) {
        $checked = [IO.Path]::GetFullPath($stage)
        $prefix = $packagesFull + [IO.Path]::DirectorySeparatorChar
        if ($checked.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($checked) -match '^\.stage-[a-f0-9]{32}$') {
            Assert-NoReparse $checked
            Assert-SafeAncestors $checked
            Remove-Item -LiteralPath $checked -Recurse -Force
        }
    }
}
