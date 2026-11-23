# Opens each file in a hidden, muted FastPlay test copy and checks: first frame, what libmpv reports against ffprobe,
# a seek to the middle, Edit mode (probe + keyframe index) and a 5 s keyframe-cut export. Exports go to -OutDir (never
# next to the media; the media files are only read). One line of results per file, plus the CSV.
#   powershell -NoProfile -File tools\media-test.ps1 -Files a.mov,b.mkv [-OutDir H:\tmp\fp-media] [-NoEdit]
param([Parameter(Mandatory)][string[]]$Files, [string]$OutDir = (Join-Path $env:TEMP 'fastplay-media-test'),
      [switch]$NoEdit, [switch]$KeepLogs, [int]$OpenTimeoutSec = 30)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MtFg {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
}
"@
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'dist\fastplay.exe'
$fpctl = Join-Path $PSScriptRoot 'fpctl.ps1'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$csv = Join-Path $OutDir 'results.csv'

function Probe([string]$f) {
    if ((Get-Item -LiteralPath $f).Length -eq 0) { return $null }
    $j = & ffprobe -v error -show_entries 'format=duration,format_name:stream=codec_type,codec_name,width,height' -of json -- $f 2>$null | Out-String
    if (-not $j) { return $null }
    $o = $j | ConvertFrom-Json
    $v = @($o.streams | Where-Object codec_type -eq 'video')[0]
    $a = @($o.streams | Where-Object codec_type -eq 'audio')[0]
    [pscustomobject]@{ dur = [double]$o.format.duration; fmt = $o.format.format_name
        v = $(if ($v) { "$($v.codec_name) $($v.width)x$($v.height)" } else { '' }); a = $(if ($a) { $a.codec_name } else { '' }) }
}

