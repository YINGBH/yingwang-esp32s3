# YingWang board - local web server for the USB serial page.
# Pure PowerShell, no install, serves files from this script's folder.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Try 8767 first, then 8768..8776 if busy.
$listener = $null
$port = 0
foreach ($p in 8767..8776) {
    try {
        $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $p)
        $l.Start()
        $listener = $l
        $port = $p
        break
    } catch { }
}
if ($null -eq $listener) {
    Write-Host "No free port between 8767 and 8776."
    exit 1
}

Write-Host ("Server started on port " + $port)
Set-Content -Path (Join-Path $root "port.txt") -Value $port -Encoding ASCII

$crlf = [string][char]13 + [string][char]10

while ($true) {
    $client = $listener.AcceptTcpClient()
    try {
        $client.ReceiveTimeout = 5000
        $stream = $client.GetStream()
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::ASCII)
        $requestLine = $reader.ReadLine()
        if ($null -eq $requestLine -or $requestLine -eq "") { $client.Close(); continue }

        $path = "/index.html"
        $parts = $requestLine.Split(" ")
        if ($parts.Length -ge 2) { $path = $parts[1] }
        $path = $path.Split("?")[0]
        if ($path -eq "/" -or $path -eq "") { $path = "/index.html" }

        $file = Join-Path $root ($path.TrimStart("/"))
        $body = [byte[]]@()
        $status = "404 Not Found"
        if (Test-Path $file -PathType Leaf) {
            $body = [System.IO.File]::ReadAllBytes($file)
            $status = "200 OK"
        }

        $ctype = "text/html; charset=utf-8"
        if ($path.EndsWith(".js"))  { $ctype = "text/javascript; charset=utf-8" }
        if ($path.EndsWith(".css")) { $ctype = "text/css; charset=utf-8" }
        if ($path.EndsWith(".png")) { $ctype = "image/png" }
        if ($path.EndsWith(".ico")) { $ctype = "image/x-icon" }

        $header = "HTTP/1.1 " + $status + $crlf
        $header = $header + "Content-Type: " + $ctype + $crlf
        $header = $header + "Content-Length: " + $body.Length + $crlf
        $header = $header + "Connection: close" + $crlf + $crlf
        $hb = [System.Text.Encoding]::UTF8.GetBytes($header)
        $stream.Write($hb, 0, $hb.Length)
        if ($body.Length -gt 0) { $stream.Write($body, 0, $body.Length) }
        $stream.Flush()
    } catch {
    } finally {
        $client.Close()
    }
}
