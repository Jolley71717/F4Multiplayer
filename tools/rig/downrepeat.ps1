# downrepeat.ps1 -Rounds N [-Explosion <form id>]: tools/rig/steps.sh step_downed_repeat's rounds, run on the machine of the game
# whose player goes down, against that game's dev channel on the loopback (its token in the F4SE folder). Over the rig's ssh tunnel
# a dev command takes 0.3-3 s, too long to land a hit inside a get-up; here it takes a few dozen ms. A lethal hit (console damageav,
# as the host's gunfire is injected for combat), then per round: wait for the down, wait for the help-up (the helper stands by), and
# hit again: at once (in the get-up), half a second on, or 0.3 s after a grenade's explosion at the feet; round 2 takes a
# second hit while still down. One line per event; "helped up" lines are counted by the step.
param([int]$Rounds = 5, [string]$Explosion = '000E574F')
$tok = (Get-Content "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token" -Raw).Trim()
function Dc([string]$cmd) {
	for ($i = 0; $i -lt 5; $i++) {
		try {
			$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 7790); $s = $c.GetStream()
			$w = New-Object System.IO.StreamWriter($s); $w.AutoFlush = $true; $r = New-Object System.IO.StreamReader($s)
			$w.WriteLine("auth $tok"); $a = $r.ReadLine()
			if ($a -eq 'ok') { $w.WriteLine($cmd); $o = $r.ReadLine(); $c.Close(); return $o }
			$c.Close()
		} catch { }
		Start-Sleep -Milliseconds 100
	}
	return ''
}
function Down { (Dc 'rules') -match ' down=true' }
function View { $c = Dc 'controls'; "$([regex]::Match($c, 'camera=[0-9-]+').Value) $([regex]::Match($c, 'life=[0-9]+').Value)" }
function T { (Get-Date).ToString('HH:mm:ss.fff') }
$sw = [Diagnostics.Stopwatch]::StartNew()
Dc 'console player.damageav health 10000' | Out-Null
for ($i = 1; $i -le $Rounds; $i++) {
	$end = $sw.ElapsedMilliseconds + 8000; while (-not (Down) -and $sw.ElapsedMilliseconds -lt $end) { Start-Sleep -Milliseconds 30 }
	if (-not (Down)) { "$(T) round ${i}: never went down"; break }
	"$(T) round ${i}: down ($(View))"
	if ($i -eq 2) { Start-Sleep -Milliseconds 800; Dc 'console player.damageav health 10000' | Out-Null; "$(T) round ${i}: hit again while down" }
	$end = $sw.ElapsedMilliseconds + 20000; while ((Down) -and $sw.ElapsedMilliseconds -lt $end) { Start-Sleep -Milliseconds 30 }
	if (Down) { "$(T) round ${i}: never helped up"; break }
	"$(T) round ${i}: helped up"
	if ($i -eq $Rounds) { break }
	switch ($i % 3) {
		1 { }
		2 { Start-Sleep -Milliseconds 500 }
		0 { Dc "console player.placeatme $Explosion" | Out-Null; Start-Sleep -Milliseconds 300 }
	}
	Dc 'console player.damageav health 10000' | Out-Null
	"$(T) round ${i}: hit again ($(View))"
}
"$(T) done"