$results = @()
foreach ($f in $Files) {
    $r = [ordered]@{ file = $f; size_gb = [math]::Round((Get-Item -LiteralPath $f).Length / 1GB, 2); probe = ''; first_ms = -1
        mpv = ''; dur_ok = ''; seek = ''; edit = ''; export = ''; errors = '' }
    $pr = Probe $f
    if ($pr) { $r.probe = "$($pr.fmt) | $($pr.v) | $($pr.a) | $([math]::Round($pr.dur, 1)) s" }
    $log = Join-Path $OutDir ("log-{0}.txt" -f [guid]::NewGuid().ToString('N'))
    $exportDir = Join-Path $OutDir ([IO.Path]::GetFileNameWithoutExtension($f))
    New-Item -ItemType Directory -Force $exportDir | Out-Null
    $env:FASTPLAY_TEST = '1'; $env:FASTPLAY_TIMING = $log; $env:FASTPLAY_EDIT_OUT = $exportDir
    $p = Start-Process -FilePath $exe -ArgumentList "`"$f`"", '--start=0' -PassThru
    $env:FASTPLAY_TEST = $null; $env:FASTPLAY_TIMING = $null; $env:FASTPLAY_EDIT_OUT = $null
    $stole = $false
    # Waits until a log line matches (and the player still runs), watching the foreground the whole time.
    function WaitLog([string]$pattern, [double]$secs, [int]$after = 0) {
        $deadline = (Get-Date).AddSeconds($secs)
        while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
            $fp = 0; [void][MtFg]::GetWindowThreadProcessId([MtFg]::GetForegroundWindow(), [ref]$fp)
            if ($fp -eq $p.Id) { $script:stole = $true; Stop-Process -Id $p.Id -Force; return $null }
            if (Test-Path $log) {
                $lines = @(Get-Content $log)
                for ($i = $after; $i -lt $lines.Count; $i++) { if ($lines[$i] -match $pattern) { return @{ line = $lines[$i]; n = $i + 1 } } }
            }
            Start-Sleep -Milliseconds 100
        }
        return $null
    }
    function Ctl([string]$c) {
        if ($p.HasExited) { throw "player exited (code $($p.ExitCode)) before '$c'`n$(Get-Content $log -Raw)" }
        & $fpctl -ProcessId $p.Id -Command $c | Out-Null
    }
    function Count { if (Test-Path $log) { @(Get-Content $log).Count } else { 0 } }
    function Get([string]$prop) {
        $n = Count; Ctl "get $prop"
        $m = WaitLog ("property " + [regex]::Escape($prop) + " = (.*)$") 5 $n
        if ($m -and $m.line -match ' = (.*)$') { $matches[1] } else { '?' }
    }
    try {
        $first = WaitLog 'first frame|load error' $OpenTimeoutSec
        if (-not $first) { $r.errors += 'no first frame; ' }
        elseif ($first.line -match 'load error') { $r.errors += 'load error; ' }
        else { $r.first_ms = [double]($first.line -split "`t")[1] }
        if ($first -and $first.line -notmatch 'load error') {
            Ctl 'action pause'  # short files would otherwise end (and maybe close) before the checks are done
            # FastPlay's own length (the seekbar's): libmpv's "duration" is partial for fragmented MP4 opened via mfra.
            $n = Count; Ctl 'state'
            $m = WaitLog 'state: .*duration ([\d.]+)' 5 $n
            $dur = if ($m -and $m.line -match 'duration ([\d.]+)') { [double]$matches[1] } else { -1 }
            $r.mpv = "{0} | {1} | hw {2} | {3}" -f (Get 'file-format'), (Get 'video-codec'), (Get 'hwdec-current'), (Get 'audio-codec-name')
            if ($pr -and $pr.dur -gt 0) {
                $r.dur_ok = if ([math]::Abs($dur - $pr.dur) -le [math]::Max(0.5, $pr.dur * 0.002)) { "ok ($([math]::Round($dur,1)) s)" } else { "MISMATCH mpv $dur vs probe $($pr.dur)" }
            }
            $target = [math]::Round(($(if ($pr -and $pr.dur -gt 0) { $pr.dur } else { $dur })) / 2, 1)
            $n = Count; $t0 = Get-Date
            Ctl "seek $target"
            $m = WaitLog 'playback restart at' 15 $n
            if ($m -and $m.line -match 'at ([\d.]+)') {
                $r.seek = "{0} s in {1} ms" -f [double]$matches[1], [int]((Get-Date) - $t0).TotalMilliseconds
                if ([math]::Abs([double]$matches[1] - $target) -gt 1) { $r.errors += "seek landed at $($matches[1]) for $target; " }
            } else { $r.errors += 'seek: no restart; ' }

            if (-not $NoEdit) {
                $n = Count; $t0 = Get-Date
                Ctl 'action edit_mode'
                $state = $null
                for ($i = 0; $i -lt 120; $i++) {
                    $n2 = Count; Ctl 'editstate'
                    $m = WaitLog 'edit: active' 5 $n2
                    if ($m -and $m.line -match 'media 1' -and ($m.line -notmatch 'keyframes 0,' -or $pr.v -eq '' -or $i -gt 40)) { $state = $m.line; break }
                    Start-Sleep -Milliseconds 250
                }
                if ($state -and $state -match 'keyframes (\d+)') {
                    $r.edit = "probe+index {0} ms, {1} keyframes" -f [int]((Get-Date) - $t0).TotalMilliseconds, $matches[1]
                    $a = [math]::Round($target * 0.8, 1)
                    Ctl "seek $a"; [void](WaitLog 'playback restart at' 10 (Count))
                    Ctl 'edit setstart'
                    Ctl ("seek {0}" -f ($a + 5)); [void](WaitLog 'playback restart at' 10 (Count))
                    Ctl 'edit setend'
                    Ctl 'editset cut_mode keyframe'
                    $t0 = Get-Date
                    Ctl 'edit export'
                    $done = $null
                    for ($i = 0; $i -lt 240; $i++) {
                        Start-Sleep -Milliseconds 500
                        $n2 = Count; Ctl 'editstate'
                        $m = WaitLog 'edit: active' 5 $n2
                        if ($m -and $m.line -match 'job 0' -and $i -gt 1) { $done = $m.line; break }
                    }
                    $out = @(Get-ChildItem -LiteralPath $exportDir -File | Where-Object Name -notlike 'log-*')
                    $status = ($done -split 'status: ')[1]
                    if ($out.Count -eq 1 -and $out[0].Length -eq 0) {
                        $r.export = "EMPTY output $($out[0].Name); status: $status"
                        $r.errors += 'export empty; '
                    } elseif ($out.Count -eq 1) {
                        $op = Probe $out[0].FullName
                        $r.export = "{0} in {1} ms: {2} s, {3}" -f $out[0].Name, [int]((Get-Date) - $t0).TotalMilliseconds, $(if ($op) { [math]::Round($op.dur, 2) } else { '?' }), $(if ($op) { $op.v } else { 'unreadable' })
                    } else {
                        $r.export = "no single output ($($out.Count)); state: $(($done -split 'status: ')[1])"
                        $r.errors += 'export; '
                    }
                } else { $r.errors += "edit mode did not load: $state; " }
            }
        }
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        if ($stole) { $r.errors += 'FOCUS STOLEN; ' }
        if ((Test-Path $log) -and -not $KeepLogs) { Remove-Item $log }
    }
    $o = [pscustomobject]$r
    $results += $o
    $o | Format-List | Out-String | Write-Output
    if ($stole) { throw 'A test copy took the foreground; stopping.' }
}
$results | Export-Csv -NoTypeInformation $csv
