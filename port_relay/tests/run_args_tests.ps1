# run_args_tests.ps1 - self test for parameterized startup (ASCII only, on purpose:
# Windows PowerShell reads .ps1 in the local ANSI codepage, so keep it ASCII-safe).
#   covers: named args / --rule spec / multi-rule in one process / config file /
#           default config auto-load / UDP named args / --key=value / error paths
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot       # port_relay dir
$relay = Join-Path $root 'portrelay.exe'
$echo  = Join-Path $PSScriptRoot 'echo.exe'
if (!(Test-Path $relay) -or !(Test-Path $echo)) {
  Write-Host '[FAIL] missing portrelay.exe or tests\echo.exe' -ForegroundColor Red
  exit 1
}

$procs = @()
$pass = 0
$fail = 0
function Start-Bg([string[]]$argList) {
  if ($argList -and $argList.Count -gt 0) {
    $p = Start-Process -FilePath $relay -ArgumentList $argList -PassThru -WindowStyle Hidden
  } else {
    $p = Start-Process -FilePath $relay -PassThru -WindowStyle Hidden   # no-arg launch
  }
  $script:procs += $p
  return $p
}
function Check([string]$name, [bool]$ok) {
  if ($ok) { Write-Host ("[PASS] " + $name) -ForegroundColor Green; $script:pass++ }
  else     { Write-Host ("[FAIL] " + $name) -ForegroundColor Red;   $script:fail++ }
}
function Test-Tcp([int]$port, [int]$size = 65536) {
  try {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $port)
    $st = $c.GetStream()
    $payload = New-Object byte[] $size
    (New-Object Random($port)).NextBytes($payload)
    $st.Write($payload, 0, $payload.Length)
    $st.Flush()
    $ms = New-Object IO.MemoryStream
    $buf = New-Object byte[] 8192
    $total = 0
    while ($total -lt $size) {
      $n = $st.Read($buf, 0, [Math]::Min($buf.Length, $size - $total))
      if ($n -le 0) { break }
      $ms.Write($buf, 0, $n)
      $total += $n
    }
    $ok = ($total -eq $size) -and
          ([Convert]::ToBase64String($payload) -eq [Convert]::ToBase64String($ms.ToArray()))
    $c.Close()
    return $ok
  } catch { return $false }
}
function Test-Udp([int]$port, [int]$size = 1500) {
  try {
    $u = New-Object Net.Sockets.UdpClient('127.0.0.1', $port)
    $u.Client.ReceiveTimeout = 3000
    $d = New-Object byte[] $size
    (New-Object Random($port)).NextBytes($d)
    $null = $u.Send($d, $d.Length)
    $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
    $r = $u.Receive([ref]$ep)
    $ok = ($r.Length -eq $d.Length) -and
          ([Convert]::ToBase64String($d) -eq [Convert]::ToBase64String($r))
    $u.Close()
    return $ok
  } catch { return $false }
}

