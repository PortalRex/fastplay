# Time-to-first-frame: FastPlay vs MPC-HC, measured identically by fpbench (Windows.Graphics.Capture, off-screen,
# muted, never focused). Reads the media files only; never writes to them.
#
# MPC-HC runs as a portable copy (bench\mpc-hc, made from your installed MPC-HC) whose mpc-hc64.ini mirrors your
# registry settings (renderer, LAV, ...), so your own MPC-HC settings and history are never touched.
#
#   powershell -NoProfile -File tools\bench\run-bench.ps1 -WarmFile <big.mp4> [-WarmStart 600] [-Runs 5]
#              [-ColdFiles <a.mp4>,<b.mp4>,...] [-ColdStart 120] [-MpcDir "H:\Portalable Programs\MPC-HC"]
#
# Warm: the same file and position over and over (OS file cache hot after the first run; first runs are dropped).
# Cold: each run opens a file nobody has read since boot/cache eviction, alternating players, one file per run.
param(
    [string]$WarmFile, [double]$WarmStart = 600, [int]$Runs = 5,
    [string[]]$ColdFiles = @(), [double]$ColdStart = 120,
    [string]$MpcDir = 'H:\Portalable Programs\MPC-HC',
    [string]$Out = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'bench\results'),
    [int]$MeasureSecs = 10
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$bench = Join-Path $PSScriptRoot 'fpbench.exe'
$fast = Join-Path $root 'dist\fastplay.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csv = Join-Path $Out "bench-$stamp.csv"
$frames = Join-Path $Out "frames-$stamp"
New-Item -ItemType Directory -Force $frames | Out-Null

# ---------------------------------------------------------------- portable MPC-HC with the user's settings
$mpc = Join-Path $root 'bench\mpc-hc'
if (-not (Test-Path (Join-Path $mpc 'mpc-hc64.exe'))) {
    Write-Output "Copying MPC-HC from $MpcDir to $mpc (portable test copy)"
    robocopy $MpcDir $mpc /E /NFL /NDL /NJH /NJS /NP | Out-Null
}
$skip = '^(MediaHistory|PlaylistHistory|Favorites|Recent|Dialogs|Capture|DVBConfiguration)'
$base = 'HKCU:\Software\MPC-HC\MPC-HC'
$lines = New-Object System.Collections.Generic.List[string]
$keys = @(Get-Item $base) + @(Get-ChildItem $base -Recurse)
foreach ($k in $keys) {
    $rel = $k.Name.Substring($k.Name.IndexOf('MPC-HC\MPC-HC') + 13).TrimStart('\')
    if ($rel -eq '' -or $rel -match $skip) { continue }
    $vals = @()
    foreach ($n in $k.GetValueNames()) {
        if ($n -eq '') { continue }
        $kind = $k.GetValueKind($n)
        $v = $k.GetValue($n)
        if ($kind -eq 'DWord' -or $kind -eq 'QWord') { $vals += "$n=$v" }  # signed, as GetProfileInt reads it
        elseif ($kind -eq 'String' -or $kind -eq 'ExpandString') { $vals += "$n=$v" }
        # binary values (window rects, column widths) are left out: MPC-HC uses its defaults for those
    }
    if ($vals.Count) { $lines.Add("[$rel]"); $lines.AddRange([string[]]$vals); $lines.Add('') }
}
# Test-copy overrides: no history writes, no resume, muted, no update checks.
$ini = ($lines -join "`r`n")
foreach ($kv in @('KeepHistory=0', 'RememberFilePos=0', 'Mute=1', 'UpdaterAutoCheck=0', 'RememberPlaylistItems=0')) {
    $name = $kv.Split('=')[0]
    if ($ini -match "(?m)^$name=.*$") { $ini = $ini -replace "(?m)^$name=.*$", $kv }
    else { $ini = $ini -replace '\[Settings\]', "[Settings]`r`n$kv" }
}
[IO.File]::WriteAllText((Join-Path $mpc 'mpc-hc64.ini'), $ini, [Text.Encoding]::Unicode)
$mpcExe = Join-Path $mpc 'mpc-hc64.exe'

function Quote([string]$a) { '"' + $a.Replace('"', '\"') + '"' }

# fpbench is started through WMI (Win32_Process.Create), hidden. A process started that way has no foreground rights,
# and neither do the players it launches, so Windows refuses any attempt to activate them (MPC-HC tries even with
# /nofocus when it inherits the rights of a foreground parent). fpbench still kills a player that takes the foreground.
function Run-One([string]$label, [string]$file, [double]$start, [int]$measure) {
    if ($label -like 'fastplay*') {
        $a = @('--label', $label, '--exe', $fast, '--file', $file, '--args', "{file} --start=$start", '--env', 'FASTPLAY_TEST=1')
    } else {
        $ms = [int]($start * 1000)
        $a = @('--label', $label, '--exe', $mpcExe, '--file', $file, '--args', "/new /nofocus /start $ms {file}")
    }
    $a += @('--runs', '1', '--csv', $csv, '--save-frame', $frames, '--measure', "$measure", '--timeout', '60')
    $cmd = (Quote $bench) + ' ' + (($a | ForEach-Object { Quote $_ }) -join ' ')
    $startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = $cmd; CurrentDirectory = $PSScriptRoot; ProcessStartupInformation = $startup }
    if ($r.ReturnValue -ne 0) { throw "could not start fpbench ($($r.ReturnValue))" }
    Wait-Process -Id $r.ProcessId -ErrorAction SilentlyContinue
    $last = Import-Csv $csv | Select-Object -Last 1
    Write-Output ("{0,-20} first frame {1,7} ms  window {2,5} ms  {3}" -f $last.label, $last.first_frame_ms, $last.window_ms, $last.status)
    if ($last.status -like '*foreground*') { throw "ABORTED: $label took the foreground (it was killed). No further launches." }
}

