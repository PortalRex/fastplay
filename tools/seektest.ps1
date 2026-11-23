# Opens a file in a test-mode FastPlay and times a series of seeks issued over the command channel.
param([Parameter(Mandatory)][string]$File, [string]$MpvOpts = '', [double]$Start = 0, [string[]]$Commands)
$log = Join-Path $env:TEMP ("fp-seek-{0}.txt" -f [guid]::NewGuid().ToString('N'))
$env:FASTPLAY_TEST = '1'; $env:FASTPLAY_TIMING = $log; $env:FASTPLAY_MPV_OPTS = $MpvOpts
$p = Start-Process -FilePath (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\fastplay.exe') -ArgumentList "`"$File`"", "--start=$Start" -PassThru
Remove-Item Env:FASTPLAY_TEST, Env:FASTPLAY_TIMING, Env:FASTPLAY_MPV_OPTS
try {
  for ($i = 0; $i -lt 300 -and -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'first frame' -Quiet)); $i++) { Start-Sleep -Milliseconds 50 }
  Start-Sleep -Milliseconds 800
  foreach ($c in $Commands) {
    & (Join-Path $PSScriptRoot 'fpctl.ps1') -ProcessId $p.Id -Command $c
    Start-Sleep -Milliseconds 2500
  }
} finally { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Get-Content $log | Select-String 'loadfile|file loaded|first frame|restart at|command'; Remove-Item $log
