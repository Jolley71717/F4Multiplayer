# Sends real input to the game window for tests that need the player to act (fire, press a key).
#
#   .\tools\input.ps1 -Click                 # left mouse click (fire)
#   .\tools\input.ps1 -Click -HoldMs 800     # hold the button (automatic weapons)
#   .\tools\input.ps1 -Key 0x2E              # press a key (virtual-key code; 0x2E = Delete)
#   .\tools\input.ps1 -Spot                  # only put the cursor where a click reaches the game, and say what was there
#
# A click goes to the window under the cursor, which is not always the game: a window kept on top of every other one (a
# system error box, "cl.exe - Application Error" from a compiler crash on 2026-11-10, sat in the middle of the host's
# screen) covers part of the game even while the game is in front. A click there lands on that window, which takes the
# front: the game sees the button go down in its raw input, loses the front before it fires, and never sees it come up
# (the dev channel's `mousedev`: leftDown rising, front=no right after). So the cursor is put first on a point of the game's
# window that no other window covers (the middle when it is free), and -Spot says which window covered the point it was on.
# SetCursorPos makes no raw mouse movement: the camera does not turn. The dev channel's `click` keeps the cursor where it is
# when it is over the game's window, so -Spot before a `click` places that click too.
param(
	[switch]$Click,
	[switch]$RightClick,  # hold the right button (aim down sights)
	[switch]$Alt,  # hold Alt while pressing the key (Alt+F9: ShadowPlay recording)
	[switch]$Spot,
	[int]$HoldMs = 60,
	[int]$Key = 0,
	[int]$Times = 1
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class F4Input {
	[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
	[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
	[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
	[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
	[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
	[DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hWnd, int index);
	[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder s, int n);
	[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
	[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
	[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
'@

# The top-level window at a screen point, and whose it is.
function Get-RootAt([int]$x, [int]$y) {
	$p = New-Object F4Input+POINT; $p.X = $x; $p.Y = $y
	$w = [F4Input]::WindowFromPoint($p)
	if ($w -ne [IntPtr]::Zero) { $w = [F4Input]::GetAncestor($w, 2) }  # GA_ROOT
	$procId = [uint32]0
	if ($w -ne [IntPtr]::Zero) { [F4Input]::GetWindowThreadProcessId($w, [ref]$procId) | Out-Null }
	[pscustomobject]@{ Window = $w; ProcessId = $procId }
}

# A window named for a report: its process, its title and whether it is kept on top. Paths under a user's profile lose the
# user's name (rig logs never hold one).
function Get-WindowName([IntPtr]$w, [uint32]$procId) {
	if ($w -eq [IntPtr]::Zero) { return 'no window' }
	$t = New-Object Text.StringBuilder 160
	[F4Input]::GetWindowText($w, $t, 160) | Out-Null
	$title = $t.ToString() -replace '(?i)(\\users\\)[^\\]+', '$1<user>'
	if ($env:USERNAME) { $title = $title.Replace($env:USERNAME, '<user>') }
	$proc = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
	$top = ([F4Input]::GetWindowLong($w, -20) -band 0x8) -ne 0  # GWL_EXSTYLE, WS_EX_TOPMOST
	"$(if ($proc) { $proc } else { "pid $procId" }) '$title'$(if ($top) { ' (kept on top)' })"
}

# Puts the cursor on a point of the game's window that the game itself has under it: the cursor's own point when it is one,
# else the middle, else the first free point of a grid over the window (kept off its edges, where a taskbar can sit). Returns
# the line -Spot prints: spot=ok, spot=moved, spot=covered (no free point: the cursor is left where it was), spot=offscreen
# or spot=nogame.
function Set-ClickSpot($game) {
	$w = $game.MainWindowHandle
	if ($w -eq [IntPtr]::Zero) { return 'spot=nogame (the game has no main window)' }
	$r = New-Object F4Input+RECT
	[F4Input]::GetWindowRect($w, [ref]$r) | Out-Null
	if ($r.Left -le -30000 -or $r.Top -le -30000) { return "spot=offscreen (the game's window is at $($r.Left), $($r.Top): parked off every screen)" }
	$c = New-Object F4Input+POINT
	[F4Input]::GetCursorPos([ref]$c) | Out-Null
	$inside = $c.X -ge $r.Left -and $c.X -lt $r.Right -and $c.Y -ge $r.Top -and $c.Y -lt $r.Bottom
	$at = Get-RootAt $c.X $c.Y
	if ($inside -and $at.ProcessId -eq $game.Id) { return "spot=ok $($c.X),$($c.Y)" }
	$why = if ($inside) { "the cursor's point $($c.X),$($c.Y) was under $(Get-WindowName $at.Window $at.ProcessId)" } else { "the cursor $($c.X),$($c.Y) was off the game's window" }
	$width = $r.Right - $r.Left; $height = $r.Bottom - $r.Top
	$fractions = 0.5, 0.35, 0.65, 0.2, 0.8
	$covers = @{}
	foreach ($fy in $fractions) {
		foreach ($fx in $fractions) {
			$x = $r.Left + [int]($width * $fx); $y = $r.Top + [int]($height * $fy)
			$hit = Get-RootAt $x $y
			if ($hit.ProcessId -eq $game.Id) {
				[F4Input]::SetCursorPos($x, $y) | Out-Null
				$middle = if ($fx -eq 0.5 -and $fy -eq 0.5) { ' (the middle)' } else { '' }
				return "spot=moved $x,$y$middle ($why)"
			}
			$covers[(Get-WindowName $hit.Window $hit.ProcessId)] = 1
		}
	}
	"spot=covered (no point of the game's window is free: $(($covers.Keys | Sort-Object) -join '; '); $why)"
}

$game = Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) {
	if ($Spot) { 'spot=nogame (Fallout 4 is not running)'; exit 0 }
	throw 'Fallout 4 is not running'
}
if ($Spot) { Set-ClickSpot $game; exit 0 }

# Windows only lets the foreground process change the foreground window; a tapped Alt lifts that.
[F4Input]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[F4Input]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
[F4Input]::keybd_event(0x12, 0, 0x0002, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 400

if ($Click -or $RightClick) {
	$s = Set-ClickSpot $game
	if ($s -notlike 'spot=ok*') { Write-Host $s }
}
for ($i = 0; $i -lt $Times; $i++) {
	if ($Click) {
		[F4Input]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # left down
		Start-Sleep -Milliseconds $HoldMs
		[F4Input]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # left up
	}
	if ($RightClick) {
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
