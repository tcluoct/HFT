# winshot.ps1 — capture a top-level window WITH its frame (title bar +
# border) to PNG, for the 附件95-3 report screenshots.
#   winshot.ps1 -Title <substring of window title> -Out <file.png>
#   winshot.ps1 -Stack a.png,b.png,c.png -Out combined.png   (vertical stack)
param([string]$Title, [string]$Out, [string[]]$Stack)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int n);
}
"@
[W]::SetProcessDPIAware() | Out-Null
if ($Stack) {
  $Stack = $Stack -split ','   # -File passes "a.png,b.png" as one string
  $imgs = $Stack | ForEach-Object { [System.Drawing.Image]::FromFile((Resolve-Path $_)) }
  $w = [int]($imgs | Measure-Object Width -Maximum).Maximum
  $gap = 12
  $h = [int](($imgs | Measure-Object Height -Sum).Sum + $gap * ($imgs.Count - 1))
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::White)
  $y = 0; foreach ($i in $imgs) { $g.DrawImage($i, 0, $y, $i.Width, $i.Height); $y += $i.Height + $gap }
  $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $imgs | ForEach-Object { $_.Dispose() }
  "stacked $($Stack.Count) -> $Out ${w}x$h"; exit 0
}
$script:hit = [IntPtr]::Zero
[W]::EnumWindows({ param($h, $l)
  if ([W]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 512; [W]::GetWindowText($h, $sb, 512) | Out-Null
    if ($sb.ToString() -like "*$Title*") { $script:hit = $h; return $false }
  }; return $true }, [IntPtr]::Zero) | Out-Null
if ($script:hit -eq [IntPtr]::Zero) { Write-Error "no visible window titled *$Title*"; exit 1 }
# PrintWindow renders the window itself (PW_RENDERFULLCONTENT), so windows
# stacked on top of it never leak into the capture. The bitmap covers
# GetWindowRect (incl. the invisible resize border); crop to the DWM frame.
$wr = New-Object W+RECT
[W]::GetWindowRect($script:hit, [ref]$wr) | Out-Null
$fr = New-Object W+RECT
[W]::DwmGetWindowAttribute($script:hit, 9, [ref]$fr, 16) | Out-Null   # DWMWA_EXTENDED_FRAME_BOUNDS
$full = New-Object System.Drawing.Bitmap ($wr.R - $wr.L), ($wr.B - $wr.T)
$g = [System.Drawing.Graphics]::FromImage($full)
$dc = $g.GetHdc()
[W]::PrintWindow($script:hit, $dc, 2) | Out-Null
$g.ReleaseHdc($dc); $g.Dispose()
$w = $fr.R - $fr.L; $h = $fr.B - $fr.T
$crop = New-Object System.Drawing.Rectangle ($fr.L - $wr.L), ($fr.T - $wr.T), $w, $h
$bmp = $full.Clone($crop, $full.PixelFormat)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"captured ${w}x$h -> $Out"
