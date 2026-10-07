# Sends real input to the game window for tests that need the player to act (fire, press a key).
#
#   .\tools\input.ps1 -Click                 # left mouse click (fire)
#   .\tools\input.ps1 -Click -HoldMs 800     # hold the button (automatic weapons)
#   .\tools\input.ps1 -Key 0x2E              # press a key (virtual-key code; 0x2E = Delete)
param(
	[switch]$Click,
	[switch]$RightClick,  # hold the right button (aim down sights)
	[switch]$Alt,  # hold Alt while pressing the key (Alt+F9: ShadowPlay recording)
	[int]$HoldMs = 60,
	[int]$Key = 0,
	[int]$Times = 1
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class F4Input {
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
	[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
	[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
	[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
	[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
'@

$game = Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { throw 'Fallout 4 is not running' }
# Windows only lets the foreground process change the foreground window; a tapped Alt lifts that.
[F4Input]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[F4Input]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
[F4Input]::keybd_event(0x12, 0, 0x0002, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 400

for ($i = 0; $i -lt $Times; $i++) {
	if ($Click) {
		[F4Input]::SetCursorPos(600, 400) | Out-Null  # over the game window, not whatever is beside it
		[F4Input]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # left down
		Start-Sleep -Milliseconds $HoldMs
		[F4Input]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # left up
	}
	if ($RightClick) {
		[F4Input]::SetCursorPos(600, 400) | Out-Null
		[F4Input]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)  # right down
		Start-Sleep -Milliseconds $HoldMs
		[F4Input]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)  # right up
	}
	if ($Key -ne 0) {
		# The game reads scan codes (DirectInput), so send both.
		if ($Alt) { [F4Input]::keybd_event(0x12, 0x38, 0x0008, [UIntPtr]::Zero) }  # Alt down
		$scan = [byte][F4Input]::MapVirtualKey([uint32]$Key, 0)
		[F4Input]::keybd_event([byte]$Key, $scan, 0x0008, [UIntPtr]::Zero)  # KEYEVENTF_SCANCODE down
		Start-Sleep -Milliseconds $HoldMs
		[F4Input]::keybd_event([byte]$Key, $scan, 0x000A, [UIntPtr]::Zero)  # scan code up
		if ($Alt) { [F4Input]::keybd_event(0x12, 0x38, 0x000A, [UIntPtr]::Zero) }  # Alt up
	}
	Start-Sleep -Milliseconds 250
}
