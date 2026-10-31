# Phase-0 probes for shared rolls (docs/SHARED-ROLLS.md) on the rig: the host PC's game and the VM's, both in one
# session, both running this branch's build with bDevChannel = true. Read-only apart from the steps named below. It
# never moves a player: walk both players to the place first (the script waits while you do).
#
# Before running it: take rig.lock, back up both machines' F4Multiplayer.ini and session files (AGENTS.md, the test
# rig), and open the tunnel to the VM's dev channel. The VM's token is read by devctl.ps1 from $env:F4MP_DEV_TOKEN for
# the guest's calls alone; this script never prints it.
#
#   .\tools\rollprobes.ps1 -GuestPort 7791
#   .\tools\rollprobes.ps1 -GuestPort 7791 -SpawnBase 0001A2B3 -HostAddress <host's address> -RecycleRef 0001F0B1
#
# What it checks (PASS, FAIL, or SKIP with the reason), each with the lines it read:
#  1 install     rollHook=hooked and rollFaults=0 in both games (the hooks' bytes and slot matched 1.11.240)
#  2 level       npcLevel, npcLevelRule and legendaryMult the same in both games, npcLevel > 0 unless the rule is own
#  3 agreement   every plugin NPC near the host's player (not dead, not FF): `rolls show` the same roll in both games
#  4 differ      rollsDiffer=0 in both after the agreement pass (no runner's report differed from what a game built)
#  5 spawn copy  -SpawnBase: the host spawns a leveled NPC; the guest's copy is built from the host's roll as it is
#                placed (net status copiesRolled=built/not: built goes up, not stays)
#  6 pre-write   rollsPrewritten in the guest's game (information: it counts after walking away from rolled NPCs)
#  7 legendary   -LegendaryGlobal: that global set to 100 then 0 in both games; a fresh spawn is legendary, then not,
#                the same in both (the global is put back after)
#  8 late joiner -HostAddress: the guest leaves and joins again; its sessionRolls and npcLevel match the host's and
#                the agreement pass still holds
#  9 recycle     -RecycleRef: that NPC recycled (recycleactor) in both games; `rolls show` still the same in both

param(
	[int]$GuestPort = 7791,
	[int]$HostPort = 7790,
	[string]$SpawnBase = '',
	[string]$LegendaryGlobal = '',
	[string]$HostAddress = '',
	[string]$RecycleRef = '',
	[float]$Radius = 4000,
	[int]$SettleSeconds = 8
)

$ErrorActionPreference = 'Stop'
$devctl = Join-Path $PSScriptRoot 'devctl.ps1'
$results = [System.Collections.Generic.List[object]]::new()
$guestToken = $env:F4MP_DEV_TOKEN

function Dev([ValidateSet('host', 'guest')][string]$Game, [string]$Command) {
	$savedPort = $env:F4MP_DEV_PORT
	$savedToken = $env:F4MP_DEV_TOKEN
	try {
		if ($Game -eq 'host') {
			$env:F4MP_DEV_PORT = "$HostPort"
			$env:F4MP_DEV_TOKEN = $null  # the host's own token file
		} else {
			$env:F4MP_DEV_PORT = "$GuestPort"
			$env:F4MP_DEV_TOKEN = $guestToken
		}
		$reply = & $devctl $Command | Select-Object -Last 1
		return [string]$reply
	} finally {
		$env:F4MP_DEV_PORT = $savedPort
		$env:F4MP_DEV_TOKEN = $savedToken
	}
}

function Field([string]$Line, [string]$Name) {
	if ($Line -match "(?:^|\s)$([regex]::Escape($Name))=(\S+)") { return $Matches[1] }
	return $null
}

function Report([string]$Probe, [ValidateSet('PASS', 'FAIL', 'SKIP', 'INFO')][string]$Result, [string]$Detail) {
	$results.Add([pscustomobject]@{ Probe = $Probe; Result = $Result; Detail = $Detail })
	"{0,-5} {1,-12} {2}" -f $Result, $Probe, $Detail
}

function Counters([string]$Game) { Dev $Game 'rolls' }

# The plugin NPCs near the host's player, not dead (the agreement pass's list).
function NearbyNpcs {
	$line = Dev 'host' "actors $Radius"
	if ($line -eq 'none' -or $line -like 'error*') { return @() }
	$refs = foreach ($part in $line -split '; ') {
		if ($part -match '^([0-9A-F]{8}) .* dead=(\w+)$' -and $Matches[2] -eq 'false' -and -not $Matches[1].StartsWith('FF')) { $Matches[1] }
	}
	return @($refs)
}

