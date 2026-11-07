[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
try { Add-Type -AssemblyName System.IO.Compression.FileSystem } catch { }
try { Add-Type -AssemblyName System.Security.Cryptography.Pkcs } catch { Add-Type -AssemblyName System.Security }
$stager = Join-Path $PSScriptRoot 'stage-exam-package.ps1'
$projection = Join-Path (Split-Path $PSScriptRoot -Parent) 'tests/exam_origin_package_projection.mjs'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$testId = 'nstu-pkg01-test-' + [guid]::NewGuid().ToString('N')
$root = Join-Path $tempBase $testId
$hostPath = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf)) { $hostPath = Join-Path $PSHOME 'powershell.exe' }
$failures = [Collections.Generic.List[string]]::new()
$passed = 0
$invocationCount = 0
$certificate = $null
$rsa = $null
$createdRoot = $false

function Assert-Contract {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { $script:passed++; Write-Host "PASS: $Message" }
    else { $script:failures.Add($Message); Write-Host "FAIL: $Message" }
}

function New-ArchiveFixture {
    param([string]$Name, [string]$Manifest, [byte[]]$Signature = $null)
    $entries = [Collections.Generic.SortedDictionary[string, byte[]]]::new([StringComparer]::Ordinal)
    $entries.Add('manifest.json', [Text.Encoding]::UTF8.GetBytes($Manifest))
    # Inert local page. The producer manifest bytes are the subject of this boundary test.
    $entries.Add('exam/web/index.html', [Text.Encoding]::UTF8.GetBytes('<!doctype html><title>PKG01 TEST ONLY</title>'))
    if ($null -ne $Signature) { $entries.Add('manifest.p7s', $Signature) }
    $archivePath = Join-Path $root ($Name + '.nstuexam')
    $stream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $zip = $null
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
        foreach ($pair in $entries.GetEnumerator()) {
            $entry = $zip.CreateEntry($pair.Key)
            $output = $entry.Open()
            try { $output.Write($pair.Value, 0, $pair.Value.Length) } finally { $output.Dispose() }
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($pair.Key)
            foreach ($bytes in @([BitConverter]::GetBytes([UInt64]$nameBytes.Length), $nameBytes,
                                 [BitConverter]::GetBytes([UInt64]$pair.Value.Length), $pair.Value)) {
                if ($bytes.Length -gt 0) { [void]$hash.TransformBlock($bytes, 0, $bytes.Length, $bytes, 0) }
            }
        }
        [void]$hash.TransformFinalBlock([byte[]]::new(0), 0, 0)
        $contentHash = ([BitConverter]::ToString($hash.Hash)).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($null -ne $zip) { $zip.Dispose() } else { $stream.Dispose() }
        $hash.Dispose()
    }
    return [pscustomobject]@{ Archive = $archivePath; ContentHash = $contentHash; Manifest = $Manifest;
        ArchiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() }
}

function Invoke-OwnedProcess {
    param([string[]]$Arguments, [ValidateRange(1, 60000)][int]$TimeoutMilliseconds = 30000)
    # Windows PowerShell can suppress native stderr under SilentlyContinue.
    # Capture both process streams directly so boundary assertions see the real error.
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $hostPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = ($Arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $started = $false
    $cleanupClock = $null
    try {
        if (-not $process.Start()) { throw 'Could not start the actual package stager.' }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutMilliseconds)
        # This handle belongs exclusively to the child just started above.
        # Never find or terminate any application by name/PID from external state.
        $cleanupClock = [Diagnostics.Stopwatch]::StartNew()
        if ($timedOut -and -not $process.HasExited) { $process.Kill() }
        if (-not $process.WaitForExit(5000)) { throw 'Owned test child did not exit within its cleanup bound.' }
        $drainBudget = [Math]::Max(0, 5000 - [int]$cleanupClock.ElapsedMilliseconds)
        if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), $drainBudget)) {
            throw 'Owned test child streams did not drain within their cleanup bound.'
        }
        $exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
        # WaitAll proved both tasks complete; Result cannot wait here.
        $output = $stdout.Result + $stderr.Result
        return [pscustomobject]@{ ExitCode = $exitCode; Output = $output; TimedOut = $timedOut; ChildExited = $process.HasExited }
    } finally {
        try {
            # Stream/startup exceptions also clean up only our successfully started child.
            if ($started -and -not $process.HasExited) {
                if ($null -eq $cleanupClock) { $cleanupClock = [Diagnostics.Stopwatch]::StartNew() }
                $process.Kill()
                $remainingCleanup = [Math]::Max(0, 5000 - [int]$cleanupClock.ElapsedMilliseconds)
                if (-not $process.WaitForExit($remainingCleanup)) { throw 'Owned test child cleanup exceeded its bound.' }
            }
        } finally { $process.Dispose() }
    }
}

