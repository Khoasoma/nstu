[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TrialRoot)
$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathFullyQualified($TrialRoot)) { throw 'Trial root must be absolute.' }
$root = [IO.Path]::GetFullPath($TrialRoot).TrimEnd('\','/')
if ($root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\','/')) { throw 'A drive root is not a trial directory.' }
$cursor = $root
while ($cursor) {
    if ((Test-Path -LiteralPath $cursor) -and
        (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { throw 'Trial root has a reparse ancestor.' }
    $parent = [IO.Path]::GetDirectoryName($cursor)
    if ($parent -eq $cursor) { break }
    $cursor = $parent
}
$entries = @(Get-ChildItem -LiteralPath $root -Recurse -Force)
foreach ($entry in $entries) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Trial contains a reparse point.' }
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'trial-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne 1 -or $manifest.status -ne 'INDEV' -or $manifest.judged -ne $false -or
    $manifest.files.Count -gt 10000) { throw 'Invalid trial manifest.' }
$paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $manifest.files) {
    $relative = [string]$file.path
    if (-not $relative -or $relative.Contains('\') -or $relative.StartsWith('/') -or
        $relative.Contains(':') -or @($relative.Split('/') | Where-Object {
            $_ -in @('','.','..') -or $_ -match '[ .]$' -or $_ -match '[\x00-\x1f<>"|?*]' -or
            $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$)'
        }).Count -ne 0 -or
        $file.sha256 -cnotmatch '^[a-f0-9]{64}$' -or -not $paths.Add($relative)) { throw 'Invalid or duplicate trial path/pin.' }
    if ($relative -notmatch '^(teacher|student|runtime/powershell)/' -and
        $relative -notin @('BYOD_CONTEST_TRIAL.md','test-contest-trial.ps1')) { throw 'Unexpected trial asset.' }
    if ($relative -match '^(teacher|student)/' -and
        $relative -notmatch '^(teacher/nstu-server\.exe|student/nstu-student\.exe|(teacher|student)/(WebView2Loader\.dll|stage-contest-package\.ps1|libstdc\+\+-6\.dll|libgcc_s_seh-1\.dll|libwinpthread-1\.dll))$') {
        throw 'Unexpected teacher/student asset.'
    }
    if ($relative -match '(?i)(nstu-(service|agent|provision)|private[-_]tests|grading|enrollment|keyring|\.sqlite|(^|/)(profiles|artifacts|outbox)(/|$)|(^|/)(profile|Microsoft\.PowerShell_profile)\.ps1$)') {
        throw 'Private or managed-install material found in trial.'
    }
    $full = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $full -PathType Leaf) -or
        (Get-Item -LiteralPath $full -Force).Length -ne $file.bytes -or
        (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw "Trial asset mismatch: $relative" }
}
$actual = @($entries | Where-Object { -not $_.PSIsContainer -and $_.FullName -ne (Join-Path $root 'trial-manifest.json') })
foreach ($file in $actual) {
    $relative = [IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/')
    if (-not $paths.Contains($relative)) { throw "Unlisted trial file found: $relative" }
}
if ($actual.Count -ne $paths.Count) { throw 'Unlisted trial files found.' }
foreach ($required in @('teacher/nstu-server.exe','student/nstu-student.exe','teacher/WebView2Loader.dll','student/WebView2Loader.dll',
    'teacher/stage-contest-package.ps1','student/stage-contest-package.ps1','runtime/powershell/pwsh.exe','BYOD_CONTEST_TRIAL.md','test-contest-trial.ps1')) {
    if (-not $paths.Contains($required)) { throw "Missing required trial asset: $required" }
}
Write-Output "Trial inventory passed: $($paths.Count) pinned files; no installed service or private grading."