# Each NPC's roll as both games built it: the part after the ref and before "session=" must be the same.
function Agreement([string]$Probe) {
	$refs = NearbyNpcs
	if ($refs.Count -eq 0) {
		Report $Probe 'SKIP' "no living plugin NPC within $Radius of the host's player"
		return
	}
	$same = 0
	$differ = @()
	$missing = @()
	foreach ($ref in $refs) {
		$hostLine = Dev 'host' "rolls show $ref"
		$guestLine = Dev 'guest' "rolls show $ref"
		if ($hostLine -like 'error*' -or $guestLine -like 'error*') {
			$missing += "$ref (host: $hostLine; guest: $guestLine)"
			continue
		}
		$hostRoll = ($hostLine -replace '\s+session=.*$', '')
		$guestRoll = ($guestLine -replace '\s+session=.*$', '')
		if ($hostRoll -eq $guestRoll) { $same++ } else { $differ += "host '$hostLine' guest '$guestLine'" }
	}
	$detail = "$same of $($refs.Count) the same"
	if ($missing.Count) { $detail += "; not in one game: $($missing -join ' | ')" }
	if ($differ.Count) {
		Report $Probe 'FAIL' "$detail; differ: $($differ -join ' | ')"
	} else {
		Report $Probe 'PASS' $detail
	}
}

function Wait-Enter([string]$What) {
	Read-Host "$What, then press Enter" | Out-Null
}

# ---- 1 install --------------------------------------------------------------------------------------------------
$hostCounters = Counters 'host'
$guestCounters = Counters 'guest'
"host:  $hostCounters"
"guest: $guestCounters"
$installed = (Field $hostCounters 'rollHook') -eq 'hooked' -and (Field $guestCounters 'rollHook') -eq 'hooked'
$noFaults = (Field $hostCounters 'rollFaults') -eq '0' -and (Field $guestCounters 'rollFaults') -eq '0'
Report 'install' $(if ($installed -and $noFaults) { 'PASS' } else { 'FAIL' }) "host rollHook=$(Field $hostCounters 'rollHook') rollFaults=$(Field $hostCounters 'rollFaults'); guest rollHook=$(Field $guestCounters 'rollHook') rollFaults=$(Field $guestCounters 'rollFaults')"
if (-not $installed) {
	"The hooks are not installed: the plugin log says why ('RollHook:'). The other probes need them."
}

# ---- 2 level ----------------------------------------------------------------------------------------------------
$rule = Field $hostCounters 'npcLevelRule'
$hostLevel = Field $hostCounters 'npcLevel'
$guestLevel = Field $guestCounters 'npcLevel'
$hostMult = Field $hostCounters 'legendaryMult'
$guestMult = Field $guestCounters 'legendaryMult'
$levelOk = $hostLevel -eq $guestLevel -and $rule -eq (Field $guestCounters 'npcLevelRule') -and ($rule -eq 'own' -or [int]$hostLevel -gt 0) -and $hostMult -eq $guestMult
Report 'level' $(if ($levelOk) { 'PASS' } else { 'FAIL' }) "host npcLevel=$hostLevel legendaryMult=$hostMult guest npcLevel=$guestLevel legendaryMult=$guestMult rule=$rule (average: the mean of both players' levels)"

# ---- 3, 4 agreement and differ ----------------------------------------------------------------------------------
Wait-Enter 'Walk both players (real movement, no teleport) to a place with leveled NPCs, within sight of each other'
Agreement 'agreement'
$hostCounters = Counters 'host'
$guestCounters = Counters 'guest'
$differOk = (Field $hostCounters 'rollsDiffer') -eq '0' -and (Field $guestCounters 'rollsDiffer') -eq '0'
Report 'differ' $(if ($differOk) { 'PASS' } else { 'FAIL' }) "host rollsDiffer=$(Field $hostCounters 'rollsDiffer') guest rollsDiffer=$(Field $guestCounters 'rollsDiffer') (the plugin log names each: 'differs from player')"

