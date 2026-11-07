# Boot the game (autostart), then save window captures of the game window only (PrintWindow).
# Usage: powershell -File tools\capture_run.ps1 -Prefix name -Times 14,15,16,17 [-Seconds 20]
param([string]$Prefix = "shot", [double[]]$Times = @(14, 16, 18), [string]$OutDir = "$PSScriptRoot\..\screenshots\runs", [switch]$NoAutostart, [string]$Buttons = "")
$sig = @'
using System; using System.Runtime.InteropServices;
public class MTWin { [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags); }
'@
Add-Type $sig; Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
$root = Resolve-Path "$PSScriptRoot\.."
if (-not $NoAutostart) { $env:MT_AUTOSTART = "1" }; if ($Buttons) { $env:MT_INPUT = $Buttons }; $env:MT_LOG = "$root\build_assets\run_capture.log"
$p = Start-Process -FilePath "$root\build\MarioTennisRecompiled.exe" -WorkingDirectory $root -PassThru
Remove-Item Env:MT_AUTOSTART, Env:MT_LOG, Env:MT_INPUT -ErrorAction SilentlyContinue
$start = Get-Date
foreach ($t in $Times) {
  $wait = $t - ((Get-Date) - $start).TotalSeconds; if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  $p.Refresh(); if ($p.HasExited) { "exited before ${t}s"; break }
  $h = $p.MainWindowHandle; $r = New-Object MTWin+R; [MTWin]::GetClientRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap $r.Rt, $r.B; $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
  [MTWin]::PrintWindow($h, $hdc, 3) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save("$OutDir\${Prefix}_${t}s.png"); $bmp.Dispose(); "saved ${Prefix}_${t}s.png"
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
