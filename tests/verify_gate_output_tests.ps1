#requires -Version 7.3
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $RepositoryRoot,
    [Parameter(Mandatory)][string] $RunRoot,
    [Parameter(Mandatory)][string] $BashPath,
    [Parameter(Mandatory)][string] $GitPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$run = $null
$summary = [ordered]@{ status = 'SETUP'; failure = $null; source_head = $null; gate_sha256 = $null; processes = @() }
$processNumber = 0

function Require([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
}

# Reject junctions/symlinks in every existing ancestor before creating or reading
# owned output. All test artifacts are retained, including on timeout/failure.
function Safe-Path([string] $Path, [switch] $MustExist) {
    Require ([IO.Path]::IsPathFullyQualified($Path)) "[SETUP] Expected an explicit absolute path: $Path"
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    Require ($full -match '^[Dd]:\\') "[SETUP] Expected an absolute D: path: $Path"
    $ancestor = $full
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            Require (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "[SETUP] Reparse point: $ancestor"
        }
        $parent = [IO.Path]::GetDirectoryName($ancestor)
        if ($parent -eq $ancestor) { break }
        $ancestor = $parent
    }
    if ($MustExist) {
        Require (Test-Path -LiteralPath $full) "[SETUP] Missing path: $full"
        $full = (Resolve-Path -LiteralPath $full).ProviderPath.TrimEnd('\', '/')
    }
    return $full
}

function Owned-Path([string] $Path) {
    $full = Safe-Path $Path
    Require ($full.StartsWith($script:run + '\', [StringComparison]::OrdinalIgnoreCase)) "[SETUP] Output escapes owned run: $full"
    return $full
}

function Write-Receipt([string] $Path, [object] $Value) {
    $full = Owned-Path $Path
    $bytes = $script:utf8.GetBytes(($Value | ConvertTo-Json -Depth 10) + "`n")
    $file = [IO.File]::Open($full, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($bytes); $file.Flush($true) } finally { $file.Dispose() }
}

function Invoke-Child([string] $Program, [string[]] $Arguments, [string] $Directory) {
    $script:processNumber++
    $receiptBase = Owned-Path (Join-Path $script:run ('receipts/child-{0:D2}' -f $script:processNumber))
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Program
    $start.WorkingDirectory = $Directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $start.Environment['TEMP'] = $script:childTemp
    $start.Environment['TMP'] = $script:childTemp
    $start.Environment['TMPDIR'] = $script:childTemp.Replace('\', '/')
    $start.Environment['PATH'] = $script:childPath
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timedOut = $false
    try {
        Require ($process.Start()) "[SETUP] Could not start $Program"
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(120000)) {
            $timedOut = $true
            $process.Kill($true)
            Require ($process.WaitForExit(10000)) "[SETUP] Timed-out child did not exit; retain $script:run"
        }
        Require ($stdoutTask.Wait(10000) -and $stderrTask.Wait(10000)) "[SETUP] Child streams did not close; retain $script:run"
        $result = [pscustomobject]@{
            program = $Program; arguments = $Arguments; directory = $Directory
            exit_code = $process.ExitCode; timed_out = $timedOut
            stdout = $stdoutTask.GetAwaiter().GetResult(); stderr = $stderrTask.GetAwaiter().GetResult()
            receipt = $receiptBase + '.json'
        }
        [IO.File]::WriteAllText($receiptBase + '.stdout.txt', $result.stdout, $script:utf8)
        [IO.File]::WriteAllText($receiptBase + '.stderr.txt', $result.stderr, $script:utf8)
        Write-Receipt $result.receipt $result
        $script:summary.processes += $result.receipt
        Require (-not $timedOut) "[SETUP] Child timed out: $Program; receipt $($result.receipt)"
        return $result
    } finally { $process.Dispose() }
}

function Git-Read([string[]] $Arguments, [string] $Directory) {
    $result = Invoke-Child $script:GitPath (@('-c', "core.hooksPath=$script:hooks") + $Arguments) $Directory
    Require ($result.exit_code -eq 0) "[SETUP] Git failed; receipt $($result.receipt)"
    return $result.stdout.Trim()
}

function New-Fixture([string] $Name) {
    $fixture = Owned-Path (Join-Path $script:run $Name)
    $clone = Invoke-Child $script:GitPath @(
        '-c', "core.hooksPath=$script:hooks", 'clone', '--no-local', '--no-hardlinks', '--no-checkout',
        '--config', "core.hooksPath=$script:hooks", '--config', 'core.autocrlf=false', '--', $script:RepositoryRoot, $fixture
    ) $script:run
    Require ($clone.exit_code -eq 0) "[SETUP] Local clone failed; receipt $($clone.receipt)"
    $null = Safe-Path $fixture -MustExist
    $null = Git-Read @('checkout', '--detach', $script:sourceHead) $fixture
    Require ((Git-Read @('rev-parse', '--verify', 'HEAD') $fixture) -eq $script:sourceHead) '[SETUP] Fixture HEAD differs from source'
    $gate = Owned-Path (Join-Path $fixture 'scripts/verify-gate.sh')
    [IO.File]::WriteAllBytes($gate, $script:gateBytes)
    Require ((Get-FileHash -LiteralPath $gate -Algorithm SHA256).Hash -eq $script:gateHash) '[SETUP] Working gate overlay differs'
    $build = Owned-Path (Join-Path $fixture 'build')
    $null = New-Item -ItemType Directory -Path $build
    $sentinel = Owned-Path (Join-Path $build 'gate')
    $file = [IO.File]::Open($sentinel, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($script:utf8.GetBytes('owned binary-directory refusal sentinel')); $file.Flush($true) } finally { $file.Dispose() }
    return [pscustomobject]@{ root = $fixture; sentinel = $sentinel; sha256 = (Get-FileHash -LiteralPath $sentinel).Hash }
}

function Check-Bash([string] $Fixture) {
    # Literal shell text only; user paths travel through direct argv/environment.
    $probe = Invoke-Child $script:BashPath @('--noprofile', '--norc', '-c',
        'set -e; cygpath -am /tmp; command -v git; command -v node; command -v cmake; git --version; node --version; cmake --version') $Fixture
    Require ($probe.exit_code -eq 0) "[SETUP] Real Git/Node/CMake preflight failed; receipt $($probe.receipt)"
    $mapped = ($probe.stdout -split '\r?\n')[0].Trim()
    $resolved = Safe-Path $mapped -MustExist
    Require ($resolved.Equals($script:childTemp, [StringComparison]::OrdinalIgnoreCase)) "[SETUP] Child /tmp is not owned D: temp: $mapped"
}

function Gate-Files([string] $Fixture) {
    $parent = Owned-Path (Join-Path $Fixture 'build/verify-gate-logs')
    if (-not (Test-Path -LiteralPath $parent)) { return @() }
    $null = Safe-Path $parent -MustExist
    $items = @(Get-ChildItem -LiteralPath $parent -Recurse -Force)
    foreach ($item in $items) { $null = Owned-Path $item.FullName }
    return @($items | Where-Object { -not $_.PSIsContainer })
}

function Check-Refusal([object] $Result, [object] $Fixture) {
    $output = $Result.stdout + "`n" + $Result.stderr
    Require ($Result.exit_code -ne 0) '[SETUP] Gate did not refuse the binary-directory sentinel'
    Require ($output -match 'PASS\s+node --check exam/web/app\.js' -and $output -match 'PASS\s+exam web recovery tests: \d+ passed') '[SETUP] Real Node suite did not pass'
    $failures = @([regex]::Matches($output, '(?m)^\s+FAIL\s+(.+)$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    Require ($failures.Count -eq 1 -and $failures[0] -eq 'configure failed:') "[SETUP] Gate failed outside the intended CMake refusal; receipt $($Result.receipt)"
    Require ($output -match 'CMake Error' -and $output -match 'build[/\\]gate' -and
        $output -match '(pkgRedirects|[Cc]annot create|[Uu]nable to .*create|[Ff]ailed to create)') "[SETUP] Intended real CMake directory diagnostic missing; receipt $($Result.receipt)"
    Require ((Test-Path -LiteralPath $Fixture.sentinel -PathType Leaf) -and
        (Get-FileHash -LiteralPath $Fixture.sentinel).Hash -eq $Fixture.sha256) '[SETUP] CMake changed the refusal sentinel'
    Require (-not (Test-Path -LiteralPath (Join-Path $Fixture.root 'build/gate/CMakeCache.txt'))) '[SETUP] Unexpected CMake cache'
}

function Configure-Log([object[]] $Files, [string] $Fixture) {
    $diagnosticFiles = @($Files | Where-Object {
        $text = [IO.File]::ReadAllText($_.FullName)
        $text -match 'CMake Error' -and $text -match 'build[/\\]gate' -and
        $text -match '(pkgRedirects|[Cc]annot create|[Uu]nable to .*create|[Ff]ailed to create)'
    })
    Require ($diagnosticFiles.Count -eq 1) '[ROUTING] Expected one new project-owned configure diagnostic log'
    $log = $diagnosticFiles[0].FullName
    $runDirectory = [IO.Path]::GetDirectoryName($log)
    $parent = Owned-Path (Join-Path $Fixture 'build/verify-gate-logs')
    Require ([IO.Path]::GetDirectoryName($runDirectory).Equals($parent, [StringComparison]::OrdinalIgnoreCase)) '[ROUTING] Configure log lacks a unique project-owned run directory'
    return [pscustomobject]@{ path = $log; run = $runDirectory; sha256 = (Get-FileHash -LiteralPath $log).Hash }
}

try {
    $RepositoryRoot = Safe-Path $RepositoryRoot -MustExist
    $RunRoot = Safe-Path $RunRoot
    Require ($RunRoot -ne $RepositoryRoot -and $RunRoot -ne 'D:') '[SETUP] RunRoot must be an explicit output directory'
    foreach ($program in @($BashPath, $GitPath)) { Require (Test-Path -LiteralPath $program -PathType Leaf) "[SETUP] Missing executable: $program" }
    if (-not (Test-Path -LiteralPath $RunRoot)) { $null = New-Item -ItemType Directory -Path $RunRoot }
    $RunRoot = Safe-Path $RunRoot -MustExist
    Require (Test-Path -LiteralPath $RunRoot -PathType Container) '[SETUP] RunRoot is not a directory'
    $run = Join-Path $RunRoot ('run-' + [Guid]::NewGuid().ToString('N'))
    Require (-not (Test-Path -LiteralPath $run)) '[SETUP] Owned run already exists'
    $null = New-Item -ItemType Directory -Path $run
    $run = Safe-Path $run -MustExist
    Require ($run.StartsWith($RunRoot + '\', [StringComparison]::OrdinalIgnoreCase)) '[SETUP] Resolved owned run escapes RunRoot'
    foreach ($name in @('temp', 'hooks', 'receipts')) { $null = New-Item -ItemType Directory -Path (Owned-Path (Join-Path $run $name)) }
    $childTemp = Safe-Path (Join-Path $run 'temp') -MustExist
    $hooks = Owned-Path (Join-Path $run 'hooks')
    $childPath = (@('C:\msys64\ucrt64\bin', 'C:\Program Files\CMake\bin', 'C:\Program Files\nodejs', (Split-Path -Parent $GitPath), $env:PATH) -join ';')
    Write-Receipt (Join-Path $run 'owner.json') @{ repository = $RepositoryRoot; run_root = $RunRoot; owned_run = $run; retention = 'All artifacts retained; no cleanup or deletion' }
    $sourceHead = Git-Read @('rev-parse', '--verify', 'HEAD') $RepositoryRoot
    $sourceGate = Join-Path $RepositoryRoot 'scripts/verify-gate.sh'
    $gateBytes = [IO.File]::ReadAllBytes($sourceGate)
    $gateHash = (Get-FileHash -LiteralPath $sourceGate -Algorithm SHA256).Hash
    $summary.source_head = $sourceHead
    $summary.gate_sha256 = $gateHash

    $fixture = New-Fixture 'configure-refusal'
    Check-Bash $fixture.root
    $firstResult = Invoke-Child $BashPath @('--noprofile', '--norc', 'scripts/verify-gate.sh', '--strict') $fixture.root
    Check-Refusal $firstResult $fixture
    $firstFiles = @(Gate-Files $fixture.root)
    $firstHashes = @{}
    foreach ($file in $firstFiles) { $firstHashes[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName).Hash }
    $secondResult = Invoke-Child $BashPath @('--noprofile', '--norc', 'scripts/verify-gate.sh', '--strict') $fixture.root
    Check-Refusal $secondResult $fixture
    $secondFiles = @(Gate-Files $fixture.root)
    $firstLog = Configure-Log $firstFiles $fixture.root
    Require ((Get-FileHash -LiteralPath $firstLog.path).Hash -eq $firstHashes[$firstLog.path]) '[ROUTING] Second invocation changed the first configure log'
    $newFiles = @($secondFiles | Where-Object { -not $firstHashes.ContainsKey($_.FullName) })
    $secondLog = Configure-Log $newFiles $fixture.root
    Require ($firstLog.path -ne $secondLog.path -and $firstLog.run -ne $secondLog.run) '[ROUTING] Serial invocations reused a configure log/run path'
    foreach ($path in $firstHashes.Keys) {
        Require ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path).Hash -eq $firstHashes[$path]) '[ROUTING] Second invocation changed an earlier project log'
    }
    Require (@(Get-ChildItem -LiteralPath $childTemp -Recurse -File -Force).Count -eq 0) '[ROUTING] Gate left fallback files in ambient temp'
    $summary.configure_refusals = @{ first = $firstLog; second = $secondLog; sentinel_sha256 = $fixture.sha256 }

    $allocation = New-Fixture 'allocation-refusal'
    Check-Bash $allocation.root
    $obstruction = Owned-Path (Join-Path $allocation.root 'build/verify-gate-logs')
    [IO.File]::WriteAllText($obstruction, 'owned log-parent obstruction', $utf8)
    $obstructionHash = (Get-FileHash -LiteralPath $obstruction).Hash
    $tempBefore = @(Get-ChildItem -LiteralPath $childTemp -Recurse -File -Force | ForEach-Object { $_.FullName })
    $fixtureBefore = @(Get-ChildItem -LiteralPath $allocation.root -Recurse -File -Force | ForEach-Object { $_.FullName })
    $blockedResult = Invoke-Child $BashPath @('--noprofile', '--norc', 'scripts/verify-gate.sh', '--strict') $allocation.root
    $blockedOutput = $blockedResult.stdout + "`n" + $blockedResult.stderr
    Require ($blockedResult.exit_code -eq 2 -and $blockedOutput -match '(?i)(FAIL|error|unable|cannot|failed)' -and
        $blockedOutput -match 'verify-gate-logs') '[ALLOCATION] Missing nonzero project log-allocation refusal'
    Require ((Test-Path -LiteralPath $obstruction -PathType Leaf) -and (Get-FileHash -LiteralPath $obstruction).Hash -eq $obstructionHash) '[ALLOCATION] Gate changed the obstruction'
    Require ((Get-FileHash -LiteralPath $allocation.sentinel).Hash -eq $allocation.sha256) '[ALLOCATION] Gate changed the binary-directory sentinel'
    Require ($blockedOutput -notmatch '(CMake Error|configure failed:|Build \(strict:)') '[ALLOCATION] Gate reached configure after log-allocation failure'
    $tempAfter = @(Get-ChildItem -LiteralPath $childTemp -Recurse -File -Force | ForEach-Object { $_.FullName })
    Require (($tempBefore -join "`n") -eq ($tempAfter -join "`n")) '[ALLOCATION] Gate created fallback temp files'
    $fixtureAfter = @(Get-ChildItem -LiteralPath $allocation.root -Recurse -File -Force | ForEach-Object { $_.FullName })
    Require (($fixtureBefore -join "`n") -eq ($fixtureAfter -join "`n")) '[ALLOCATION] Gate created a fallback file in the fixture'
    $summary.allocation_refusal = @{ exit_code = $blockedResult.exit_code; receipt = $blockedResult.receipt; obstruction_sha256 = $obstructionHash }
    $summary.status = 'PASS'
} catch {
    $summary.status = 'FAIL'
    $summary.failure = $_.Exception.Message
} finally {
    if ($summary.source_head) {
        try {
            Require ((Get-FileHash -LiteralPath $sourceGate -Algorithm SHA256).Hash -eq $gateHash) '[SETUP] Original working gate changed'
            Require ((Git-Read @('rev-parse', '--verify', 'HEAD') $RepositoryRoot) -eq $sourceHead) '[SETUP] Original source HEAD changed'
        } catch { $summary.status = 'FAIL'; $summary.failure = $_.Exception.Message; $summary.source_integrity_failure = $_.Exception.Message }
    }
    if ($run) { Write-Receipt (Join-Path $run 'result.json') $summary }
}

if ($summary.status -ne 'PASS') {
    [Console]::Error.WriteLine("verify_gate_output_tests: $($summary.failure); retained receipts: $run")
    exit 1
}
Write-Output "verify_gate_output_tests: PASS; retained receipts: $run"
exit 0
