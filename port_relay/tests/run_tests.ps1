# run_tests.ps1 - end-to-end self test for portrelay
# TCP: 128KB random payload relayed and compared byte-by-byte
# UDP: multiple datagrams relayed and compared
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot        # port_relay dir
$relay = Join-Path $root 'portrelay.exe'
$echo  = Join-Path $PSScriptRoot 'echo.exe'
if (!(Test-Path $relay) -or !(Test-Path $echo)) {
  Write-Host '[FAIL] executables missing - build portrelay.exe and tests\echo.exe first' -ForegroundColor Red
  exit 1
}

$procs = @()
function Start-Bg([string]$exePath, [string[]]$argList) {
  $p = Start-Process -FilePath $exePath -ArgumentList $argList -PassThru -WindowStyle Hidden
  $script:procs += $p
  return $p
}
try {
  $allPass = $true

  # ---------------- TCP test ----------------
  Write-Host '== TCP: relay(47001) -> echo(47002) ==' -ForegroundColor Cyan
  Start-Bg $echo @('tcp','47002') | Out-Null
  Start-Bg $relay @('tcp','47001','127.0.0.1','47002','127.0.0.1','-b','512') | Out-Null
  Start-Sleep -Milliseconds 800

  $payload = New-Object byte[] (128 * 1024)
  (New-Object Random(42)).NextBytes($payload)

  $client = $null
  try {
    $client = New-Object Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', 47001)
    $st = $client.GetStream()
    $st.Write($payload, 0, $payload.Length)
    $st.Flush()
    $ms = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 65536
    $total = 0
    while ($total -lt $payload.Length) {
      $n = $st.Read($buf, 0, [Math]::Min($buf.Length, $payload.Length - $total))
      if ($n -le 0) { break }
      $ms.Write($buf, 0, $n)
      $total += $n
    }
    $recvBytes = $ms.ToArray()
    $ok = ($total -eq $payload.Length) -and
          ([Convert]::ToBase64String($payload) -eq [Convert]::ToBase64String($recvBytes))
    Write-Host ("TCP echoed {0}/{1} bytes, byte-identical: {2}" -f $total, $payload.Length, $ok)
    if (!$ok) { $allPass = $false }
  } catch { Write-Host "[FAIL] TCP client exception: $_" -ForegroundColor Red; $allPass = $false }
  finally { if ($client) { $client.Close() } }

  # ---------------- UDP test ----------------
  Write-Host '== UDP: relay(47004) -> echo(47003) ==' -ForegroundColor Cyan
  Start-Bg $echo @('udp','47003') | Out-Null
  Start-Bg $relay @('udp','47004','127.0.0.1','47003','127.0.0.1','-u','30','-b','512') | Out-Null
  Start-Sleep -Milliseconds 800

  $u = $null
  try {
    $u = New-Object Net.Sockets.UdpClient('127.0.0.1', 47004)
    $u.Client.ReceiveTimeout = 3000
    $udpOk = $true
    for ($i = 0; $i -lt 3; $i++) {
      $dgram = New-Object byte[] (2000 + $i * 777)
      (New-Object Random(1000 + $i)).NextBytes($dgram)
      $null = $u.Send($dgram, $dgram.Length)
      $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
      $r = $u.Receive([ref]$ep)
      $same = ($r.Length -eq $dgram.Length) -and
              ([Convert]::ToBase64String($dgram) -eq [Convert]::ToBase64String($r))
      Write-Host ("UDP dgram#{0}: sent {1}B, got {2}B, identical: {3} (from {4})" -f $i, $dgram.Length, $r.Length, $same, $ep)
      if (!$same) { $udpOk = $false }
    }
    if (!$udpOk) { $allPass = $false }
  } catch { Write-Host "[FAIL] UDP client exception: $_" -ForegroundColor Red; $allPass = $false }
  finally { if ($u) { $u.Close() } }

  # ---------------- unit tests ----------------
  Write-Host '== unit tests: pure functions + config-parsing fuzz ==' -ForegroundColor Cyan
  $unit = Join-Path $PSScriptRoot 'unit_tests.exe'
  if (Test-Path $unit) {
    & $unit
    if ($LASTEXITCODE -ne 0) { $allPass = $false }
  } else {
    Write-Host '[FAIL] unit_tests.exe missing - build it via tests\build.bat' -ForegroundColor Red
    $allPass = $false
  }

  # ---------------- summary ----------------
  if ($allPass) {
    Write-Host '== ALL TESTS PASSED ==' -ForegroundColor Green
    exit 0
  } else {
    Write-Host '== SOME TESTS FAILED ==' -ForegroundColor Red
    exit 1
  }
} finally {
  foreach ($p in $procs) {
    if (!$p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
}
