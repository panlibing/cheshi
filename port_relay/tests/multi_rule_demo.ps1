# multi_rule_demo.ps1 - 验证"一次进程内多条转发规则(多端口)"是否可用
# 启动一个 echo tcp 47102，再启动 portrelay 两条规则：47105->47102、47106->47102
$root  = Split-Path -Parent $PSScriptRoot
$relay = Join-Path $root 'portrelay.exe'
$echo  = Join-Path $PSScriptRoot 'echo.exe'

$ep = Start-Process -FilePath $echo -ArgumentList 'tcp', '47102' -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 600
$rp = Start-Process -FilePath $relay -ArgumentList '--rule', 'tcp:127.0.0.1:47105->127.0.0.1:47102', '--rule', 'tcp:127.0.0.1:47106->127.0.0.1:47102' -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1000

foreach ($port in 47105, 47106) {
  try {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', $port)
    $s = $c.GetStream()
    $d = [Text.Encoding]::ASCII.GetBytes("hello-from-$port")
    $s.Write($d, 0, $d.Length)
    $b = New-Object byte[] 64
    $n = $s.Read($b, 0, 64)
    Write-Host ("listen $port -> echo returned: " + [Text.Encoding]::ASCII.GetString($b, 0, $n))
    $c.Close()
  } catch {
    Write-Host "listen $port FAILED: $($_.Exception.Message)"
  }
}

Stop-Process -Id $rp.Id -Force
Stop-Process -Id $ep.Id -Force
Write-Host 'demo done'
