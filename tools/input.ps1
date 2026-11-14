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
# client area (never its title bar or borders, where a press drags the window) that no other window covers (the middle when
# it is free), and -Spot says which window covered the point it was on: its process and window class, never its title (a
# title can hold a person's name, and these lines go into rig reports).
# SetCursorPos makes no raw mouse movement: the camera does not turn. The dev channel's `click` keeps the cursor where it is
# when it is over the game's window, so -Spot before a `click` places that click too.
#
# The host is the maintainer's own PC: input from here must never land anywhere but the game. No click or key is sent when
#   - Fallout 4 is not running: exit 2;
#   - for -Click / -RightClick: no point of the game's client area is free (spot=covered), its window is parked off every
#     screen (spot=offscreen) or it has no window (spot=nogame), or, checked again just before each press, the window under
#     the cursor is not the game's: exit 3;
#   - for -Key: the game is not the foreground window (checked before each press): exit 4.
# Each prints one line starting "input=refused". -Spot sends no input and always exits 0 with its spot= line.
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
	[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
	[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
	[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
	[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
	[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
	[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
	[DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hWnd, int index);
	[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder s, int n);
	[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
	[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
	[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
'@

# The process that owns a window (0 for none).
function Get-WindowProcess([IntPtr]$w) {
	$procId = [uint32]0
	if ($w -ne [IntPtr]::Zero) { [F4Input]::GetWindowThreadProcessId($w, [ref]$procId) | Out-Null }
	$procId
}

# The top-level window at a screen point, and whose it is.
function Get-RootAt([int]$x, [int]$y) {
	$p = New-Object F4Input+POINT; $p.X = $x; $p.Y = $y
	$w = [F4Input]::WindowFromPoint($p)
	if ($w -ne [IntPtr]::Zero) { $w = [F4Input]::GetAncestor($w, 2) }  # GA_ROOT
	[pscustomobject]@{ Window = $w; ProcessId = (Get-WindowProcess $w) }
}

# A window named for a report: its process, its window class and whether it is kept on top. Never its title: a chat's or a
# browser tab's title can hold a person's name, and these lines go into check results pasted into PRs.
function Get-WindowName([IntPtr]$w, [uint32]$procId) {
	if ($w -eq [IntPtr]::Zero) { return 'no window' }
	$c = New-Object Text.StringBuilder 128
	[F4Input]::GetClassName($w, $c, 128) | Out-Null
	$proc = (Get-Process -Id $procId -ErrorAction SilentlyContinue).ProcessName
	$top = ([F4Input]::GetWindowLong($w, -20) -band 0x8) -ne 0  # GWL_EXSTYLE, WS_EX_TOPMOST
	"$(if ($proc) { $proc } else { "pid $procId" }) [$($c.ToString())]$(if ($top) { ' (kept on top)' })"
}

# The game's client area in screen coordinates (no title bar, no borders), or $null when it has none.
function Get-ClientArea([IntPtr]$w) {
	$cr = New-Object F4Input+RECT
	if (-not [F4Input]::GetClientRect($w, [ref]$cr)) { return $null }
	$o = New-Object F4Input+POINT
	if (-not [F4Input]::ClientToScreen($w, [ref]$o)) { return $null }
	[pscustomobject]@{ Left = $o.X; Top = $o.Y; Right = $o.X + $cr.Right; Bottom = $o.Y + $cr.Bottom }
}

# Is the cursor on the game's client area, with the game's own window under it? (a press there reaches the game)
function Test-CursorOnGame($game) {
	$a = Get-ClientArea $game.MainWindowHandle
	if (-not $a) { return $false }
	$c = New-Object F4Input+POINT
	[F4Input]::GetCursorPos([ref]$c) | Out-Null
	$inside = $c.X -ge $a.Left -and $c.X -lt $a.Right -and $c.Y -ge $a.Top -and $c.Y -lt $a.Bottom
	$inside -and (Get-RootAt $c.X $c.Y).ProcessId -eq $game.Id
}

# Puts the cursor on a point of the game's client area that the game itself has under it: the cursor's own point when it is
# one, else the middle, else the first free point of a grid over the client area (kept off its edges, where a taskbar can
# sit). Returns the line -Spot prints: spot=ok, spot=moved, spot=covered (no free point: the cursor is left where it was),
# spot=offscreen (the window is parked off every screen; nothing over it is to blame) or spot=nogame.
function Set-ClickSpot($game) {
	$w = $game.MainWindowHandle
	if ($w -eq [IntPtr]::Zero) { return 'spot=nogame (the game has no main window)' }
	$r = New-Object F4Input+RECT
	[F4Input]::GetWindowRect($w, [ref]$r) | Out-Null
	if ($r.Left -le -30000 -or $r.Top -le -30000) { return "spot=offscreen (the game's window is at $($r.Left), $($r.Top): parked off every screen, minimised during its load)" }
	$a = Get-ClientArea $w
	if (-not $a -or $a.Right -le $a.Left -or $a.Bottom -le $a.Top) { return "spot=offscreen (the game's window has no client area on a screen)" }
	$c = New-Object F4Input+POINT
	[F4Input]::GetCursorPos([ref]$c) | Out-Null
	$inside = $c.X -ge $a.Left -and $c.X -lt $a.Right -and $c.Y -ge $a.Top -and $c.Y -lt $a.Bottom
	$at = Get-RootAt $c.X $c.Y
	if ($inside -and $at.ProcessId -eq $game.Id) { return "spot=ok $($c.X),$($c.Y)" }
	$why = if ($inside) { "the cursor's point $($c.X),$($c.Y) was under $(Get-WindowName $at.Window $at.ProcessId)" } else { "the cursor $($c.X),$($c.Y) was off the game's client area" }
	$width = $a.Right - $a.Left; $height = $a.Bottom - $a.Top
	$fractions = 0.5, 0.35, 0.65, 0.2, 0.8
	$covers = @{}
	foreach ($fy in $fractions) {
		foreach ($fx in $fractions) {
			$x = $a.Left + [int]($width * $fx); $y = $a.Top + [int]($height * $fy)
			$hit = Get-RootAt $x $y
			if ($hit.ProcessId -eq $game.Id) {
				[F4Input]::SetCursorPos($x, $y) | Out-Null
				$middle = if ($fx -eq 0.5 -and $fy -eq 0.5) { ' (the middle)' } else { '' }
				return "spot=moved $x,$y$middle ($why)"
			}
			$covers[(Get-WindowName $hit.Window $hit.ProcessId)] = 1
		}
	}
	"spot=covered (no point of the game's client area is free: covered by $(($covers.Keys | Sort-Object) -join '; '); $why)"
}

# Is the game the foreground window? (a key goes to the foreground window, whatever it is)
function Test-GameInFront($game) {
	$f = [F4Input]::GetForegroundWindow()
	if ($f -ne [IntPtr]::Zero) { $f = [F4Input]::GetAncestor($f, 2) }  # GA_ROOT
	(Get-WindowProcess $f) -eq $game.Id
}
function Get-FrontName {
	$f = [F4Input]::GetForegroundWindow()
	if ($f -ne [IntPtr]::Zero) { $f = [F4Input]::GetAncestor($f, 2) }
	Get-WindowName $f (Get-WindowProcess $f)
}

$game = Get-Process Fallout4 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) {
	if ($Spot) { 'spot=nogame (Fallout 4 is not running)'; exit 0 }
	'input=refused (Fallout 4 is not running): nothing was sent'; exit 2
}
if ($Spot) { Set-ClickSpot $game; exit 0 }

# A click's spot is checked before anything is sent: a refused click sends nothing at all, not even the Alt tap below (which
# goes to whatever window is in front). Checked again before each press.
if ($Click -or $RightClick) {
	$s = Set-ClickSpot $game
	if ($s -notlike 'spot=ok*' -and $s -notlike 'spot=moved*') { "input=refused ($s): no click was sent"; exit 3 }
	if ($s -notlike 'spot=ok*') { Write-Host $s }
}

# Windows only lets the foreground process change the foreground window; Alt held across the call lifts that. Its release
# comes after the game has the front, so it goes to the game. Not done when the game is in front already (the rig's scripts
# bring it to the front through the dev channel first, so this is the rare case).
if (-not (Test-GameInFront $game)) {
	[F4Input]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
	[F4Input]::SetForegroundWindow($game.MainWindowHandle) | Out-Null
	[F4Input]::keybd_event(0x12, 0, 0x0002, [UIntPtr]::Zero)
	Start-Sleep -Milliseconds 400
}

for ($i = 0; $i -lt $Times; $i++) {
	if (($Click -or $RightClick) -and -not (Test-CursorOnGame $game)) {
		$c = New-Object F4Input+POINT; [F4Input]::GetCursorPos([ref]$c) | Out-Null; $at = Get-RootAt $c.X $c.Y
		"input=refused (press $($i + 1) of ${Times}: the cursor $($c.X),$($c.Y) is not on the game's client area, it is over $(Get-WindowName $at.Window $at.ProcessId)): no click was sent"; exit 3
	}
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
		if (-not (Test-GameInFront $game)) { "input=refused (press $($i + 1) of ${Times}: the game is not the foreground window, $(Get-FrontName) is): no key was sent"; exit 4 }
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
