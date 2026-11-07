# Launch to the launcher, then do a list of clicks (client coords) with captures in between.
# Usage: & tools\click_capture.ps1 -Prefix name -Steps "1450,843;shot;400,100;shot"
param([string]$Exe = "build", [string]$WorkDir = "", [string]$Prefix = "ui", [string]$Steps = "shot", [string]$OutDir = "$PSScriptRoot\..\screenshots\runs")
$sig = @'
using System; using System.Runtime.InteropServices;
public class MTC { [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
[StructLayout(LayoutKind.Sequential)] public struct P { public int X,Y; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, IntPtr e);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags); }
'@
Add-Type $sig; Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
$root = Resolve-Path "$PSScriptRoot\.."
$p = Start-Process -FilePath "$root\$Exe\MarioTennisRecompiled.exe" -WorkingDirectory $(if ($WorkDir) { $WorkDir } else { $root }) -PassThru
Start-Sleep -Seconds 5
$i = 0
foreach ($s in $Steps.Split(';')) {
  $p.Refresh(); if ($p.HasExited) { "exited"; break }
  $h = $p.MainWindowHandle
  if ($s -eq 'shot') {
    $r = New-Object MTC+R; [MTC]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap $r.Rt, $r.B; $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [MTC]::PrintWindow($h, $hdc, 3) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save("$OutDir\${Prefix}_$i.png"); $bmp.Dispose(); "saved ${Prefix}_$i.png"; $i++
  } else {
    $xy = $s.Split(','); $pt = New-Object MTC+P; $pt.X = [int]$xy[0]; $pt.Y = [int]$xy[1]
    [MTC]::SetForegroundWindow($h) | Out-Null; [MTC]::ClientToScreen($h, [ref]$pt) | Out-Null
    [MTC]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 150
    [MTC]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60; [MTC]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
  }
  Start-Sleep -Milliseconds 1200
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }


