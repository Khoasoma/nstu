[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
$testBase = [IO.Path]::GetFullPath($TestRoot)
if (-not [IO.Path]::IsPathFullyQualified($TestRoot) -or -not (Test-Path -LiteralPath $testBase -PathType Container)) { throw 'Provide an existing absolute fixture root.' }
$fixtureRoot = Join-Path $testBase ('contest-package-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$stageScript = Join-Path $PSScriptRoot 'stage-contest-package.ps1'
$items = [Collections.Generic.SortedDictionary[string,byte[]]]::new([StringComparer]::Ordinal)
$items.Add('manifest.json', [Text.Encoding]::UTF8.GetBytes('{"id":"fixture","contentMode":"computer_science"}'))
foreach ($name in @('exam/web/index.html','exam/web/app.js','exam/web/styles.css','exam/shared/programming.js')) { $items.Add($name, [Text.Encoding]::UTF8.GetBytes('fixture: ' + $name)) }

function New-FixtureArchive([string]$path, [string]$extra = '', [switch]$Duplicate) {
    $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($pair in $items.GetEnumerator()) {
            $entry = $zip.CreateEntry($pair.Key, [IO.Compression.CompressionLevel]::NoCompression)
            $stream = $entry.Open()
            try { $stream.Write($pair.Value,0,$pair.Value.Length) } finally { $stream.Dispose() }
        }
        if ($extra) { $stream = $zip.CreateEntry($extra).Open(); $stream.Dispose() }
        if ($Duplicate) { $stream = $zip.CreateEntry('exam/web/app.js').Open(); $stream.Dispose() }
    } finally { $zip.Dispose() }
}
function Content-Pin {
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($pair in $items.GetEnumerator()) {
            $name = [Text.Encoding]::UTF8.GetBytes($pair.Key)
            foreach ($chunk in @([BitConverter]::GetBytes([UInt64]$name.Length),$name,[BitConverter]::GetBytes([UInt64]$pair.Value.Length),$pair.Value)) {
                [void]$hash.TransformBlock($chunk,0,$chunk.Length,$chunk,0)
            }
        }
        [void]$hash.TransformFinalBlock([byte[]]::new(0),0,0)
        return ([BitConverter]::ToString($hash.Hash)).Replace('-','').ToLowerInvariant()
    } finally { $hash.Dispose() }
}
function Assert-Rejected([string]$path,[string]$contentPin) {
    $rejected = $false
    try { & $stageScript -Mode Inspect -ArchivePath $path -ExpectedArchiveSha256 (Get-FileHash -LiteralPath $path).Hash -ExpectedContentSha256 $contentPin | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Unsafe or unpinned archive was accepted.' }
}
$archive = Join-Path $fixtureRoot 'public.nstuexam'
New-FixtureArchive $archive
$contentPin = Content-Pin
$result = & $stageScript -Mode Inspect -ArchivePath $archive -ExpectedArchiveSha256 (Get-FileHash -LiteralPath $archive).Hash -ExpectedContentSha256 $contentPin | ConvertFrom-Json
if ($result.content_sha256 -ne $contentPin -or $result.file_count -ne 5 -or -not $result.inspected_only) { throw 'Valid public archive inspection failed.' }
Assert-Rejected $archive ('0' * 64)
foreach ($name in @('../outside','/absolute','C:/device','exam/web/CON','exam/web/App.js','private/grading.json','exam/web/../app.js','exam/web/app.js:ads','exam/web/app.js.','exam\web\app.js')) {
    $malicious = Join-Path $fixtureRoot ([guid]::NewGuid().ToString('N') + '.zip')
    New-FixtureArchive $malicious $name
    Assert-Rejected $malicious $contentPin
}
$duplicate = Join-Path $fixtureRoot 'duplicate.zip'
New-FixtureArchive $duplicate -Duplicate
Assert-Rejected $duplicate $contentPin
$mismatch = $false
try { & $stageScript -Mode Inspect -ArchivePath $archive -ExpectedArchiveSha256 ('1'*64) -ExpectedContentSha256 $contentPin | Out-Null } catch { $mismatch = $true }
if (-not $mismatch) { throw 'Archive pin mismatch accepted.' }
# Exercise the actual pure Windows ACL validators without granting permissions
# to fixture files under the restricted execution token.
$parseTokens = $null; $parseErrors = $null
$scriptAst = [Management.Automation.Language.Parser]::ParseFile($stageScript,[ref]$parseTokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Package helper has a parse error.' }
foreach ($definition in $scriptAst.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Assert-OwnedAcl','Assert-AncestorAcl')},$true)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
$safeAcl = [Security.AccessControl.DirectorySecurity]::new()
$safeAcl.SetOwner($currentSid); $safeAcl.SetAccessRuleProtection($true,$false)
[void]$safeAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($currentSid,'FullControl','Allow'))
Assert-OwnedAcl $safeAcl $true
Assert-AncestorAcl $safeAcl
$unsafeChild = [Security.AccessControl.FileSecurity]::new()
$unsafeChild.SetOwner($currentSid)
[void]$unsafeChild.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-1-0'),'Modify','Allow'))
$childRejected = $false
try { Assert-OwnedAcl $unsafeChild $false } catch { $childRejected = $true }
if (-not $childRejected) { throw 'Unrelated-writable descendant accepted.' }
$unsafeParent = [Security.AccessControl.DirectorySecurity]::new()
$unsafeParent.SetOwner($currentSid)
[void]$unsafeParent.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-1-0'),'DeleteSubdirectoriesAndFiles','Allow'))
$parentRejected = $false
try { Assert-AncestorAcl $unsafeParent } catch { $parentRejected = $true }
if (-not $parentRejected) { throw 'Unrelated ancestor replacement authority accepted.' }
"PASS: public CS archive content pin, archive pin, private/traversal/alias/duplicate rejection."
"PASS: private descendant ACL and ancestor replacement authority rejection."
# Preserve the bounded fixtures as evidence; no recursive deletion of a computed path.
