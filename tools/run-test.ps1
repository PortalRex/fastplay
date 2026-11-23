# Starts fastplay in test mode (off-screen, muted, never activated) on a file, waits for it to exit after its first
# frame (--bench-exit), kills it if it ever takes the foreground, and prints its timing log.
#   powershell -NoProfile -File tools\run-test.ps1 -File <video> [-Start 600] [-MpvOpts "name=value;..."] [-Keep]
param([Parameter(Mandatory)][string]$File, [double]$Start = -1, [string]$MpvOpts = '', [int]$TimeoutSec = 30,
      [switch]$Keep)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Fg {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
}
"@
$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $env:TEMP ("fastplay-timing-{0}.txt" -f [guid]::NewGuid().ToString('N'))
$env:FASTPLAY_TEST = '1'
$env:FASTPLAY_TIMING = $log
$env:FASTPLAY_MPV_OPTS = $MpvOpts
$args2 = @("`"$File`"")
if (-not $Keep) { $args2 += '--bench-exit' }
if ($Start -ge 0) { $args2 += "--start=$Start" }
$p = Start-Process -FilePath (Join-Path $root 'dist\fastplay.exe') -ArgumentList $args2 -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$stole = $false
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
  $fpid = 0; [void][Fg]::GetWindowThreadProcessId([Fg]::GetForegroundWindow(), [ref]$fpid)
  if ($fpid -eq $p.Id) { $stole = $true; Stop-Process -Id $p.Id -Force; break }
  Start-Sleep -Milliseconds 20
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "TIMEOUT (killed pid $($p.Id))" }
if ($stole) { Write-Output "FOCUS STOLEN - killed" }
if (Test-Path $log) { Get-Content $log; Remove-Item $log }
Remove-Item Env:FASTPLAY_TEST, Env:FASTPLAY_TIMING, Env:FASTPLAY_MPV_OPTS -ErrorAction SilentlyContinue