function Invoke-Fixture {
    param([string]$Name, $Fixture, [string[]]$Extra = @(), [string]$ArchiveHash = '', [string]$ContentHash = '')
    if (-not $ArchiveHash) { $ArchiveHash = $Fixture.ArchiveHash }
    if (-not $ContentHash) { $ContentHash = $Fixture.ContentHash }
    # Keep test paths short: the existing native metadata move uses MAX_PATH.
    $script:invocationCount++
    $publish = Join-Path $root ('p' + $script:invocationCount)
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $stager,
        '-ArchivePath', $Fixture.Archive, '-PublishRoot', $publish,
        '-ExpectedArchiveSha256', $ArchiveHash, '-ExpectedContentSha256', $ContentHash, '-AllowNonElevatedTest') + $Extra
    $result = Invoke-OwnedProcess -Arguments $arguments
    $result | Add-Member -NotePropertyName Publish -NotePropertyValue $publish
    return $result
}

function Assert-NoPublication {
    param($Result, [string]$Name)
    $leftovers = @(Get-ChildItem -LiteralPath $Result.Publish -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '.staging-*' -or $_.Name -like '*.nstu-package.json' -or $_.Name -eq 'manifest.json' })
    Assert-Contract ($leftovers.Count -eq 0) "$Name refuses publication and leaves no transient package"
}

