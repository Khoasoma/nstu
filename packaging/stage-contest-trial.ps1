[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildRoot,
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$WebView2Loader,
    [Parameter(Mandatory=$true)][string]$PowerShellRoot,
    [ValidateSet('','Debug','Release','RelWithDebInfo','MinSizeRel')][string]$Configuration = '',
    [string]$CompilerRuntimeRoot = ''
)
$ErrorActionPreference = 'Stop'

function Full-Path([string]$path) {
    if (-not [IO.Path]::IsPathFullyQualified($path)) { throw 'Trial paths must be absolute.' }
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\','/')
    if ($full -eq [IO.Path]::GetPathRoot($full).TrimEnd('\','/')) { throw 'A drive root is not a trial directory.' }
    return $full
}
function Assert-NoReparse([string]$path) {
    $cursor = $path
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'Reparse-backed trial paths are forbidden.'
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Copy-Asset([string]$source, [string]$relative) {
    Assert-NoReparse $source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing trial asset: $relative" }
    $target = Join-Path $trialRoot $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target -ErrorAction Stop
}

$build = Full-Path $BuildRoot
$trialRoot = Full-Path $Destination
$powershell = Full-Path $PowerShellRoot
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
foreach ($path in @($build,$trialRoot,$powershell,(Full-Path $WebView2Loader))) { Assert-NoReparse $path }
if (Test-Path -LiteralPath $trialRoot) { throw 'Use a new trial destination; existing files are preserved.' }
if (-not (Test-Path -LiteralPath (Join-Path $powershell 'pwsh.exe') -PathType Leaf)) { throw 'PowerShell 7 runtime is required.' }
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($trialRoot)) -PathType Container)) { throw 'Trial parent must already exist.' }
$runtimeEntries = @(Get-ChildItem -LiteralPath $powershell -Recurse -Force)
if ($runtimeEntries.Count -gt 10000) { throw 'PowerShell runtime inventory is too large.' }
foreach ($entry in $runtimeEntries) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'PowerShell runtime contains a reparse point.' }
}
[IO.Directory]::CreateDirectory($trialRoot) | Out-Null
foreach ($role in @('teacher','student')) {
    $directory = if ($role -eq 'teacher') { 'server' } else { 'client' }
    if ($Configuration) { $directory = Join-Path $directory $Configuration }
    $binary = Join-Path (Join-Path $build $directory) $(if ($role -eq 'teacher') { 'nstu-server.exe' } else { 'nstu-student.exe' })
    Copy-Asset $binary "$role/$([IO.Path]::GetFileName($binary))"
    Copy-Asset $WebView2Loader "$role/WebView2Loader.dll"
    Copy-Asset (Join-Path $repo 'packaging/stage-contest-package.ps1') "$role/stage-contest-package.ps1"
    if ($CompilerRuntimeRoot) {
        $compilerRuntime = Full-Path $CompilerRuntimeRoot
        foreach ($dll in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
            Copy-Asset (Join-Path $compilerRuntime $dll) "$role/$dll"
        }
    }
}
foreach ($entry in $runtimeEntries | Where-Object { -not $_.PSIsContainer }) {
    if ($entry.Name -in @('profile.ps1','Microsoft.PowerShell_profile.ps1')) { continue }
    $relative = [IO.Path]::GetRelativePath($powershell, $entry.FullName)
    Copy-Asset $entry.FullName "runtime/powershell/$relative"
}
Copy-Asset (Join-Path $repo 'docs/BYOD_CONTEST_TRIAL.md') 'BYOD_CONTEST_TRIAL.md'
Copy-Asset (Join-Path $repo 'packaging/test-contest-trial.ps1') 'test-contest-trial.ps1'
$files = @(Get-ChildItem -LiteralPath $trialRoot -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
    @{ path = [IO.Path]::GetRelativePath($trialRoot,$_.FullName).Replace('\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes = $_.Length }
})
@{ version=1; status='INDEV'; judged=$false; files=$files } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $trialRoot 'trial-manifest.json') -Encoding utf8
& (Join-Path $repo 'packaging/test-contest-trial.ps1') -TrialRoot $trialRoot
if (-not $?) { throw 'Trial inventory verification failed.' }
Write-Output "Staged uninstalled trial: $trialRoot"
