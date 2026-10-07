# End-to-end soak test: the standalone server with several fake players (F4MPBot) on this machine.
#
#   tools\soak.ps1 [-Seconds 45] [-Port 7801] [-SharedStory] [-Build] [-KeepLogs] [-Mode releasedbg]
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
param(
	[int]$Seconds = 45,
	[int]$Port = 7801,
	[switch]$SharedStory,
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
	Require (Wait-Log "'Raider' joined as player 1 ") 'Raider joins as player 1'
	Start-Bot 'owner' 'Owner' $Seconds @('--own', $npc, '--say', $npc, '--radius', '400') | Out-Null
	Require (Wait-Log "'Owner' joined as player 2 ") 'Owner joins as player 2'
	Start-Bot 'looter' 'Looter' $Seconds @('--loot', "${container}:${item}:-10", '--quest', "${quest}:20", '--pickup', $pickup,
		'--door', "${door}:1:0", '--marker', $marker, '--ping', '1', '--speed', '370') | Out-Null
	Require (Wait-Log "'Looter' joined as player 3 ") 'Looter joins as player 3'

	Write-Host "Rejoiner: plays $short s and leaves"
	Start-Bot 'rejoiner1' 'Rejoiner' $short @('--identity', $identity) | Out-Null
	Require (Wait-Log "'Rejoiner' joined as player 4 ") 'Rejoiner joins as player 4'
	Wait-Exit 'rejoiner1' ($short + 10)
	Require (Wait-Log "'Rejoiner' left" 1 5) 'the server sees the Rejoiner leave'

	Write-Host 'Rejoiner: comes back, then is killed (a crash)'
	Start-Bot 'rejoiner2' 'Rejoiner' $Seconds @('--identity', $identity) | Out-Null
	Require (Wait-Log "'Rejoiner' came back as player 4 " 1) 'Rejoiner comes back as player 4'
	Start-Sleep -Seconds 3
	Stop-Process -Id $procs['rejoiner2'].Id -Force

	Write-Host "Rejoiner: comes back before the server notices the crash, plays $short s"
	Start-Bot 'rejoiner3' 'Rejoiner' $short @('--identity', $identity) | Out-Null
	Require (Wait-Log "'Rejoiner' came back as player 4 " 2) 'Rejoiner comes back as player 4 after the crash'

	Write-Host "Late: joins now, plays $short s"
	Start-Bot 'late' 'Late' $short @('--talk', $npc, '--radius', '150') | Out-Null
	Require (Wait-Log "'Late' joined as player 7 ") 'Late joins as player 7'

	foreach ($name in 'rejoiner3', 'late') { Wait-Exit $name ($short + 10) }
	$remaining = [Math]::Max(1, $Seconds - [int]((Get-Date) - $started).TotalSeconds)
	Write-Host "Waiting about $remaining s for the main bots"
	foreach ($name in 'raider', 'owner', 'looter') { Wait-Exit $name ($remaining + 15) }
	[void](Wait-Log "'Looter' left" 1 5)
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
	# The server log.
	foreach ($n in 'Raider', 'Owner', 'Looter', 'Late') {
		Check ((Count-Matches $log "'$n' joined as player") -eq 1) "server: '$n' joined once"
		Check ((Count-Matches $log "'$n' left") -eq 1) "server: '$n' left once"
	}
	Check ((Count-Matches $log "'Rejoiner' joined as player") -eq 1) "server: 'Rejoiner' joined once"
	Check ((Count-Matches $log "'Rejoiner' came back as player 4 ") -eq 2) "server: 'Rejoiner' came back as player 4 twice"
	Check ((Count-Matches $log "dropping the old connection of 'Rejoiner'") -eq 1) 'server: dropped the crashed Rejoiner connection'
	Check ((Count-Matches $log "'Rejoiner' left") -eq 3) "server: 'Rejoiner' left 3 times (left, crash, end)"
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
