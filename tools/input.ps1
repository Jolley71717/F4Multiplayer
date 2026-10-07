# Sends real input to the game window for tests that need the player to act (fire, press a key).
#
#   .\tools\input.ps1 -Click                 # left mouse click (fire)
#   .\tools\input.ps1 -Click -HoldMs 800     # hold the button (automatic weapons)
#   .\tools\input.ps1 -Key 0x2E              # press a key (virtual-key code; 0x2E = Delete)
param(
	[switch]$Click,
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
	[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
	[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
	[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
'@

$game = Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { throw 'Fallout 4 is not running' }
[F4Input]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300

for ($i = 0; $i -lt $Times; $i++) {
	if ($Click) {
		[F4Input]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # left down
		Start-Sleep -Milliseconds $HoldMs
		[F4Input]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # left up
	}
	if ($Key -ne 0) {
		# The game reads scan codes (DirectInput), so send both.
		$scan = [byte][F4Input]::MapVirtualKey([uint32]$Key, 0)
		[F4Input]::keybd_event([byte]$Key, $scan, 0x0008, [UIntPtr]::Zero)  # KEYEVENTF_SCANCODE down
		Start-Sleep -Milliseconds $HoldMs
		[F4Input]::keybd_event([byte]$Key, $scan, 0x000A, [UIntPtr]::Zero)  # scan code up
	}
	Start-Sleep -Milliseconds 250
}