$confFile = Join-Path $root 'tests\args_test.conf'
$autoConf = Join-Path $root 'portrelay.conf'
try {
  # backend echo servers: TCP 47102 + UDP 47103
  Start-Process -FilePath $echo -ArgumentList @('tcp', '47102') -PassThru -WindowStyle Hidden | ForEach-Object { $procs += $_ }
  Start-Process -FilePath $echo -ArgumentList @('udp', '47103') -PassThru -WindowStyle Hidden | ForEach-Object { $procs += $_ }
  Start-Sleep -Milliseconds 700

  Write-Host '== 0) legacy positional form still works ==' -ForegroundColor Cyan
  Start-Bg @('tcp', '47117', '127.0.0.1', '47102', '127.0.0.1', '-b', '256') | Out-Null
  Start-Sleep -Milliseconds 500
  Check 'legacy positional relay' (Test-Tcp 47117)

  Write-Host '== 1) named args -M/-l/-t ==' -ForegroundColor Cyan
  Start-Bg @('-M', 'tcp', '-l', '47101', '-t', '127.0.0.1:47102', '-v') | Out-Null
  Start-Sleep -Milliseconds 500
  Check 'named args relay' (Test-Tcp 47101)

  Write-Host '== 2) --rule spec ==' -ForegroundColor Cyan
  Start-Bg @('--rule', 'tcp:47103->127.0.0.1:47102') | Out-Null
  Start-Sleep -Milliseconds 500
  Check 'rule spec relay' (Test-Tcp 47103)

  Write-Host '== 3) multi-rule in one process ==' -ForegroundColor Cyan
  Start-Bg @('--rule', 'tcp:127.0.0.1:47105->127.0.0.1:47102',
             '--rule', 'tcp:127.0.0.1:47106->127.0.0.1:47102') | Out-Null
  Start-Sleep -Milliseconds 500
  Check 'multi-rule #1 relay' (Test-Tcp 47105)
  Check 'multi-rule #2 relay' (Test-Tcp 47106)

  Write-Host '== 4) config file (-f: positional line + key=value + comments) ==' -ForegroundColor Cyan
  @(
    '# args_test.conf - test only',
    '; semicolon comment',
    'tcp 47107 127.0.0.1 47102 127.0.0.1 -b 256    # positional form',
    '# each config line must be a COMPLETE rule (lines are flushed as they are read),',
    'rule = tcp:127.0.0.1:47109->127.0.0.1:47102   # key=value form = one complete rule',
    '# (a bare "target = host:port" line is NOT valid: every line must be one complete rule)',
    'verbose = off'
  ) | Set-Content -Path $confFile -Encoding ASCII
  Start-Bg @('-f', $confFile) | Out-Null
  Start-Sleep -Milliseconds 500
  Check 'config positional line' (Test-Tcp 47107)
  Check 'config key=value lines' (Test-Tcp 47109)

  Write-Host '== 5) command line --key=value ==' -ForegroundColor Cyan
  Start-Bg @('--mode=tcp', '--listen-port=47111', '--target=127.0.0.1:47102') | Out-Null
  Start-Sleep -Milliseconds 500
  Check '--key=value form' (Test-Tcp 47111)

  Write-Host '== 6) no args: auto-load portrelay.conf next to exe ==' -ForegroundColor Cyan
  @('rule = tcp:127.0.0.1:47113->127.0.0.1:47102') | Set-Content -Path $autoConf -Encoding ASCII
  Start-Bg @() | Out-Null
  Start-Sleep -Milliseconds 700
  Check 'default config auto-load' (Test-Tcp 47113)

  Write-Host '== 7) UDP named args ==' -ForegroundColor Cyan
  Start-Bg @('-M', 'udp', '-l', '47115', '-t', '127.0.0.1:47103', '-u', '30', '-m', '64') | Out-Null
  Start-Sleep -Milliseconds 500
  Check 'UDP named args relay' (Test-Udp 47115)

  Write-Host '== 8) help / version / error paths ==' -ForegroundColor Cyan
  # error-path cases write to stderr on purpose; native stderr must not abort the run
  $ErrorActionPreference = 'Continue'
  $out = (& $relay -h 2>&1 | Out-String)
  Check 'help exit code 0' ($LASTEXITCODE -eq 0)
  Check 'help shows options' ($out -match '--rule')
  $out = (& $relay -V 2>&1 | Out-String)
  Check 'version 1.2.0' ($out -match '1\.2\.0')
  $out = (& $relay --badopt 2>&1 | Out-String)
  Check 'unknown option rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'badopt'))
  $out = (& $relay -M tcp -l 9000 -t 10.0.0.1 2>&1 | Out-String)
  Check 'target without port rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'host:port'))
  $out = (& $relay --mode tcp -l 9000 2>&1 | Out-String)
  Check 'incomplete rule rejected' (($LASTEXITCODE -ne 0) -and ($out -match '\[relay\] error'))
  $out = (& $relay -M ftp -l 9000 -t 1.2.3.4:80 2>&1 | Out-String)
  Check 'bad mode rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'ftp'))
  $out = (& $relay tcp 9000 1.2.3.4:80 2>&1 | Out-String)
  Check 'non numeric target port rejected' (($LASTEXITCODE -ne 0) -and ($out -match '1\.2\.3\.4:80'))
  $out = (& $relay --rule 'bad-spec' 2>&1 | Out-String)
  Check 'bad rule spec rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'bad-spec'))
  $out = (& $relay -f no_such_file.conf 2>&1 | Out-String)
  Check 'missing config rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'no_such_file\.conf')); $badConf = Join-Path $PSScriptRoot 'args_bad.conf'; @('tcp 47118 127.0.0.1 47102 127.0.0.1','target = 1.2.3.4:80') | Set-Content -Path $badConf -Encoding ASCII; $out = (& $relay -f $badConf 2>&1 | Out-String); Check 'incomplete config line rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'args_bad\.conf')); Remove-Item $badConf -Force -ErrorAction SilentlyContinue
  if (Test-Path $autoConf) { Remove-Item $autoConf -Force -ErrorAction SilentlyContinue }; $out = (& $relay 2>&1 | Out-String)
  Check 'no args without default config rejected' (($LASTEXITCODE -ne 0) -and ($out -match 'portrelay\.conf'))

  Write-Host ''
  if ($fail -eq 0) {
    Write-Host ("== ALL ARGS TESTS PASSED ({0}) ==" -f $pass) -ForegroundColor Green
    exit 0
  } else {
    Write-Host ("== {0} PASSED / {1} FAILED ==" -f $pass, $fail) -ForegroundColor Red
    exit 1
  }
} finally {
  foreach ($p in $procs) {
    if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  }
  if (Test-Path $autoConf) { Remove-Item $autoConf -Force -ErrorAction SilentlyContinue }
  if (Test-Path $confFile) { Remove-Item $confFile -Force -ErrorAction SilentlyContinue }
}
