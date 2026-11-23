# Scripting/test helper for a running FastPlay: sends commands over WM_COPYDATA (no simulated input) and can save
# window images with PrintWindow (works for off-screen windows too).
#   fpctl.ps1 -Pid <pid> -Command "action fullscreen" | "seek 120" | "hover 300" | "open <path>"
#   fpctl.ps1 -Pid <pid> -Shot out.png [-Popup]     (main window, or the seekbar thumbnail popup)
param([Parameter(Mandatory)][int]$ProcessId, [string]$Command, [string]$Shot, [switch]$Popup, [string]$Class)
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class FpCtl {
  [StructLayout(LayoutKind.Sequential)] struct COPYDATA { public IntPtr dwData; public int cbData; public IntPtr lpData; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, ref COPYDATA l);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new System.Text.StringBuilder(64);
      GetClassName(h, sb, 64); if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static void Send(IntPtr h, string cmd) {
    var cd = new COPYDATA(); cd.dwData = (IntPtr)0x46504143; cd.cbData = (cmd.Length + 1) * 2;
    cd.lpData = Marshal.StringToHGlobalUni(cmd); SendMessage(h, 0x4A, IntPtr.Zero, ref cd); Marshal.FreeHGlobal(cd.lpData);
  }
  public static bool Shot(IntPtr h, string path) {
    if (!IsWindowVisible(h)) return false;
    RECT r; GetWindowRect(h, out r); int w = r.R - r.L, hh = r.B - r.T; if (w <= 0 || hh <= 0) return false;
    using (var bmp = new Bitmap(w, hh)) { using (var g = Graphics.FromImage(bmp)) { var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png); }
    return true;
  }
}
"@
$main = [FpCtl]::Find([uint32]$ProcessId, 'FastPlay.Main')
if ($main -eq [IntPtr]::Zero) { throw "no FastPlay window for pid $ProcessId" }
if ($Command) { [FpCtl]::Send($main, $Command) }
if ($Shot) {
  $h = if ($Class) { [FpCtl]::Find([uint32]$ProcessId, $Class) } elseif ($Popup) { [FpCtl]::Find([uint32]$ProcessId, 'FastPlay.Thumb') } else { $main }
  if (-not [FpCtl]::Shot($h, $Shot)) { Write-Output "window not visible" }
}
