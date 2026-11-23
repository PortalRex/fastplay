# Mouse-driven UI test on a hidden, muted FastPlay test copy (FASTPLAY_TEST=1), using window messages only
# (tools\fpmouse.ps1): never the real cursor, keyboard or desktop. Checks every step through properties / editstate.
#   powershell -NoProfile -File tools\ui-mouse-test.ps1 -File <long video> [-Out <folder for test exports>]
param([Parameter(Mandatory)][string]$File, [string]$Out = (Join-Path $env:TEMP 'fastplay-ui-test'))
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class UiT {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
}
"@
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'ui-log.txt'
Remove-Item $log -ErrorAction SilentlyContinue
$env:FASTPLAY_TEST = '1'; $env:FASTPLAY_TIMING = $log; $env:FASTPLAY_EDIT_OUT = $Out
$p = Start-Process (Join-Path $root 'dist\fastplay.exe') -ArgumentList "`"$File`"", '--start=600' -PassThru
$env:FASTPLAY_TEST = $null; $env:FASTPLAY_TIMING = $null; $env:FASTPLAY_EDIT_OUT = $null
$script:fails = 0
function Check([string]$what, [bool]$ok, [string]$detail) {
    if (-not $ok) { $script:fails++ }
    "{0}  {1,-44} {2}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what, $detail
}
function Guard {
    $fp = 0; [void][UiT]::GetWindowThreadProcessId([UiT]::GetForegroundWindow(), [ref]$fp)
    if ($fp -eq $p.Id) { Stop-Process -Id $p.Id -Force; throw 'test copy took the foreground: killed' }
    $r = New-Object UiT+RECT; [void][UiT]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $right = [UiT]::GetSystemMetrics(76) + [UiT]::GetSystemMetrics(78)  # virtual screen right edge
    if ($r.L -lt $right) { Stop-Process -Id $p.Id -Force; throw "test copy moved on screen (x=$($r.L)): killed" }
}
function C($c) { & (Join-Path $PSScriptRoot 'fpctl.ps1') -ProcessId $p.Id -Command $c; Start-Sleep -Milliseconds 300 }
function M($cls, [string[]]$d) { & (Join-Path $PSScriptRoot 'fpmouse.ps1') -ProcessId $p.Id -Class $cls -Do $d; Start-Sleep -Milliseconds 600; Guard }
function Prop($name) { C "get $name"; ((Get-Content $log | Select-String "property $name = " | Select-Object -Last 1).Line -split ' = ')[1] }
function Ed { C 'editstate'; ((Get-Content $log | Select-String 'edit: active' | Select-Object -Last 1).Line -split "`t")[2] }
function Segs { [regex]::Matches((Ed), '\[([\d.]+)-([\d.]+)( off)?\]') | ForEach-Object { [pscustomobject]@{ a = [double]$_.Groups[1].Value; b = [double]$_.Groups[2].Value } } }
try {
    Start-Sleep 3
    $p.Refresh(); Guard
    C 'action pause'
    C 'state'
    $dur = [double](Get-Content $log | Select-String 'state: .*duration ([\d.]+)' | Select-Object -Last 1).Matches[0].Groups[1].Value
    $bar = & (Join-Path $PSScriptRoot 'fpmouse.ps1') -ProcessId $p.Id -Class FastPlay.Bar -Info
    $bw = [int](($bar -split ' ')[1] -split 'x')[0]
    $l = 12; $r = $bw - 12  # seekbar ends (S(4) inside the 8 px side margins at 96 dpi)
    $tAt = { param($x) ($x - $l) / ($r - $l) * $dur }

    # ---------------------------------------------------------------- View mode
    M 'FastPlay.Bar' "click $([int](($l + $r) / 2)) 10"
    $t = [double](Prop 'time-pos'); Check 'seekbar click (middle)' ([math]::Abs($t - $dur / 2) -lt $dur * 0.01) "$t s"
    M 'FastPlay.Bar' "drag 200 10 $([int]($l + ($r - $l) * 0.8)) 10 12"
    $t = [double](Prop 'time-pos'); Check 'seekbar drag to 80 %' ([math]::Abs($t - $dur * 0.8) -lt $dur * 0.01) "$t s"
    M 'FastPlay.Bar' 'click 19 34'; $a = Prop 'pause'
    M 'FastPlay.Bar' 'click 19 34'; $b = Prop 'pause'
    Check 'play button toggles' ($a -eq 'no' -and $b -eq 'yes') "$a then $b"
    M 'FastPlay.Bar' "drag $($bw - 18) 34 $($bw - 58) 34 6"
    $v = [double](Prop 'volume'); Check 'volume slider drag' ($v -gt 20 -and $v -lt 70) "$v"
    M 'FastPlay.Video' 'click 600 400'; Start-Sleep -Milliseconds 600
    Check 'click on video plays' ((Prop 'pause') -eq 'no') ''
    M 'FastPlay.Video' 'wheel 600 400 120'
    Check 'wheel up on video: volume' ([double](Prop 'volume') -gt $v) ''
    M 'FastPlay.Video' 'click 600 400'; Start-Sleep -Milliseconds 600
    $r0 = New-Object UiT+RECT; [void][UiT]::GetWindowRect($p.MainWindowHandle, [ref]$r0)
    M 'FastPlay.Video' 'dbl 600 400'; Start-Sleep -Milliseconds 500
    $r1 = New-Object UiT+RECT; [void][UiT]::GetWindowRect($p.MainWindowHandle, [ref]$r1)
    M 'FastPlay.Video' 'dbl 600 400'; Start-Sleep -Milliseconds 500
    $r2 = New-Object UiT+RECT; [void][UiT]::GetWindowRect($p.MainWindowHandle, [ref]$r2)
    Check 'double-click: fullscreen and back' (($r1.R - $r1.L) -gt ($r0.R - $r0.L) -and $r2.L -eq $r0.L -and ($r2.R - $r2.L) -eq ($r0.R - $r0.L)) "$($r0.R - $r0.L) -> $($r1.R - $r1.L) -> $($r2.R - $r2.L), x $($r0.L) -> $($r2.L)"
    C 'action pause'

    # ---------------------------------------------------------------- Edit mode
    C 'action edit_mode'; Start-Sleep 2; Guard
    $tl = & (Join-Path $PSScriptRoot 'fpmouse.ps1') -ProcessId $p.Id -Class FastPlay.EditTimeline -Info
    $tw = [int](($tl -split ' ')[1] -split 'x')[0]; $th = [int](($tl -split 'x')[1])
    $sy = $th - 30  # segment row
    $ts = { param($x) $x / $tw * $dur }
    M 'FastPlay.EditTimeline' "click $([int]($tw / 2)) 40"
    $t = [double](Prop 'time-pos'); Check 'timeline click seeks' ([math]::Abs($t - $dur / 2) -lt $dur * 0.01) "$t s"
    M 'FastPlay.EditTimeline' "drag 300 $sy 500 $sy 10"
    $s = @(Segs); Check 'drag on empty row creates a segment' ($s.Count -eq 1 -and [math]::Abs($s[0].a - (& $ts 300)) -lt $dur * 0.01 -and [math]::Abs($s[0].b - (& $ts 500)) -lt $dur * 0.01) "$($s | ForEach-Object { "$($_.a)-$($_.b)" })"
    M 'FastPlay.EditTimeline' "drag 400 $sy 600 $sy 10"
    $s2 = @(Segs); Check 'drag segment body moves it' ($s2.Count -eq 1 -and [math]::Abs(($s2[0].a - $s[0].a) - (& $ts 200)) -lt $dur * 0.01 -and [math]::Abs(($s2[0].b - $s2[0].a) - ($s[0].b - $s[0].a)) -lt 0.01) "$($s2[0].a)-$($s2[0].b)"
    $re = [int]($s2[0].b / $dur * $tw)
    M 'FastPlay.EditTimeline' "drag $re $sy $($re + 100) $sy 10"
    $s3 = @(Segs); Check 'drag right edge resizes' ([math]::Abs($s3[0].b - (& $ts ($re + 100))) -lt $dur * 0.01 -and [math]::Abs($s3[0].a - $s2[0].a) -lt 0.01) "$($s3[0].a)-$($s3[0].b)"
    $le = [int]($s3[0].a / $dur * $tw)
    M 'FastPlay.EditTimeline' "drag $le $sy $($le - 50) $sy 10"
    $s4 = @(Segs); Check 'drag left edge resizes' ([math]::Abs($s4[0].a - (& $ts ($le - 50))) -lt $dur * 0.01 -and [math]::Abs($s4[0].b - $s3[0].b) -lt 0.01) "$($s4[0].a)-$($s4[0].b)"
    M 'FastPlay.EditTimeline' "drag 1100 $sy 1200 $sy 10"
    Check 'second segment by drag' (@(Segs).Count -eq 2) ''
    C 'edit undo'; Check 'undo removes it' (@(Segs).Count -eq 1) ''
    C 'edit redo'; Check 'redo brings it back' (@(Segs).Count -eq 2) ''
    M 'FastPlay.EditTimeline' "click $([int]((($s4[0].a + $s4[0].b) / 2) / $dur * $tw)) $sy"
    Check 'click segment selects it' ((Ed) -match 'selected 0') ''
    M 'FastPlay.EditTimeline' "wheel 700 60 120", "wheel 700 60 120"
    M 'FastPlay.EditTimeline' "click 200 40"  # zoom keeps the time under the cursor (x 700) fixed; x 200 moves
    $t = [double](Prop 'time-pos'); $tz = & $ts 200
    Check 'wheel zooms around the cursor' ([math]::Abs($t - $tz) -gt 5) "x 200 -> $t s (unzoomed $([math]::Round($tz,1)) s)"
    $cards = & (Join-Path $PSScriptRoot 'fpmouse.ps1') -ProcessId $p.Id -Class FastPlay.EditSegmentCards -Info
    M 'FastPlay.EditSegmentCards' 'click 60 75'
    Check 'segment card click selects the second' ((Ed) -match 'selected 1') "cards $cards"
    C 'action edit_mode'; Start-Sleep 1; Guard
    $r3 = New-Object UiT+RECT; [void][UiT]::GetWindowRect($p.MainWindowHandle, [ref]$r3)
    Check 'back to View mode, still off-screen' ($r3.L -eq $r0.L) "x $($r3.L)"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
"$script:fails failure(s)"
