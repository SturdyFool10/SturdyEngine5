# Temporary diagnostic harness: launches UiWorkbench, drives a real Windows modal
# resize loop (SC_SIZE + arrow keys, so the mouse is not hijacked), then exits.
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cls, string win);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int left, top, right, bottom; }
}
'@

$exe = "C:\Projects\sturdyengine5\build\x86-64\win\relwithdebinfo\bin\UiWorkbench.exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory "C:\Projects\sturdyengine5" -PassThru
Start-Sleep -Seconds 6

$hwnd = [IntPtr]::Zero
foreach ($p in (Get-Process -Id $proc.Id)) { $hwnd = $p.MainWindowHandle }
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NO WINDOW"; $proc.Kill(); exit 1 }

$r = New-Object W+RECT
[void][W]::GetWindowRect($hwnd, [ref]$r)
Write-Output ("hwnd={0} rect={1},{2},{3},{4}" -f $hwnd, $r.left, $r.top, $r.right, $r.bottom)

[void][W]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 500

# WM_SYSCOMMAND = 0x112, SC_SIZE = 0xF000, WMSZ_RIGHT = 2 -> enter modal resize on right edge.
Write-Output "MODAL-ENTER $(Get-Date -Format 'HH:mm:ss.fff')"
[void][W]::PostMessage($hwnd, 0x112, [IntPtr]0xF002, [IntPtr]0)
Start-Sleep -Milliseconds 700

# Drag the right edge with arrow keys: each keypress moves the modal resize rect.
for ($i = 0; $i -lt 60; $i++) {
    [W]::keybd_event(0x27, 0, 0, [UIntPtr]::Zero)   # VK_RIGHT down
    [W]::keybd_event(0x27, 0, 2, [UIntPtr]::Zero)   # up
    Start-Sleep -Milliseconds 40
}
Start-Sleep -Milliseconds 800
for ($i = 0; $i -lt 60; $i++) {
    [W]::keybd_event(0x25, 0, 0, [UIntPtr]::Zero)   # VK_LEFT
    [W]::keybd_event(0x25, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
}
Write-Output "MODAL-COMMIT $(Get-Date -Format 'HH:mm:ss.fff')"
[W]::keybd_event(0x0D, 0, 0, [UIntPtr]::Zero)       # VK_RETURN commits the modal resize
[W]::keybd_event(0x0D, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Seconds 2
Write-Output "DONE $(Get-Date -Format 'HH:mm:ss.fff')"
$proc.Kill()
