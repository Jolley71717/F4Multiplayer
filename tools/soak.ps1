# End-to-end soak test: the standalone server with several fake players (F4MPBot) on this machine.
#
#   tools\soak.ps1 [-Seconds 45] [-Port 7801] [-SharedStory] [-Abuse] [-Build] [-KeepLogs] [-Mode releasedbg]
#
# Starts F4MPServer.exe on 127.0.0.1:Port, then:
#   Raider  (player 1)  sets the time of day, shoots, and hits the Owner with an NPC once the Owner is in
#   Owner   (player 2)  takes over an NPC as its companion, walks it around and has it talk
#   Looter  (player 3)  takes from a container, picks something up, opens a door, sets a quest stage,
#                       finds a map marker and pings
#   Rejoiner            leaves, comes back with the same identity (same player ID), "crashes" (is
#                       killed) and comes back again before the server notices
#   Late                joins halfway, must get the whole world state, and tries to talk the Owner's
#                       companion away (must be refused)
# Then checks exit codes, the server log and what each bot heard. Prints PASS/FAIL and exits 1 on
# failure. Logs go to a temp folder (kept on failure or with -KeepLogs).
# -Abuse runs the abuse scenario instead (described where it starts).
param(
	[int]$Seconds = 45,
	[int]$Port = 7801,
	[switch]$SharedStory,
	[switch]$Abuse,
	[switch]$Build,
	[switch]$KeepLogs,
	[string]$Mode = 'releasedbg'
)

$ErrorActionPreference = 'Stop'

if ($Seconds -lt 20) { throw '-Seconds must be at least 20' }
if ($Port -eq 7779) { throw 'Port 7779 is for real games; pick another' }

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\windows\x64\$Mode"
$serverExe = Join-Path $bin 'F4MPServer.exe'
$botExe = Join-Path $bin 'F4MPBot.exe'

if ($Build) {
	$xmake = 'C:\Program Files\xmake\xmake.exe'
	Push-Location $root
	try {
		& $xmake build -P . -y F4MPServer | Out-Null
		if ($LASTEXITCODE -ne 0) { throw 'Server build failed' }
		& $xmake build -P . -y F4MPBot | Out-Null
		if ($LASTEXITCODE -ne 0) { throw 'Bot build failed' }
	} finally {
		Pop-Location
	}
}
foreach ($exe in $serverExe, $botExe) {
	if (-not (Test-Path $exe)) { throw "$exe not found (build it, or pass -Build)" }
}