# ---- 5 spawn copy -----------------------------------------------------------------------------------------------
function CopiesRolled {
	$status = Dev 'guest' 'net'
	if ($status -match 'copiesRolled=(\d+)/(\d+)') { return @([int]$Matches[1], [int]$Matches[2]) }
	return $null
}
if ($SpawnBase) {
	$before = CopiesRolled
	$spawned = Dev 'host' "spawn $SpawnBase 600"
	Start-Sleep -Seconds $SettleSeconds
	$after = CopiesRolled
	if (-not $before -or -not $after) {
		Report 'spawn copy' 'FAIL' "no copiesRolled= in the guest's status (spawn: $spawned)"
	} else {
		$built = $after[0] - $before[0]
		$notBuilt = $after[1] - $before[1]
		$ok = $built -ge 1 -and $notBuilt -eq 0
		Report 'spawn copy' $(if ($ok) { 'PASS' } else { 'FAIL' }) "spawn: $spawned; guest copies built from the roll +$built, not +$notBuilt (the guest's log: 'built as the spawn's roll')"
	}
} else {
	Report 'spawn copy' 'SKIP' 'no -SpawnBase (a leveled NPC base, hex)'
}

# ---- 6 pre-write ------------------------------------------------------------------------------------------------
$guestCounters = Counters 'guest'
Report 'pre-write' 'INFO' "guest rollsPrewritten=$(Field $guestCounters 'rollsPrewritten') sessionRolls=$(Field $guestCounters 'sessionRolls') (walk away and back to see it rise)"

# ---- 7 legendary ------------------------------------------------------------------------------------------------
if ($LegendaryGlobal -and $SpawnBase) {
	$was = Dev 'host' "global $LegendaryGlobal"
	foreach ($chance in 100, 0) {
		foreach ($game in 'host', 'guest') { Dev $game "console set $LegendaryGlobal to $chance" | Out-Null }
		$spawned = Dev 'host' "spawn $SpawnBase 600"
		Start-Sleep -Seconds $SettleSeconds
		$ref = if ($spawned -match '([0-9A-F]{8})') { $Matches[1] } else { '' }
		$hostLine = if ($ref) { Dev 'host' "rolls show $ref" } else { 'no ref' }
		$want = if ($chance -eq 100) { 'legendary' } else { 'plain' }
		$isLegendary = $hostLine -match 'legendary'
		$ok = $ref -and (($chance -eq 100) -eq $isLegendary)
		Report "legendary $chance" $(if ($ok) { 'PASS' } else { 'FAIL' }) "want $want; spawn: $spawned; host: $hostLine (compare the guest's copy in its log: 'built as the spawn's roll')"
	}
	if ($was -match '([0-9.]+)\s*$') {
		foreach ($game in 'host', 'guest') { Dev $game "console set $LegendaryGlobal to $($Matches[1])" | Out-Null }
	}
} else {
	Report 'legendary' 'SKIP' 'needs -LegendaryGlobal (the legendary chance global''s editor id) and -SpawnBase'
}

# ---- 8 late joiner ----------------------------------------------------------------------------------------------
if ($HostAddress) {
	Dev 'guest' 'net leave' | Out-Null
	Start-Sleep -Seconds 5
	Dev 'guest' "net connect $HostAddress" | Out-Null
	Start-Sleep -Seconds ($SettleSeconds * 2)
	$hostCounters = Counters 'host'
	$guestCounters = Counters 'guest'
	$ok = (Field $hostCounters 'npcLevel') -eq (Field $guestCounters 'npcLevel') -and [int](Field $guestCounters 'sessionRolls') -ge [int](Field $hostCounters 'sessionRolls')
	Report 'late joiner' $(if ($ok) { 'PASS' } else { 'FAIL' }) "host sessionRolls=$(Field $hostCounters 'sessionRolls') npcLevel=$(Field $hostCounters 'npcLevel'); guest sessionRolls=$(Field $guestCounters 'sessionRolls') npcLevel=$(Field $guestCounters 'npcLevel')"
	Agreement 'late agree'
} else {
	Report 'late joiner' 'SKIP' 'no -HostAddress'
}

# ---- 9 recycle --------------------------------------------------------------------------------------------------
if ($RecycleRef) {
	foreach ($game in 'host', 'guest') { Dev $game "console $RecycleRef.recycleactor" | Out-Null }
	Start-Sleep -Seconds $SettleSeconds
	$hostLine = Dev 'host' "rolls show $RecycleRef"
	$guestLine = Dev 'guest' "rolls show $RecycleRef"
	$ok = ($hostLine -replace '\s+session=.*$', '') -eq ($guestLine -replace '\s+session=.*$', '') -and $hostLine -notlike 'error*'
	Report 'recycle' $(if ($ok) { 'PASS' } else { 'FAIL' }) "host '$hostLine' guest '$guestLine'"
} else {
	Report 'recycle' 'SKIP' 'no -RecycleRef'
}

""
$failed = @($results | Where-Object Result -eq 'FAIL').Count
"$($results.Count) probes, $failed failed"
exit $failed
