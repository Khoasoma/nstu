$ErrorActionPreference = 'Stop'
$stagePath = Join-Path $PSScriptRoot 'stage-exam-package.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($stagePath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Package stager must parse.' }
foreach ($function in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
$entries = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($path in @('manifest.json','exam/web/index.html','exam/web/app.js','exam/web/styles.css','exam/shared/programming.js')) { $entries.Add($path, 'file') }
$json = '{"id":"cs-package","title":"Programming","durationSeconds":600,"contentMode":"computer_science","programmingVersion":1,"sessionMode":"exam","languages":["cpp20","python3"],"questions":[{"id":"sum","type":"programming","revision":1,"prompt":"Add integers","timeLimitMs":1000,"memoryLimitMiB":64,"samples":[{"id":"sample-1","input":" 2 3\r\n\n","output":"5 \n"}]}]}'
$manifest = $json | ConvertFrom-Json
$result = Test-ExamManifest -Manifest $manifest -EntryKinds $entries
if ($result.Id -cne 'cs-package') { throw 'Valid CS package admission failed.' }
$cases = @(
    @{ From='"programmingVersion":1'; To='"programmingVersion":2' },
    @{ From='"timeLimitMs":1000'; To='"hiddenTests":[],"timeLimitMs":1000' },
    @{ From='"type":"programming"'; To='"type":"essay"' },
    @{ From='"revision":1'; To='"revision":0' },
    @{ From='"memoryLimitMiB":64'; To='"memoryLimitMiB":513' },
    @{ From='"id":"sample-1"'; To='"id":"../private"' }
)
foreach ($case in $cases) {
    $rejected = $false
    try { [void](Test-ExamManifest -Manifest (($json.Replace($case.From, $case.To)) | ConvertFrom-Json) -EntryKinds $entries) }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Unsafe CS manifest was accepted: $($case.To)" }
}
$manifest.questions[0].samples[0].input = 'ế' * 1366
$rejected = $false
try { [void](Test-ExamManifest -Manifest $manifest -EntryKinds $entries) } catch { $rejected = $true }
if (-not $rejected) { throw 'Sample byte budget must reject multibyte UTF-8 overflow.' }
$entries.Add('private-tests.json', 'file')
$rejected = $false
try { [void](Test-ExamManifest -Manifest ($json | ConvertFrom-Json) -EntryKinds $entries) } catch { $rejected = $true }
if (-not $rejected) { throw 'Private or undeclared CS package files must be rejected.' }
$entries.Remove('private-tests.json') | Out-Null
$entries.Add('private-case-count-42', 'directory')
$rejected = $false
try { [void](Test-ExamManifest -Manifest ($json | ConvertFrom-Json) -EntryKinds $entries) } catch { $rejected = $true }
if (-not $rejected) { throw 'Private metadata in CS directory names must be rejected.' }
$entries.Remove('private-case-count-42') | Out-Null
$entries.Remove('exam/shared/programming.js') | Out-Null
$rejected = $false
try { [void](Test-ExamManifest -Manifest ($json | ConvertFrom-Json) -EntryKinds $entries) } catch { $rejected = $true }
if (-not $rejected) { throw 'A CS package without its source runtime must be rejected.' }
Write-Host 'PROGRAMMING PACKAGE TESTS: PASS (real stager admission, private fields/files, revisions, byte budgets and runtime presence)'
