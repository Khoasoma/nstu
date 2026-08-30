param(
    [int]$DurationMinutes = 60,
    [int]$SampleSeconds = 5,
    [string]$OutputPath = ".\nstu-benchmarks.csv"
)

$ErrorActionPreference = "Stop"
if ($DurationMinutes -lt 1 -or $SampleSeconds -lt 1) {
    throw "DurationMinutes and SampleSeconds must be positive."
}
$samples = [Collections.Generic.List[object]]::new()
$end = [DateTimeOffset]::Now.AddMinutes($DurationMinutes)
$previous = @{}
while ([DateTimeOffset]::Now -lt $end) {
    $timestamp = [DateTimeOffset]::Now
    foreach ($process in Get-Process -Name "nstu-server", "nstu-service", "nstu-agent" -ErrorAction SilentlyContinue) {
        $key = "$($process.Id)"
        $cpu = $process.TotalProcessorTime.TotalSeconds
        $cpuPercent = 0.0
        if ($previous.ContainsKey($key)) {
            $elapsed = ($timestamp - $previous[$key].Timestamp).TotalSeconds
            if ($elapsed -gt 0) {
                $cpuPercent = (($cpu - $previous[$key].Cpu) / $elapsed) * 100.0
            }
        }
        $previous[$key] = @{ Timestamp = $timestamp; Cpu = $cpu }
        $samples.Add([pscustomobject]@{
            Timestamp = $timestamp.ToString("o")
            Process = $process.ProcessName
            Pid = $process.Id
            CpuPercentOneCore = [Math]::Round($cpuPercent, 2)
            WorkingSetBytes = $process.WorkingSet64
            PrivateBytes = $process.PrivateMemorySize64
            Handles = $process.HandleCount
            Threads = $process.Threads.Count
        })
    }
    Start-Sleep -Seconds $SampleSeconds
}
$samples | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding utf8
Write-Host "Benchmark samples written to $OutputPath"
