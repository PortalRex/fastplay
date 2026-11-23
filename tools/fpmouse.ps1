# Mouse tests for a FastPlay test copy WITHOUT touching the real cursor or desktop: sends mouse window messages
# straight to one of its windows (found by class), in that window's client coordinates. Nothing goes through
# SendInput or the system input queue, so it works on off-screen test windows and never moves the user's pointer.
#   fpmouse.ps1 -ProcessId <pid> -Class FastPlay.Bar -Do "click 300 20", "drag 100 20 400 20 10", "wheel 50 20 -120"
#   fpmouse.ps1 -ProcessId <pid> -Class FastPlay.EditTimeline -Info      (prints the window's client size)
# Steps: move X Y | down X Y | up X Y | click X Y | dbl X Y | rclick X Y | drag X1 Y1 X2 Y2 [STEPS] | wheel X Y DELTA
#        sleep MS. Append "+ctrl" / "+shift" / "+alt" to a step for modifier flags (MK_CONTROL / MK_SHIFT; Alt is
#        only reflected where the code reads GetKeyState, which these messages cannot fake).
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Class, [string[]]$Do = @(), [switch]$Info)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FpMouse {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  static string Cls(IntPtr h) { var sb = new System.Text.StringBuilder(128); GetClassName(h, sb, 128); return sb.ToString(); }
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p != pid) return true;
      if (Cls(h) == cls && IsWindowVisible(h)) { found = h; return false; }
      EnumChildWindows(h, (c, l2) => { if (Cls(c) == cls && IsWindowVisible(c)) { found = c; return false; } return true; }, IntPtr.Zero);
      return found == IntPtr.Zero;
    }, IntPtr.Zero);
    return found;
  }
  public static IntPtr Pack(int x, int y) { return (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF)); }
}
"@
$h = [FpMouse]::Find([uint32]$ProcessId, $Class)
if ($h -eq [IntPtr]::Zero) { throw "no visible $Class window in pid $ProcessId" }
$rc = New-Object FpMouse+RECT
[void][FpMouse]::GetClientRect($h, [ref]$rc)
if ($Info) { "client {0}x{1}" -f $rc.R, $rc.B }
$WM_MOUSEMOVE = 0x200; $WM_LBUTTONDOWN = 0x201; $WM_LBUTTONUP = 0x202; $WM_LBUTTONDBLCLK = 0x203
$WM_RBUTTONDOWN = 0x204; $WM_RBUTTONUP = 0x205; $WM_MOUSEWHEEL = 0x20A; $MK_LBUTTON = 1; $MK_SHIFT = 4; $MK_CONTROL = 8
function Send([int]$msg, [int]$wp, [int]$x, [int]$y) { [void][FpMouse]::SendMessage($h, $msg, [IntPtr]$wp, [FpMouse]::Pack($x, $y)) }
foreach ($step in $Do) {
    $mods = 0
    if ($step -match '\+ctrl') { $mods = $mods -bor $MK_CONTROL }
    if ($step -match '\+shift') { $mods = $mods -bor $MK_SHIFT }
    $t = ($step -replace '\+\w+', '').Trim() -split '\s+'
    $n = @($t | Select-Object -Skip 1 | ForEach-Object { [int]$_ })
    switch ($t[0]) {
        'move'   { Send $WM_MOUSEMOVE $mods $n[0] $n[1] }
        'down'   { Send $WM_MOUSEMOVE $mods $n[0] $n[1]; Send $WM_LBUTTONDOWN ($mods -bor $MK_LBUTTON) $n[0] $n[1] }
        'up'     { Send $WM_LBUTTONUP $mods $n[0] $n[1] }
        'click'  { Send $WM_MOUSEMOVE $mods $n[0] $n[1]; Send $WM_LBUTTONDOWN ($mods -bor $MK_LBUTTON) $n[0] $n[1]; Send $WM_LBUTTONUP $mods $n[0] $n[1] }
        'dbl'    { Send $WM_LBUTTONDOWN ($mods -bor $MK_LBUTTON) $n[0] $n[1]; Send $WM_LBUTTONUP $mods $n[0] $n[1]
                   Send $WM_LBUTTONDBLCLK ($mods -bor $MK_LBUTTON) $n[0] $n[1]; Send $WM_LBUTTONUP $mods $n[0] $n[1] }
        'rclick' { Send $WM_RBUTTONDOWN $mods $n[0] $n[1]; Send $WM_RBUTTONUP $mods $n[0] $n[1] }
        'drag'   {
            $steps = if ($n.Count -ge 5) { $n[4] } else { 8 }
            Send $WM_MOUSEMOVE $mods $n[0] $n[1]
            Send $WM_LBUTTONDOWN ($mods -bor $MK_LBUTTON) $n[0] $n[1]
            for ($i = 1; $i -le $steps; $i++) {
                $x = [int]($n[0] + ($n[2] - $n[0]) * $i / $steps); $y = [int]($n[1] + ($n[3] - $n[1]) * $i / $steps)
                Send $WM_MOUSEMOVE ($mods -bor $MK_LBUTTON) $x $y
                Start-Sleep -Milliseconds 15
            }
            Send $WM_LBUTTONUP $mods $n[2] $n[3]
        }
        'wheel'  {
            # WM_MOUSEWHEEL carries screen coordinates and the delta in the high word of wParam.
            $pt = New-Object FpMouse+POINT; $pt.X = $n[0]; $pt.Y = $n[1]
            [void][FpMouse]::ClientToScreen($h, [ref]$pt)
            Send $WM_MOUSEWHEEL ((($n[2] -band 0xFFFF) -shl 16) -bor $mods) $pt.X $pt.Y
        }
        'sleep'  { Start-Sleep -Milliseconds $n[0] }
        default  { throw "unknown step '$step'" }
    }
    Start-Sleep -Milliseconds 30
}
