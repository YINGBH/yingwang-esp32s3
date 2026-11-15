# Start the web server (if not running) and open the browser.
$ErrorActionPreference = "SilentlyContinue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$portFile = Join-Path $here "port.txt"

function Test-Port([int]$p) {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect("127.0.0.1", $p)
        $c.Close()
        return $true
    } catch { return $false }
}

# Already running?
$port = 8767
if (Test-Path $portFile) {
    $saved = (Get-Content $portFile -ErrorAction SilentlyContinue)
    if ($saved -match '^\d+$') { $port = [int]$saved }
}
if (Test-Port $port) {
    Start-Process ("http://localhost:" + $port + "/")
    exit 0
}

# Start the server hidden
Start-Process -FilePath "powershell.exe" `
    -ArgumentList "-NoProfile","-ExecutionPolicy","Bypass","-WindowStyle","Hidden","-File","`"$here\serve.ps1`"" `
    -WindowStyle Hidden

# Wait until the port is up (serve.ps1 picks 8767..8776 and writes port.txt)
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    $p = $port
    if (Test-Path $portFile) {
        $saved = (Get-Content $portFile -ErrorAction SilentlyContinue)
        if ($saved -match '^\d+$') { $p = [int]$saved }
    }
    if (Test-Port $p) {
        Start-Process ("http://localhost:" + $p + "/")
        exit 0
    }
}

# Still not up: show a message instead of failing silently
Add-Type -AssemblyName PresentationFramework
[System.Windows.MessageBox]::Show("Web server did not start.`n`nTry running serve.ps1 manually to see the error.", "YingWang") | Out-Null
exit 1