function New-TestOnlySignature {
    param([string]$Manifest)
    $cms = [Security.Cryptography.Pkcs.SignedCms]::new(
        [Security.Cryptography.Pkcs.ContentInfo]::new([Text.Encoding]::UTF8.GetBytes($Manifest)), $true)
    $signer = [Security.Cryptography.Pkcs.CmsSigner]::new($certificate)
    $signer.IncludeOption = [Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $cms.ComputeSignature($signer, $true)
    return ,$cms.Encode()
}

try {
    if (-not [BitConverter]::IsLittleEndian) { throw 'Package digest fixture requires little-endian Windows.' }
    New-Item -ItemType Directory -Path $root -ErrorAction Stop | Out-Null
    $createdRoot = $true
    [IO.File]::WriteAllText((Join-Path $root '.pkg01-owner'), $testId, [Text.UTF8Encoding]::new($false))
    $probeScript = Join-Path $root 'timeout-probe.ps1'
    $probeMarker = Join-Path $root 'timeout-probe.started'
    [IO.File]::WriteAllText($probeScript, @'
param([string]$MarkerPath)
[IO.File]::WriteAllText($MarkerPath, 'PKG01 TEST ONLY')
[Console]::Out.WriteLine('PKG01 TEST ONLY child started')
Start-Sleep -Seconds 30
'@, [Text.UTF8Encoding]::new($false))
    try {
        $probeClock = [Diagnostics.Stopwatch]::StartNew()
        $probe = Invoke-OwnedProcess -Arguments @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', $probeScript, '-MarkerPath', $probeMarker) -TimeoutMilliseconds 3000
        $probeClock.Stop()
        Assert-Contract ($probe.TimedOut -and $probe.ExitCode -eq 124 -and $probe.ChildExited) 'harmless owned child times out and is stopped'
        Assert-Contract ($probeClock.ElapsedMilliseconds -lt 9000) 'owned child timeout and stream cleanup stay bounded'
        Assert-Contract ((Test-Path -LiteralPath $probeMarker -PathType Leaf) -and $probe.Output.Contains('PKG01 TEST ONLY child started')) 'owned child drains real output after timeout'
    } finally {
        # Exact files created in this exclusive test root, never derived from child output.
        [IO.File]::Delete($probeScript)
        [IO.File]::Delete($probeMarker)
    }
    Assert-Contract (-not (Test-Path -LiteralPath $probeScript) -and -not (Test-Path -LiteralPath $probeMarker)) 'owned timeout fixture resources are removed'
    $fixtureText = & node.exe $projection
    if ($LASTEXITCODE -ne 0) { throw 'Actual Studio projection fixture generation failed.' }
    $fixtures = $fixtureText | ConvertFrom-Json
    foreach ($case in $fixtures.cases) {
        $fixture = New-ArchiveFixture -Name $case.name -Manifest $case.manifest
        $result = Invoke-Fixture -Name $case.name -Fixture $fixture -Extra @('-AllowUnsigned')
        Assert-Contract (($result.ExitCode -eq 0) -eq $case.accepted) "$($case.name) [$($case.kind)] expected accepted=$($case.accepted); exit=$($result.ExitCode)"
        if ($case.accepted -and $result.ExitCode -eq 0) {
            $metadata = $result.Output | ConvertFrom-Json
            $manifestPath = Join-Path $metadata.published_root 'manifest.json'
            $expectedBytes = [Text.Encoding]::UTF8.GetBytes($case.manifest)
            Assert-Contract ([Convert]::ToBase64String([IO.File]::ReadAllBytes($manifestPath)) -ceq [Convert]::ToBase64String($expectedBytes)) "$($case.name) preserves manifest and origin bytes/order"
            Assert-Contract ($metadata.content_sha256 -ceq $fixture.ContentHash) "$($case.name) retains the exact content digest"
        } elseif (-not $case.accepted) { Assert-NoPublication -Result $result -Name $case.name }
        if (($result.ExitCode -eq 0) -ne $case.accepted) { Write-Host $result.Output }
    }

    # Memory-only nonproduction fixture: no private-key export and no certificate-store registration.
    $rsa = [Security.Cryptography.RSACng]::new(2048)
    if (-not $rsa.Key.IsEphemeral) { throw 'Test-only signing key must remain ephemeral.' }
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=NSTU PKG01 TEST ONLY', $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $certificate = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-1), [DateTimeOffset]::UtcNow.AddHours(1))
    $signature = New-TestOnlySignature -Manifest $fixtures.signedManifest
    $signed = New-ArchiveFixture -Name 'signed-test-only' -Manifest $fixtures.signedManifest -Signature $signature
    $signedResult = Invoke-Fixture -Name 'signed-valid' -Fixture $signed -Extra @('-TrustedPublisherThumbprint', $certificate.Thumbprint)
    Assert-Contract ($signedResult.ExitCode -eq 0) 'signed origin projection accepts the explicitly configured test-only public thumbprint'
    if ($signedResult.ExitCode -eq 0) {
        $signedMetadata = $signedResult.Output | ConvertFrom-Json
        Assert-Contract $signedMetadata.signature_verified 'signed package records verified signature'
        Assert-Contract ([Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $signedMetadata.published_root 'manifest.json'))) -ceq [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($fixtures.signedManifest))) 'signed projection preserves the exact signed manifest bytes'
    }
    $tampered = New-ArchiveFixture -Name 'signed-tampered' -Manifest ($fixtures.signedManifest.Replace('Package origin contract test', 'Altered package origin test')) -Signature $signature
    $invalidOrigin = New-ArchiveFixture -Name 'invalid-origin-signed' -Manifest $fixtures.invalidOriginManifest -Signature ([byte[]](1, 2, 3, 4))
    $invalidSignature = New-ArchiveFixture -Name 'invalid-signature' -Manifest $fixtures.signedManifest -Signature ([byte[]](1, 2, 3, 4))
    $unsigned = New-ArchiveFixture -Name 'unsigned-default' -Manifest $fixtures.signedManifest
    foreach ($securityCase in @(
        @{ Name='signed-wrong-publisher'; Fixture=$signed; Extra=@('-TrustedPublisherThumbprint', ('0' * 40)); Error='not the configured publisher' },
        @{ Name='signed-no-publisher'; Fixture=$signed; Extra=@(); Error='Thumbprint' },
        @{ Name='signed-tampered-manifest'; Fixture=$tampered; Extra=@('-TrustedPublisherThumbprint', $certificate.Thumbprint); Error='signature verification failed' },
        @{ Name='unsigned-default-refused'; Fixture=$unsigned; Extra=@(); Error='manifest.p7s is missing' },
        @{ Name='manifest-before-signature'; Fixture=$invalidOrigin; Extra=@('-TrustedPublisherThumbprint', $certificate.Thumbprint); Error='allowed origin' },
        @{ Name='content-digest-refused'; Fixture=$unsigned; Extra=@('-AllowUnsigned'); ContentHash=('1' * 64); Error='content SHA-256' },
        @{ Name='archive-before-manifest'; Fixture=$invalidOrigin; Extra=@(); ArchiveHash=('0' * 64); Error='archive SHA-256' },
        @{ Name='unsigned-with-configured-invalid-signature'; Fixture=$invalidSignature; Extra=@('-AllowUnsigned','-TrustedPublisherThumbprint',$certificate.Thumbprint); Error='signature verification failed' }
    )) {
        $result = Invoke-Fixture -Name $securityCase.Name -Fixture $securityCase.Fixture -Extra $securityCase.Extra `
            -ArchiveHash $securityCase.ArchiveHash -ContentHash $securityCase.ContentHash
        Assert-Contract ($result.ExitCode -ne 0 -and $result.Output -match [regex]::Escape($securityCase.Error)) "$($securityCase.Name) fails at the existing validation boundary"
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch [regex]::Escape($securityCase.Error)) { Write-Host $result.Output }
        Assert-NoPublication -Result $result -Name $securityCase.Name
    }
    foreach ($difference in $fixtures.producerNativeDifferences) { Write-Host "CONTRACT: $difference" }
    if ($failures.Count -gt 0) { throw "NSTU origin package contract failed: $($failures.Count) assertions; $passed passed." }
    Write-Host "NSTU origin package contract passed: $passed assertions. Test-only signatures do not establish production publisher trust or candidate enforcement."
} finally {
    if ($null -ne $certificate) { $certificate.Dispose() }
    if ($null -ne $rsa) { $rsa.Dispose() }
    if ($createdRoot -and (Test-Path -LiteralPath $root)) {
        $resolvedRoot = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\', '/')
        $marker = Join-Path $resolvedRoot '.pkg01-owner'
        if (-not $resolvedRoot.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedRoot) -cne $testId -or
            -not (Test-Path -LiteralPath $marker -PathType Leaf) -or [IO.File]::ReadAllText($marker) -cne $testId -or
            ((Get-Item -LiteralPath $resolvedRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Refusing cleanup of unowned or out-of-bounds package fixture directory.'
        }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
exit 0
