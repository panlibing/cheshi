# diag.ps1 - narrow down which listen-host form fails, capture relay logs
$root  = Split-Path -Parent $PSScriptRoot
$relay = Join-Path $root 'portrelay.exe'
$echo  = Join-Path $PSScriptRoot 'echo.exe'
$procs = @()
function Probe([string]$name, [int]$port, [string[]]$argList) {
  $o = Join-Path $env:TEMP ("relay_" + $name + ".out")
  $e = Join-Path $env:TEMP ("relay_" + $name + ".err")
  if (Test-Path $o) { Remove-Item $o -Force }
  if (Test-Path $e) { Remove-Item $e -Force }
  $p = Start-Process -FilePath $relay -ArgumentList $argList -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $o -RedirectStandardError $e
  $script:procs += $p
  Start-Sleep -Milliseconds 700
  $ok = $false
  try {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $port)
    $ok = $true
    $c.Close()
  } catch { $ok = $false }
  $state = if ($p.HasExited) { "exited($($p.ExitCode))" } else { "running" }
  Write-Host ("--- {0}: connect={1} proc={2} args=[{3}]" -f $name, $ok, $state, ($argList -join ' '))
  if (Test-Path $o) { Get-Content $o | ForEach-Object { Write-Host ("    out| " + $_) } }
  if (Test-Path $e) { Get-Content $e | ForEach-Object { Write-Host ("    err| " + $_) } }
}
try {
  Start-Process -FilePath $echo -ArgumentList @('tcp','47102') -PassThru -WindowStyle Hidden | ForEach-Object { $procs += $_ }
  Start-Sleep -Milliseconds 600
  Probe 'A_legacy_explicit' 47131 @('tcp','47131','127.0.0.1','47102','127.0.0.1')
  Probe 'B_legacy_allif'    47132 @('tcp','47132','127.0.0.1','47102')
  Probe 'C_named_explicit'  47133 @('-M','tcp','-l','47133','-t','127.0.0.1:47102','-lh','127.0.0.1')
  Probe 'D_named_allif'     47134 @('-M','tcp','-l','47134','-t','127.0.0.1:47102')
} finally {
  foreach ($p in $procs) { if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
}
