param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Sender", "Receiver")]
    [string]$Mode,
    [string]$Group = "239.192.0.1",
    [int]$Port = 47000,
    [int]$Count = 100,
    [int]$IntervalMilliseconds = 100
)

$ErrorActionPreference = "Stop"
$endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Parse($Group), $Port)
if ($Mode -eq "Sender") {
    $udp = [Net.Sockets.UdpClient]::new()
    try {
        $udp.Ttl = 1
        for ($sequence = 0; $sequence -lt $Count; $sequence++) {
            $payload = [Text.Encoding]::ASCII.GetBytes("NSTU-MATRIX:$sequence")
            [void]$udp.Send($payload, $payload.Length, $endpoint)
            Start-Sleep -Milliseconds $IntervalMilliseconds
        }
    } finally {
        $udp.Dispose()
    }
    exit 0
}

$udp = [Net.Sockets.UdpClient]::new($Port)
try {
    $udp.JoinMulticastGroup([Net.IPAddress]::Parse($Group))
    $udp.Client.ReceiveTimeout = [Math]::Max(5000, $Count * $IntervalMilliseconds * 2)
    $received = [Collections.Generic.HashSet[int]]::new()
    while ($received.Count -lt $Count) {
        $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        try {
            $text = [Text.Encoding]::ASCII.GetString($udp.Receive([ref]$remote))
        } catch [Net.Sockets.SocketException] {
            break
        }
        if ($text -match '^NSTU-MATRIX:(\d+)$') {
            [void]$received.Add([int]$Matches[1])
        }
    }
    $loss = $Count - $received.Count
    Write-Host "received=$($received.Count) sent=$Count lost=$loss"
    if ($loss -ne 0) { exit 1 }
} finally {
    $udp.Dispose()
}