# ---------------------------------------------------------------- warm: interleaved, first run of each dropped
if ($WarmFile) {
    Write-Output "== warm: $WarmFile @ $WarmStart s"
    for ($i = 0; $i -le $Runs; $i++) {
        $m = if ($i -eq $Runs) { $MeasureSecs } else { 0 }
        Run-One ("fastplay-warm" + $(if ($i -eq 0) { '-first' } else { '' })) $WarmFile $WarmStart $m
        Start-Sleep -Milliseconds 1500
        Run-One ("mpchc-warm" + $(if ($i -eq 0) { '-first' } else { '' })) $WarmFile $WarmStart $m
        Start-Sleep -Milliseconds 1500
    }
}

# ---------------------------------------------------------------- cold: one untouched file per run, alternating
$i = 0
foreach ($f in $ColdFiles) {
    $label = if ($i % 2 -eq 0) { 'fastplay-cold' } else { 'mpchc-cold' }
    Write-Output "== cold ($label): $f @ $ColdStart s"
    Run-One $label $f $ColdStart 0
    Start-Sleep -Milliseconds 1500
    $i++
}

# ---------------------------------------------------------------- summary
$rows = Import-Csv $csv
Write-Output ''
Write-Output "Results: $csv"
$rows | Group-Object label | ForEach-Object {
    $t = @($_.Group | Where-Object { [double]$_.first_frame_ms -ge 0 } | ForEach-Object { [double]$_.first_frame_ms } | Sort-Object)
    $med = if ($t.Count) { if ($t.Count % 2) { $t[[int][math]::Floor($t.Count / 2)] } else { ($t[$t.Count / 2 - 1] + $t[$t.Count / 2]) / 2 } } else { -1 }
    $res = $_.Group | Where-Object { [double]$_.cpu_pct -ge 0 } | Select-Object -Last 1
    '{0,-20} n={1,-2} median {2,6:N0} ms  min {3,6:N0}  max {4,6:N0}{5}' -f $_.Name, $t.Count, $med, ($t | Select-Object -First 1), ($t | Select-Object -Last 1),
        $(if ($res) { "   playing: cpu {0}%  private {1} MB  working set {2} MB" -f $res.cpu_pct, $res.private_mb, $res.working_mb } else { '' })
}