$logDir = Join-Path ([IO.Path]::GetTempPath()) ("f4mp-soak-{0}-{1}" -f $Port, (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $logDir | Out-Null
$serverLog = Join-Path $logDir 'server.log'

$contentHash = '2C42CB18'
$identity = '5EED5EED5EED5EED'
$npc = '0001D162'      # the Owner's companion
$attacker = '0002F4C5' # the NPC that "hits" the Owner
$quest = '000229A5'
$container = '000AE12F'
$item = '0000000F'
$pickup = '0001F00D'
$door = '0001E0AA'
$marker = '0001ABCD'
$hex = { param($h) ([Convert]::ToUInt32($h, 16)).ToString('x') }  # how the bot prints refs
$npcRef = & $hex $npc

$started = Get-Date
$results = New-Object System.Collections.ArrayList
$procs = @{}  # name -> process

function Check([bool]$ok, [string]$what) {
	[void]$results.Add([pscustomobject]@{ Ok = $ok; What = $what })
	if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red }
}

function Read-Text([string]$path) {
	try {
		$fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
		$reader = New-Object IO.StreamReader($fs)
		$text = $reader.ReadToEnd()
		$reader.Close()
		return $text
	} catch {
		return ''
	}
}

function Count-Matches([string]$text, [string]$pattern) {
	return ([regex]::Matches($text, $pattern, 'Multiline')).Count
}

function Stop-Leftovers {
	# Anything still running from an earlier, interrupted soak on this port.
	$filter = "Name = 'F4MPServer.exe' OR Name = 'F4MPBot.exe'"
	foreach ($p in @(Get-CimInstance Win32_Process -Filter $filter)) {
		if ($p.CommandLine -match "(--port $Port\b|127\.0\.0\.1:$Port\b)") {
			Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
		}
	}
}

function Start-Exe([string]$name, [string]$exe, [string[]]$arguments) {
	$p = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru `
		-RedirectStandardOutput (Join-Path $logDir "$name.log") -RedirectStandardError (Join-Path $logDir "$name.err.log")
	$null = $p.Handle  # Windows PowerShell only keeps the exit code if the handle was opened
	$procs[$name] = $p
	return $p
}

function Start-Bot([string]$name, [string]$botName, [int]$botSeconds, [string[]]$extra) {
	$arguments = @('--server', "127.0.0.1:$Port", '--name', $botName, '--content-hash', $contentHash, '--seconds', "$botSeconds",
		'--worldspace', '0000003C', '--x', '-80352', '--y', '89600', '--z', '7790') + $extra
	return Start-Exe $name $botExe $arguments
}

# Waits until the server log matches $pattern at least $count times.
function Wait-Log([string]$pattern, [int]$count = 1, [int]$timeout = 10) {
	$deadline = (Get-Date).AddSeconds($timeout)
	while ((Get-Date) -lt $deadline) {
		if ((Count-Matches (Read-Text $serverLog) $pattern) -ge $count) { return $true }
		if ($procs['server'].HasExited) { return $false }
		Start-Sleep -Milliseconds 100
	}
	return $false
}

function Require([bool]$ok, [string]$what) {
	Check $ok $what
	if (-not $ok) { throw "aborting: $what" }
}

function Wait-Exit([string]$name, [int]$timeout) {
	if (-not $procs[$name].WaitForExit($timeout * 1000)) {
		Check $false "$name finished within $timeout s"
	}
}

# How long each part takes: the three main bots stay the whole time; the rest come and go meanwhile.
$short = [Math]::Max(5, [int]($Seconds / 5))

# -Abuse: what one misbehaving game may cost the others, against a server with small tables (1200 entries,
# 1000 per player) so the limits are reached in seconds. An honest player plays throughout; meanwhile one
# bot after another reports thousands of made-up kills, loot, pickups and NPC claims, and another leaves and
# joins again over and over under one identity. The server must log each limit once, stay up and stay flat
# in memory, keep relaying the honest player, and never limit them.
if ($Abuse) {
	if ($Seconds -lt 40) { throw '-Abuse needs -Seconds 40 or more' }
	Write-Host "Abuse soak: port $Port, $Seconds s, logs: $logDir"
	$aborted = $null
	$memoryAfterFloods = 0
	$memoryAtEnd = 0
	try {
		Stop-Leftovers
		Start-Exe 'server' $serverExe @('--port', "$Port", '--max-players', '8', '--max-world-entries', '1200', '--max-share', '1000') | Out-Null
		Require (Wait-Log "listening on UDP port $Port") "server listens on port $Port"

		Start-Bot 'honest' 'Honest' $Seconds @('--identity', '0000000000001001', '--loot', "${container}:${item}:-10", '--kill', '0001A001',
			'--pickup', $pickup, '--door', "${door}:1:0", '--quest', "${quest}:20") | Out-Null
		Require (Wait-Log "'Honest' joined as player 1 ") 'Honest joins as player 1'
		Start-Bot 'looper' 'Looper' ($Seconds - 5) @('--identity', '0000000000002002', '--rejoin-loop', '20') | Out-Null

		$floods = @(
			@('kills', 'kills:1100', '0000000000003001'),
			@('kills2', 'kills:300:00B00000', '0000000000003002'),
			@('loot', 'loot:1100', '0000000000003003'),
			@('pickups', 'pickups:1100', '0000000000003004'),
			@('claims', 'claims:5000', '0000000000003005'))
		foreach ($flood in $floods) {
			Write-Host "Flooder: $($flood[1])"
			Start-Bot "flood-$($flood[0])" "Flood$($flood[0])" 8 @('--identity', $flood[2], '--flood', $flood[1]) | Out-Null
			Wait-Exit "flood-$($flood[0])" 20
		}
		$memoryAfterFloods = (Get-Process -Id $procs['server'].Id).WorkingSet64
		$remaining = [Math]::Max(1, $Seconds - [int]((Get-Date) - $started).TotalSeconds)
		Write-Host "Waiting about $remaining s for the honest player and the looper"
		foreach ($name in 'honest', 'looper') { Wait-Exit $name ($remaining + 15) }
		$memoryAtEnd = (Get-Process -Id $procs['server'].Id).WorkingSet64
		Start-Sleep -Milliseconds 500
	} catch {
		$aborted = $_.Exception.Message
		Write-Host "  $aborted" -ForegroundColor Red
	} finally {
		$serverAlive = $procs.ContainsKey('server') -and -not $procs['server'].HasExited
		foreach ($p in $procs.Values) {
			if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
		}
		foreach ($p in $procs.Values) { [void]$p.WaitForExit(5000) }
		Stop-Leftovers
	}

	Write-Host 'Checks:'
	Check $serverAlive 'server still running at the end'
	$log = Read-Text $serverLog
	foreach ($name in @($procs.Keys | Where-Object { $_ -ne 'server' })) {
		Check ($procs[$name].ExitCode -eq 0) "$name exits 0 (got $($procs[$name].ExitCode))"
	}
	if (-not $aborted) {
		Check ((Count-Matches $log "'Floodkills' has added 1000 dead NPC entries this run, their share") -eq 1) 'server: the kill flooder is stopped at its share, said once'
		Check ((Count-Matches $log "dead NPC table is \d+% full \(\d+/1200\); kill sharing stops at 100%") -eq 1) 'server: the kill table is said to be near full once'
		Check ((Count-Matches $log "dead NPC table is 100% full \(1200/1200\)") -eq 1) 'server: the kill table is said to be full once'
		Check ((Count-Matches $log "'Floodloot' has made 1000 container changes this session, their share") -eq 1) 'server: the loot flooder is stopped at its share, said once'
		Check ((Count-Matches $log "'Floodpickups' has added 1000 picked-up item entries this run, their share") -eq 1) 'server: the pickup flooder is stopped at its share, said once'
		Check ((Count-Matches $log "says hello too often") -ge 1) 'server: the looper waits for its budget'
		Check ((Count-Matches $log "'Honest'.*(their share|too often|dropping some)") -eq 0) 'server: the honest player is never limited'
		Check ((Count-Matches $log "(?i)too many events") -eq 0) 'server: no flood of events beyond the per-second limit'
		$grown = $memoryAtEnd - $memoryAfterFloods
		Check ($memoryAfterFloods -gt 0 -and $grown -lt 20MB) "server: memory flat after the floods (grew $([int]($grown / 1KB)) KB)"

		$h = Read-Text (Join-Path $logDir 'honest.log')
		Check ($h -match "(?m)^container changed #0 by player 1") 'honest: its loot is shared'
		$rates = [regex]::Match($h, '^rates: player states (\d+)/s', 'Multiline')
		Check ($rates.Success -and [int]$rates.Groups[1].Value -ge 20) "honest: hears the others throughout ($($rates.Groups[1].Value)/s)"
		$fk = Read-Text (Join-Path $logDir 'flood-kills.log')
		$fkRates = [regex]::Match($fk, '^rates: player states (\d+)/s', 'Multiline')
		Check ($fkRates.Success -and [int]$fkRates.Groups[1].Value -ge 20) "the honest player's moves keep arriving during a flood ($($fkRates.Groups[1].Value)/s)"

		# The looper: three welcomes at once (its bucket of world states), then at most one per ten seconds.
		$lp = Read-Text (Join-Path $logDir 'looper.log')
		$welcomes = Count-Matches $lp '^welcomed as player'
		$allowed = 3 + [Math]::Ceiling(($Seconds - 5) / 10) + 1
		Check ($welcomes -ge 4 -and $welcomes -le $allowed) "looper: welcomed $welcomes times in $($Seconds - 5) s (want 4 to $allowed)"
	}

	$failed = @($results | Where-Object { -not $_.Ok })
	$elapsed = [int]((Get-Date) - $started).TotalSeconds
	Write-Host ''
	if ($aborted -or $failed.Count -gt 0) {
		Write-Host "ABUSE SOAK FAIL: $($failed.Count) of $($results.Count) checks failed in $elapsed s. Logs: $logDir" -ForegroundColor Red
		exit 1
	}
	Write-Host "ABUSE SOAK PASS: $($results.Count) checks in $elapsed s." -ForegroundColor Green
	if ($KeepLogs) { Write-Host "Logs: $logDir" } else { Remove-Item -Recurse -Force $logDir }
	exit 0
}

Write-Host "Soak test: port $Port, $Seconds s, shared story: $([bool]$SharedStory), logs: $logDir"
$aborted = $null
try {
	Stop-Leftovers

	$serverArgs = @('--port', "$Port", '--max-players', '8')
	if ($SharedStory) { $serverArgs += '--shared-story' }
	Start-Exe 'server' $serverExe $serverArgs | Out-Null
	Require (Wait-Log "listening on UDP port $Port") "server listens on port $Port"

	Write-Host 'Starting Raider, Owner, Looter'
	Start-Bot 'raider' 'Raider' $Seconds @('--time', '14', '--time-step', '1', '--weather', '0001E0F2', '--shoot', '0000463F',
		'--hit-player', "2:15:$attacker", '--radius', '200') | Out-Null
	Require (Wait-Log 'server: player 1 joined from ') 'Raider joins as player 1'
	Start-Bot 'owner' 'Owner' $Seconds @('--own', $npc, '--say', $npc, '--radius', '400') | Out-Null
	Require (Wait-Log 'server: player 2 joined from ') 'Owner joins as player 2'
	Start-Bot 'looter' 'Looter' $Seconds @('--loot', "${container}:${item}:-10", '--quest', "${quest}:20", '--pickup', $pickup,
		'--door', "${door}:1:0", '--marker', $marker, '--ping', '1', '--speed', '370') | Out-Null
	Require (Wait-Log 'server: player 3 joined from ') 'Looter joins as player 3'

	Write-Host "Rejoiner: plays $short s and leaves"
	Start-Bot 'rejoiner1' 'Rejoiner' $short @('--identity', $identity) | Out-Null
	Require (Wait-Log 'server: player 4 joined from ') 'Rejoiner joins as player 4'
	Wait-Exit 'rejoiner1' ($short + 10)
	Require (Wait-Log 'server: player 4 left' 1 5) 'the server sees the Rejoiner leave'

	Write-Host 'Rejoiner: comes back, then is killed (a crash)'
	Start-Bot 'rejoiner2' 'Rejoiner' $Seconds @('--identity', $identity) | Out-Null
	Require (Wait-Log 'server: player 4 came back from ' 1) 'Rejoiner comes back as player 4'
	Start-Sleep -Seconds 3
	Stop-Process -Id $procs['rejoiner2'].Id -Force

	Write-Host "Rejoiner: comes back before the server notices the crash, plays $short s"
	Start-Bot 'rejoiner3' 'Rejoiner' $short @('--identity', $identity) | Out-Null
	Require (Wait-Log 'server: player 4 came back from ' 2) 'Rejoiner comes back as player 4 after the crash'

	Write-Host "Late: joins now, plays $short s"
	Start-Bot 'late' 'Late' $short @('--talk', $npc, '--radius', '150') | Out-Null
	Require (Wait-Log 'server: player 7 joined from ') 'Late joins as player 7'

	foreach ($name in 'rejoiner3', 'late') { Wait-Exit $name ($short + 10) }
	$remaining = [Math]::Max(1, $Seconds - [int]((Get-Date) - $started).TotalSeconds)
	Write-Host "Waiting about $remaining s for the main bots"
	foreach ($name in 'raider', 'owner', 'looter') { Wait-Exit $name ($remaining + 15) }
	[void](Wait-Log 'server: player 3 left' 1 5)
	Start-Sleep -Milliseconds 500
} catch {
	$aborted = $_.Exception.Message
	Write-Host "  $aborted" -ForegroundColor Red
} finally {
	$serverAlive = $procs.ContainsKey('server') -and -not $procs['server'].HasExited
	foreach ($p in $procs.Values) {
		if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
	}
	foreach ($p in $procs.Values) { [void]$p.WaitForExit(5000) }
	Stop-Leftovers
}

Write-Host 'Checks:'
if ($procs.ContainsKey('server')) {
	$serverCode = ''
	if (-not $serverAlive) { $serverCode = " (it exited with code $($procs['server'].ExitCode))" }
	Check $serverAlive "server still running at the end$serverCode"
}

$log = Read-Text $serverLog
$out = @{}
foreach ($name in $procs.Keys) { $out[$name] = Read-Text (Join-Path $logDir "$name.log") }
$finished = @('raider', 'owner', 'looter', 'rejoiner1', 'rejoiner3', 'late') | Where-Object { $procs.ContainsKey($_) }

# Exit codes: a crash shows up as a huge negative number (e.g. -1073741819 = access violation).
foreach ($name in $finished) {
	$code = $procs[$name].ExitCode
	Check ($code -eq 0) "$name exits 0 (got $code)"
}

if (-not $aborted) {
	# The server log (players by ID: Raider 1, Owner 2, Looter 3, Rejoiner 4, Late 7).
	foreach ($pair in @(@('Raider', 1), @('Owner', 2), @('Looter', 3), @('Late', 7))) {
		$n = $pair[0]; $p = $pair[1]
		Check ((Count-Matches $log "server: player $p joined from ") -eq 1) "server: $n (player $p) joined once"
		Check ((Count-Matches $log "server: player $p left") -eq 1) "server: $n (player $p) left once"
	}
	Check ((Count-Matches $log 'server: player 4 joined from ') -eq 1) 'server: the Rejoiner (player 4) joined once'
	Check ((Count-Matches $log 'server: player 4 came back from ') -eq 2) 'server: the Rejoiner came back as player 4 twice'
	Check ((Count-Matches $log 'dropping the old connection of player 4 ') -eq 1) 'server: dropped the crashed Rejoiner connection'
	Check ((Count-Matches $log 'server: player 4 left') -eq 3) 'server: the Rejoiner left 3 times (left, crash, end)'
	# What players attach to bug reports: no names, no SteamID64, no address but this machine's.
	Check ((Count-Matches $log 'Raider|Owner|Looter|Rejoiner|Late') -eq 0) 'server: names no player'
	Check ((Count-Matches $log '7656\d{13}') -eq 0) 'server: holds no SteamID64'
	Check ((Count-Matches $log '\b(?!127\.)\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b') -eq 0) 'server: holds no address but 127.0.0.1'
	foreach ($bad in 'too many', 'no hello', 'load order changed', 'Server is full', 'wrong password', 'error', 'failed') {
		Check ((Count-Matches $log "(?i)$bad") -eq 0) "server: nothing about '$bad'"
	}

	# What each bot heard.
	$story = ''
	if ($SharedStory) { $story = ' (shared story)' }
	$expectId = @{ raider = 1; owner = 2; looter = 3; rejoiner1 = 4; rejoiner3 = 4; late = 7 }
	foreach ($name in $finished) {
		$text = $out[$name]
		$welcome = [regex]::Match($text, '^welcomed as player (\d+) in session [0-9a-f]+(.*)$', 'Multiline')
		Check ($welcome.Success -and [int]$welcome.Groups[1].Value -eq $expectId[$name]) "${name}: welcomed as player $($expectId[$name])"
		Check ($welcome.Success -and $welcome.Groups[2].Value.TrimEnd() -eq $story) "${name}: shared story flag is $([bool]$SharedStory)"
		$rates = [regex]::Match($text, '^rates: player states (\d+)/s, npc states/s:(.*)$', 'Multiline')
		Check ($rates.Success -and [int]$rates.Groups[1].Value -ge 20) "${name}: hears other players ($($rates.Groups[1].Value)/s, want >= 20)"
		if ($name -ne 'owner') {
			$npcRate = [regex]::Match($rates.Groups[2].Value, "\b$npcRef=(\d+)")
			Check ($npcRate.Success -and [int]$npcRate.Groups[1].Value -ge 10) "${name}: hears the Owner's NPC ($($npcRate.Groups[1].Value)/s, want >= 10)"
		} else {
			Check ($rates.Success -and $rates.Groups[2].Value.Trim() -eq '') 'owner: its own NPC is not echoed back'
		}
		# Nobody ever takes the Owner's companion (Late keeps trying).
		Check ((Count-Matches $text "owners:.* $npcRef=([13-9]|\d\d)") -eq 0) "${name}: the companion is never given to anyone but the Owner"
	}

	$o = $out['owner']; $r = $out['raider']; $l = $out['looter']; $late = $out['late']
	Check ($o -match "(?m)^damaged by player 1's npc: 15 \(attacker $(& $hex $attacker)\)") 'owner: hit by the Raider''s NPC'
	Check ($o -match "(?m)^owners:.* $npcRef=2") 'owner: gets its NPC'
	Check ($o -match "(?m)^time: player 1 hour 14 ") 'owner: gets the Raider''s time of day'
	Check ($l -match "(?m)^time: player 1 hour ") 'looter: gets the Raider''s time of day'
	Check (-not ($r -match "(?m)^time: ")) 'raider: is the clock, so nobody else''s time comes back'
	Check ($l -match "(?m)^container changed #0 by player 3: $(& $hex $container) item $(& $hex $item) count -10") 'looter: its container change is echoed as #0'
	foreach ($pair in @(@('raider', $r), @('owner', $o))) {
		$n = $pair[0]; $t = $pair[1]
		Check ($t -match "(?m)^container changed #0 by player 3") "${n}: sees the Looter's container change"
		Check ($t -match "(?m)^quest stage: $(& $hex $quest) 20") "${n}: sees the Looter's quest stage"
		Check ($t -match "(?m)^picked up: $(& $hex $pickup)") "${n}: sees the Looter's pickup"
		Check ($t -match "(?m)^ref state: $(& $hex $door) open=1 locked=0") "${n}: sees the Looter's door"
		Check ($t -match "(?m)^markers: player 3 1 $(& $hex $marker):3") "${n}: sees the Looter's map marker"
		Check ($t -match "(?m)^ping: player 3 at ") "${n}: sees the Looter's ping"
	}
	Check ($l -match "(?m)^shot: player 1 ref 0") 'looter: sees the Raider shoot'
	Check ($l -match "(?m)^line: player 2 speaker $npcRef ") 'looter: hears the Owner''s NPC talk'

	# The late joiner gets everything that happened before it came.
	Check ($late -match "(?m)^world state: 1 container changes from #0, 1 pickups, 1 ref states, 1 quest stages, 0 dead actors") 'late: gets the whole world state'
	Check ($late -match "(?m)^markers: player 0 1 $(& $hex $marker):3") 'late: gets the found map markers'
	Check ($late -match "(?m)^owners:.* $npcRef=2") 'late: is told who runs the NPC'
	Check ($late -match "(?m)^time: player 1 hour ") 'late: gets the time of day'
	foreach ($p in '1 Raider', '2 Owner', '3 Looter', '4 Rejoiner') {
		Check ($late -match "(?m)^player joined: $p\r?$") "late: told about player $p"
	}
	Check ((Count-Matches $late '^status: player [1-4] ') -ge 4) 'late: gets everyone''s status'

	# The Raider was there for all the Rejoiner's comings and goings.
	Check ((Count-Matches $r '^player joined: 4 Rejoiner') -eq 3) 'raider: told the Rejoiner joined 3 times'
	Check ((Count-Matches $r '^player left: 4\r?$') -eq 3) 'raider: told the Rejoiner left 3 times'
	Check ((Count-Matches $r '^player left: 7\r?$') -eq 1) 'raider: told Late left'
}

$failed = @($results | Where-Object { -not $_.Ok })
$elapsed = [int]((Get-Date) - $started).TotalSeconds
Write-Host ''
if ($aborted -or $failed.Count -gt 0) {
	Write-Host "SOAK FAIL: $($failed.Count) of $($results.Count) checks failed in $elapsed s. Logs: $logDir" -ForegroundColor Red
	exit 1
}
Write-Host "SOAK PASS: $($results.Count) checks in $elapsed s." -ForegroundColor Green
if ($KeepLogs) {
	Write-Host "Logs: $logDir"
} else {
	Remove-Item -Recurse -Force $logDir
}
exit 0
