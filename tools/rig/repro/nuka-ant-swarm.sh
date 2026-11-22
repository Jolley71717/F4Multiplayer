#!/bin/bash
# nuka-ant-swarm.sh [phases...]: a crash to desktop whenever a flying ant swarm is met in Nuka-World, only in a session. Player
# report (on 0.20.1, two players, 25-30 hours in): every flying ant swarm they met crashed the game, never in single player,
# regular ants fine. No crash log came with it.
# The swarm is Nuka-World's DLC04_SwarmRace (a cloud of ants on its own skeleton and behaviour graph, with the race spell whose
# script dismembers it part by part as it is hit); its bases are DLC04_lvlSwarm (two placed) and DLC04_lvlSwarmAmbush (about
# twenty, each in a mound it bursts out of as a player comes near).
# What crashed (game/KillBlast.h): the console's kill crashes the engine on an actor whose torso is set to always explode but has
# no limb value, as the swarm's is, and the session lands a death from another game with that command whenever the body still
# stands here: a friend who kills their copy of our swarm first, a placed swarm that died in the other game, a death report that
# beats the health report. So the crash needs a swarm to DIE in one game with the other player near; meeting and shooting at
# them does nothing (the first run of this script, with no death, said NO), and a death by damage in its owner's game usually
# reaches the copy as health 0 first, with no kill (the second run).
# Phases (default: nuke-host nuke-vm away-host away-vm shots-host shots-vm copy-host copy-vm death-host death-vm; after each step both games must
# answer and no new crash log may be on either machine):
#   nuke-host  many swarms die in one explosion. Both players go to the cell's two mounds (a real walk; console moveto when the
#              walk does not get there, said so), each game makes SWARMS swarms round its player, and the host's player, given
#              a Fat Man and mini nukes by console, fires one with a real click at the swarms: the placed mound swarms, the
#              host's own and its copies of the VM's die together in the host's game, and each death must land in the VM's.
#              Swarms the blast left get a second mini nuke, then a Nuka grenade thrown with the real key.
#   nuke-vm    the same with the VM's player firing (the placed swarms are dead by then: its own and its copies of the host's)
#   away-host  the friend clears the swarms ahead: the VM's player goes away (console coc AWAY_CELL, Nuka-World unloads there),
#              the host's player goes to NukaWorld cell (-11,-12) by console cow, walks toward its five placed swarms (a real
#              walk) and fires up to two mini nukes at them with real clicks (any left: console damageav); then the VM's player
#              comes back by console cow: the deaths the VM's game held land as the cell loads (the session's console kill)
#   away-vm    the other way round, at NukaWorld cell (-8,-7) and its four placed swarms (AWAY2_*)
#   shots-host the host makes SWARMS swarms round itself (dev spawnenemies: made at runtime as an ambush's are, so the session
#              shares them with the friend). The VM's player shoots its copy of the first dead with real clicks (a combat
#              shotgun), the host's player shoots the second, its own; each death must land in the other game.
#   shots-vm   the same with the VM making them
#   copy-host  the VM makes a swarm; the HOST's game kills its copy of it first (console damageav on the copy), and the VM's
#              game must land that death on its own swarm (the session's console kill there)
#   copy-vm    the other way round
#   placed     (not in the default: the nuke phases take the placed swarms) the cell's two placed swarms (MOUNDS, each in its
#              mound): one dies of console damageav in the host's game, the other in the VM's; each must land in the other game
#   death-host the host makes SWARMS swarms: the first dies in the host's game of console damageav, the second of the console's
#              own kill there (the command itself, on its owner's swarm: what crashed the rig's host twice on 0.21.0)
#   death-vm   the same in the VM's game
#   ambush     both players to DLC04POIJS06's two ambush mounds: each walks to them on the pad's stick (real movement), the
#              swarms burst out and attack; both fire real clicks at them
#   spawn-host / spawn-vm   that game makes SWARMS swarms round itself; both fire real clicks at them for TIMEOUT s
# Set-up, said so where it happens: both games to the cell by console coc; a combat shotgun and shells each (console additem,
# equipitem); god mode, so the swarms' stings end no run.
# The swarm's base ids are DLCNukaWorld.esm's at load index 4 (both rig machines: Fallout4, DLCRobot, DLCCoast, DLCworkshop02,
# DLCNukaWorld), SWARM_BASE and the mound refs below; set them for another load order. SWARM_BASE can name another creature of the
# class (KillBlast.h) for the phases that make their own.
# REPRO: YES when a game crashed or stopped answering (the step named, the crash log kept in the run's folder and its path read); NO when every phase ran with both games up,
# no crash, at least one swarm died and every death landed in the other game; VOID when the scene could not be reached (no
# swarm died at all, or a death never reached the other game: not this bug, and no proof either way).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
PHASES=${*:-nuke-host nuke-vm away-host away-vm shots-host shots-vm copy-host copy-vm death-host death-vm}
CELL=${CELL:-DLC04POIJS06}
SWARM_BASE=${SWARM_BASE:-040138C2}                 # DLC04_lvlSwarm
MOUNDS=${MOUNDS:-"040560B4 040560AE"}              # DLC04_lvlSwarmAmbush refs in the cell (their mounds)
MOUND_AT=${MOUND_AT:-"-34650 6950"}                # between the two mounds (x y)
# the away phases: NukaWorld cell (-11,-12) (01003428), five placed swarms close together (four in mounds, one DLC04_lvlSwarm)
AWAY_GRID=${AWAY_GRID:-"-11 -12"}; AWAY_REFS=${AWAY_REFS:-"04053704 04053705 04053706 04053707 04053708"}
AWAY_FROM=${AWAY_FROM:-"-43300 -46600"}; AWAY_CELL=${AWAY_CELL:-SanctuaryExt}
# away-vm: NukaWorld cell (-8,-7) (01003360, one in 01003361 beside it), four placed swarms in mounds
AWAY2_GRID=${AWAY2_GRID:-"-8 -7"}; AWAY2_REFS=${AWAY2_REFS:-"04043346 04043347 040560A5 040560A0"}; AWAY2_FROM=${AWAY2_FROM:-"-31800 -29400"}
SWARMS=${SWARMS:-2}; RADIUS=${RADIUS:-350}; TIMEOUT=${TIMEOUT:-45}; LAND=${LAND:-40}
GUN=${GUN:-0014831C}; AMMO=${AMMO:-0001F673}     # a combat shotgun and its shells: a spread reaches a flying swarm
FATMAN=000BD56F; MININUKE=000E6B2E; NUKA_GRENADE=000E5750; VK_ALT=0x12
need_session

alive_both() { local w; for w in host vm; do wait_for 30 _answers "$w" || return 1; done; }
_answers() { dc "$1" status | grep -q 'ingame=true'; }
refhp() { dc "$1" "refinfo $2" | grep -oE 'hp=[-0-9.]+|dead=[a-z]+|formFlags=[0-9A-F]+' | paste -sd' ' -; }
crashes() { CH=$(crash_list_host); CV=$(crash_list_vm); }
# new_crashes [events]: a crash log on either machine that was not there before; with "events" also Fallout4.exe's Application
# Errors since the start (a fast fail writes no crash log), read at the end and when a game stops answering (they are slow)
new_crashes() {
	local nh nv eh="" ev=""; crashes; nh=$(crash_new "$CH0" "$CH"); nv=$(crash_new "$CV0" "$CV")
	[ -n "$1" ] && { eh=$(crash_event_lines "$(crash_events_host "$T0")"); ev=$(crash_event_lines "$(crash_events_vm "$T0")"); }
	NEW="${nh:+host log: $nh; }${nv:+vm log: $nv; }${eh:+host event: $eh; }${ev:+vm event: $ev; }"
	[ -n "$NEW" ]
}
# check <step>: both games answer and no crash; YES otherwise, the logs kept
check() {
	if ! alive_both; then
		sleep 10; new_crashes events
		say "$1: a game stopped answering: host '$(dc host status | cut -c1-40)', vm '$(dc vm status | cut -c1-40)'; $NEW"
		kept "$1"
		verdict YES "a game stopped answering at $1 ${NEW:+($NEW)}; $(crash_path)"
	fi
	if new_crashes; then kept "$1"; verdict YES "a crash at $1: $NEW; $(crash_path)"; fi
}
# kept <step>: the new crash log(s) and both plugin logs copied into the run's folder (crashlib's crash_keep)
kept() { A_OUT=$REPRO_OUT; C_HOST0=$CH0; C_VM0=$CV0; C_T0=$T0; say "$(crash_keep "$1" | tail -1)"; rkeep host nuka-ant-swarm 2>/dev/null; rkeep vm nuka-ant-swarm 2>/dev/null; }
# crash_path: what the kept crash log(s) say of the path: where it stopped, whether the console's kill (+5C0573) is on the stack,
# and the plugin's own frames if any (the session's kill comes through the plugin; a kill typed in the console does not)
crash_path() {
	local f o=""
	for f in "$REPRO_OUT"/crash-*/host/crash-*.log "$REPRO_OUT"/crash-*/vm/crash-*.log; do
		[ -e "$f" ] || continue
		o="$o$(basename "$(dirname "$f")") $(basename "$f"): $(grep -m1 -oE 'EXCEPTION_[A-Z_]+" at 0x[0-9A-F]+ [A-Za-z0-9_.]+\+[0-9A-F]+' "$f" | tr -d '"')"
		grep -qE '\]\[P\].*Fallout4\.exe\+05C0573' "$f" && o="$o; the console's kill (+5C0573) is on the stack"
		o="$o; plugin frames: $(grep -E '\]\[P\].*F4Multiplayer\.dll' "$f" | head -3 | sed -E 's/^[[:space:]]*//' | cut -c1-110 | paste -sd'|' -)$(grep -qE '\]\[P\].*F4Multiplayer\.dll' "$f" || echo none). "
	done
	echo "${o:-no crash log was kept}"
}
kit() {
	# a 10mm first, so a load order without the shotgun's form still leaves a gun in the hand
	dc "$1" "console player.additem 00004822 1" >/dev/null; dc "$1" "console player.additem 0001F276 300" >/dev/null; dc "$1" "console player.equipitem 00004822" >/dev/null
	dc "$1" "console player.additem $GUN 1" >/dev/null; dc "$1" "console player.additem $AMMO 300" >/dev/null
	dc "$1" "console player.equipitem $GUN" >/dev/null; dc "$1" "draw on" >/dev/null
}
# swarms_near <who>: the swarms (by name) that game lists as enemies within 3000, "id name hp" a line
swarms_near() { dc "$1" 'enemies 3000' | tr ';' '\n' | grep -iE "swarm|base=${SWARM_BASE}|base=04(0138C2|00B02C|0138BA|03B57D|03F9[3-9A-F][0-9A-F])" | head -6 | sed 's/^ *//' | cut -c1-120; }
# fight <seconds> <refs...>: both players fire real clicks at the first of those refs alive in their game, until all are dead
# or the time is up; FOUGHT: clicks fired
fight() {
	local secs=$1 end w t r any; shift; end=$((SECONDS + secs)); FOUGHT=0
	while [ $SECONDS -lt $end ]; do
		any=0
		for w in host vm; do
			t=""
			for r in "$@"; do [ "$(field "$(dc "$w" "refinfo $r")" dead)" = false ] && { t=$r; break; }; done
			[ -n "$t" ] || continue
			any=1; dc "$w" "turnto $t 300 aim" >/dev/null; dc "$w" focus >/dev/null; dc "$w" 'click 80' >/dev/null; FOUGHT=$((FOUGHT + 1))
		done
		[ "$any" = 1 ] || return 0
		sleep 0.7
	done
}
MET=0; DIED=0; LANDED=0; SHOT=0; SHOT_TRIED=0
# copy_of <who> <owner's spawn id>: that game's own reference for the other game's spawn (its log's "placed ... as our copy of"
# line since the run's mark), or empty
copy_of() { rsince "$1" | grep -oE "Spawns: placed [0-9A-F]{8} .* as our copy of player [0-9]+'s $2" | tail -1 | awk '{print $3}'; }
_dead() { [ "$(field "$(dc "$1" "refinfo $2")" dead)" = true ]; }
# kill_lines <who> <ref>: what that game's log says of the reference's death since the run's mark (the session's kill, the
# blast left out, a health report of 0), "|"-joined
kill_lines() { rsince "$1" | grep -E "(Spawns|WorldSync): killed $2|KillBlasts: $2|our copy $2 .* to 0\.0 health" | cut -c16-170 | paste -sd'|' -; }
# landed <step> <game it died in> <its ref there> <the other game> <its ref there>: the death (seen in the first game) must land
# in the other within LAND s, both games up. DIED and LANDED count
landed() {
	local step=$1 a=$2 ra=$3 b=$4 rb=$5
	DIED=$((DIED + 1)); sleep 3; check "$step: 3 s after the death in $a's game"
	[ -n "$rb" ] || { say "$step: $b's reference for it is not known (no copy line in its log): the landing can't be read"; return 1; }
	if wait_for "$LAND" _dead "$b" "$rb"; then
		LANDED=$((LANDED + 1)); say "$step: the death landed in $b's game: $rb $(refhp "$b" "$rb"); $b's log: $(kill_lines "$b" "$rb")"
	else
		say "$step: the death did NOT land in $b's game in $LAND s: $rb $(refhp "$b" "$rb")"
	fi
	check "$step: the death landed"
}
# shoot <who> <ref> <seconds>: that player fires real clicks at the reference, turned to it before each, until it is dead in
# that game or the time is up (0 = dead). SHOTS: clicks fired
shoot() {
	local w=$1 r=$2 end=$((SECONDS + $3)); SHOTS=0
	while [ $SECONDS -lt $end ]; do
		_dead "$w" "$r" && return 0
		[ -n "$(dc "$w" status)" ] || return 1
		dc "$w" "turnto $r 250 aim" >/dev/null; dc "$w" focus >/dev/null; sleep 0.3; dc "$w" 'click 80' >/dev/null; SHOTS=$((SHOTS + 1)); sleep 0.6
	done
	_dead "$w" "$r"
}
_near_mounds() { local d; d=$(dist_xy "$(pos_of "$1")" "$MOUND_AT"); is_int "$d" && [ "$d" -lt 700 ]; }
# tally <phase> <shooter's game> <other game>: of the targets in TARGETS ("<ref in the shooter's game>:<ref in the other
# game>:<kind>") not counted yet, those dead in the shooter's game now are counted (DIED, BLAST: this call's), and each must
# land in the other game (LANDED). LEFT: the targets still alive in the shooter's game
tally() {
	local phase=$1 a=$2 b=$3 t ra rb kind new=""
	BLAST=0; LEFT=""
	for t in $TARGETS; do
		IFS=: read -r ra rb kind <<<"$t"
		case " $COUNTED " in *" $ra "*) continue;; esac
		[ -n "$ra" ] || continue
		if _dead "$a" "$ra"; then new="$new $t"; COUNTED="$COUNTED $ra"; BLAST=$((BLAST + 1)); DIED=$((DIED + 1))
		elif grep -q 'dead=false' <<<"$(refhp "$a" "$ra")"; then LEFT="$LEFT $ra"; fi
	done
	say "$phase: $BLAST swarm(s) died in $a's game: [$(for t in $new; do IFS=: read -r ra rb kind <<<"$t"; echo -n "$ra ($kind) "; done)]; still alive there: [${LEFT# }]"
	for t in $new; do
		IFS=: read -r ra rb kind <<<"$t"
		if [ -z "$rb" ]; then say "$phase: $ra ($kind): $b's reference for it is not known; the landing can't be read"; continue; fi
		if wait_for "$LAND" _dead "$b" "$rb"; then
			LANDED=$((LANDED + 1)); say "$phase: $ra ($kind) landed in $b's game as $rb: $b's log: $(kill_lines "$b" "$rb"); $a's log: $(kill_lines "$a" "$ra")"
		else
			say "$phase: $ra ($kind) did NOT land in $b's game in $LAND s: $rb $(refhp "$b" "$rb")"
		fi
	done
	check "$phase: the deaths landed"
}
# make_swarms <who> <n>: IDS = the swarms that game made round its player (alive there), after 8 s for the copies
make_swarms() {
	local out r; IDS=""
	out=$(dc "$1" "spawnenemies $SWARM_BASE $2 $RADIUS")
	say "$phase: $1 made $2 swarm(s) round itself: $(cut -c1-160 <<<"$out")"
	sleep 8; check "$phase: 8 s after the spawn"
	for r in $(grep -oE 'ids=[^ ]*' <<<"$out" | cut -d= -f2 | tr ',' ' '); do grep -q 'dead=false' <<<"$(refhp "$1" "$r")" && { IDS="$IDS $r"; MET=$((MET + 1)); }; done
	[ -n "$IDS" ]
}

# ---- set-up ------------------------------------------------------------------------------------------------------------------------
say "nuka-ant-swarm: phases $PHASES; cell $CELL; swarm base $SWARM_BASE; mounds $MOUNDS; out $REPRO_OUT"
say "games: host $(dc host 'net status' | grep -oE 'status=[a-z]+|hash=[0-9A-F]+' | tr '\n' ' ') | vm $(dc vm 'net status' | grep -oE 'status=[a-z]+|hash=[0-9A-F]+' | tr '\n' ' ')"
crashes; CH0=$CH; CV0=$CV; T0=$(date +%s)
[ -n "$CH0" ] && [ -n "$CV0" ] || verdict VOID "a crash folder could not be read (host '$(head -1 <<<"$CH0")', vm '$(head -1 <<<"$CV0")')"
say "crash logs before: host $(crash_count "$CH0") (newest $(crash_stamp "$CH0")), vm $(crash_count "$CV0") (newest $(crash_stamp "$CV0"))"
rmark host; rmark vm
say "set-up: both games to $CELL by console coc; a gun ($GUN) and ammo each by console; god mode"
enter_both "$CELL" 16
for w in host vm; do _answers "$w" || verdict VOID "$w is not in the game after the coc to $CELL"; dc "$w" 'god on' >/dev/null; kit "$w"; done
say "arrived: host $(pos_of host) vm $(pos_of vm); in the hand: host [$(dc host equipped | cut -c1-90)] vm [$(dc vm equipped | cut -c1-90)]"
check "set-up"

for phase in $PHASES; do
	case $phase in
	ambush)
		say "ambush: mounds before: host [$(for m in $MOUNDS; do echo -n "$m $(refhp host $m); "; done)] vm [$(for m in $MOUNDS; do echo -n "$m $(refhp vm $m); "; done)]"
		for w in host vm; do
			# shellcheck disable=SC2086
			rwalk "$w" $MOUND_AT 150 0.8; say "ambush: $w walked to the mounds (pad): $WALKED"
			check "ambush: $w's walk"
		done
		sleep 8; check "ambush: 8 s at the mounds"
		say "ambush: swarms near: host [$(swarms_near host | paste -sd'|' -)] vm [$(swarms_near vm | paste -sd'|' -)]"
		say "ambush: mounds now: host [$(for m in $MOUNDS; do echo -n "$m $(refhp host $m); "; done)] vm [$(for m in $MOUNDS; do echo -n "$m $(refhp vm $m); "; done)]"
		for m in $MOUNDS; do grep -q 'dead=' <<<"$(refhp host $m)" && MET=$((MET + 1)); done
		# shellcheck disable=SC2086
		fight "$TIMEOUT" $MOUNDS; say "ambush: fought the mound swarms, $FOUGHT clicks; now host [$(for m in $MOUNDS; do echo -n "$m $(refhp host $m); "; done)] vm [$(for m in $MOUNDS; do echo -n "$m $(refhp vm $m); "; done)]"
		check "ambush: the fight"
		sleep 10; check "ambush: 10 s after the fight"
		;;
	nuke-host|nuke-vm)
		who=${phase#nuke-}; other=$(other_game "$who"); first=${MOUNDS%% *}
		for w in host vm; do
			_near_mounds "$w" && continue
			# shellcheck disable=SC2086
			rwalk "$w" $MOUND_AT 250 0.8; say "$phase: $w walked to the mounds (pad): $WALKED"
			if ! _near_mounds "$w"; then
				dc "$w" "console player.moveto $first" >/dev/null; sleep 6
				say "$phase: $w was put at the mound $first by console moveto (its walk did not get there): now at $(pos_of "$w")"
			fi
			check "$phase: $w at the mounds"
		done
		sleep 5; check "$phase: 5 s at the mounds"
		make_swarms "$who" "$SWARMS"; own=$IDS
		make_swarms "$other" "$SWARMS"; theirs=$IDS
		TARGETS=""; COUNTED=""
		for m in $MOUNDS; do grep -q 'dead=false' <<<"$(refhp "$who" "$m")" && { TARGETS="$TARGETS $m:$m:placed"; MET=$((MET + 1)); }; done
		for r in $own; do TARGETS="$TARGETS $r:$(copy_of "$other" "$r"):own"; done
		for r in $theirs; do TARGETS="$TARGETS $(copy_of "$who" "$r"):$r:copy"; done
		say "$phase: the swarms in $who's game (its ref:$other's ref:kind): [${TARGETS# }]"
		say "$phase: $who's player gets a Fat Man and 6 mini nukes by console"
		dc "$who" "console player.additem $FATMAN 1" >/dev/null; dc "$who" "console player.additem $MININUKE 6" >/dev/null
		dc "$who" "console player.equipitem $FATMAN" >/dev/null; dc "$who" "draw on" >/dev/null; sleep 4
		say "$phase: in $who's hand: $(dc "$who" equipped | cut -c1-120)"
		LEFT=""; for t in $TARGETS; do LEFT="$LEFT ${t%%:*}"; done
		for try in 1 2; do
			aim=""; for r in $LEFT; do grep -q 'dead=false' <<<"$(refhp "$who" "$r")" && { aim=$r; break; }; done
			[ -n "$aim" ] || break
			dc "$who" "turnto $aim 300 aim" >/dev/null; dc "$who" focus >/dev/null; sleep 0.4; dc "$who" 'click 150' >/dev/null
			say "$phase: $who's player fired mini nuke $try with a real click, turned to $aim"
			sleep 8; check "$phase: 8 s after $who's mini nuke $try"
			tally "$phase" "$who" "$other"
			say "$phase: mini nuke $try killed $BLAST swarm(s) in one blast"
			[ -n "$LEFT" ] || break
			sleep 3
		done
		if [ -n "$LEFT" ]; then
			aim=${LEFT# }; aim=${aim%% *}
			say "$phase: ${LEFT# } still alive: $who's player gets Nuka grenades by console and throws one (the real key, held) turned to $aim"
			dc "$who" "console player.additem $NUKA_GRENADE 3" >/dev/null; dc "$who" "console player.equipitem $NUKA_GRENADE" >/dev/null; sleep 1.5
			dc "$who" "turnto $aim 300 aim" >/dev/null; rkey "$who" "$VK_ALT" 700
			sleep 8; check "$phase: 8 s after $who's Nuka grenade"
			tally "$phase" "$who" "$other"
			say "$phase: the Nuka grenade killed $BLAST swarm(s)"
		fi
		sleep 5; check "$phase: 5 s after the blasts"
		for r in $own; do dc "$who" "remove $r" >/dev/null; done; for r in $theirs; do dc "$other" "remove $r" >/dev/null; done
		sleep 3; check "$phase: the swarms removed"
		;;
	away-host|away-vm)
		# the friend clears the swarms ahead while the other player is away; the deaths wait in the other game until the cell
		# loads there, and land then (WorldSync's held deaths: the session's console kill as the swarms load)
		who=${phase#away-}; other=$(other_game "$who")
		if [ "$who" = vm ]; then GRID=$AWAY2_GRID; REFS=$AWAY2_REFS; FROM=$AWAY2_FROM; else GRID=$AWAY_GRID; REFS=$AWAY_REFS; FROM=$AWAY_FROM; fi
		say "$phase: $other's player goes away by console coc $AWAY_CELL (Nuka-World unloads in its game); $who's player to NukaWorld $GRID by console cow"
		dc "$other" "console coc $AWAY_CELL" >/dev/null; sleep 16; wait_ingame "$other" 30
		dc "$who" "console cow NukaWorld $GRID" >/dev/null; sleep 16; wait_ingame "$who" 30
		check "$phase: $other away, $who at $GRID"
		# shellcheck disable=SC2086
		rwalk "$who" $FROM 250 0.8; say "$phase: $who walked toward the swarms (pad): $WALKED; at $(pos_of "$who")"
		check "$phase: $who's walk"
		TARGETS=""
		for r in $REFS; do grep -q 'dead=false' <<<"$(refhp "$who" "$r")" && { TARGETS="$TARGETS $r"; MET=$((MET + 1)); }; done
		say "$phase: placed swarms alive in $who's game: [${TARGETS# }] (of $REFS); in $other's game now: [$(for r in $REFS; do echo -n "$r $(refhp "$other" "$r"); "; done)]"
		[ -n "$TARGETS" ] || { say "$phase: none alive; skipped"; continue; }
		say "$phase: $who's player gets a Fat Man and 6 mini nukes by console"
		dc "$who" "console player.additem $FATMAN 1" >/dev/null; dc "$who" "console player.additem $MININUKE 6" >/dev/null
		dc "$who" "console player.equipitem $FATMAN" >/dev/null; dc "$who" "draw on" >/dev/null; sleep 4
		HELD=""
		for try in 1 2; do
			aim=""; for r in $TARGETS; do case " $HELD " in *" $r "*) continue;; esac; grep -q 'dead=false' <<<"$(refhp "$who" "$r")" && { aim=$r; break; }; done
			[ -n "$aim" ] || break
			dc "$who" "turnto $aim 300 aim" >/dev/null; dc "$who" focus >/dev/null; sleep 0.4; dc "$who" 'click 150' >/dev/null
			sleep 8; check "$phase: 8 s after $who's mini nuke $try"
			n=0; for r in $TARGETS; do case " $HELD " in *" $r "*) continue;; esac; _dead "$who" "$r" && { HELD="$HELD $r"; n=$((n + 1)); DIED=$((DIED + 1)); }; done
			say "$phase: $who's mini nuke $try (a real click, turned to $aim) killed $n placed swarm(s) in one blast"
		done
		for r in $TARGETS; do
			case " $HELD " in *" $r "*) continue;; esac
			dc "$who" "console $r.damageav health 5000" >/dev/null
			wait_for 10 _dead "$who" "$r" && { HELD="$HELD $r"; DIED=$((DIED + 1)); say "$phase: $r (left by the blasts) killed in $who's game by console damageav"; }
		done
		sleep 3; check "$phase: the deaths in $who's game"
		say "$phase: dead in $who's game: [${HELD# }]; $other's player comes back by console cow NukaWorld $GRID: the cell loads there and the held deaths land as it does"
		dc "$other" "console cow NukaWorld $GRID" >/dev/null; sleep 16; wait_ingame "$other" 30
		check "$phase: $other back at the swarms"
		for r in $HELD; do
			if wait_for "$LAND" _dead "$other" "$r"; then
				LANDED=$((LANDED + 1)); say "$phase: $r landed in $other's game as it loaded: $(refhp "$other" "$r"); $other's log: $(kill_lines "$other" "$r")"
			else
				say "$phase: $r did NOT land in $other's game in $LAND s: $(refhp "$other" "$r"); $other's log: $(kill_lines "$other" "$r")"
			fi
		done
		sleep 5; check "$phase: the deaths landed"
		;;
	shots-host|shots-vm)
		who=${phase#shots-}; other=$(other_game "$who")
		for w in host vm; do dc "$w" "console player.equipitem $GUN" >/dev/null; dc "$w" "draw on" >/dev/null; done; sleep 3
		make_swarms "$who" "$SWARMS" || { say "$phase: nothing made; skipped"; continue; }
		n=0
		for r in $IDS; do
			n=$((n + 1)); copy=$(copy_of "$other" "$r"); SHOT_TRIED=$((SHOT_TRIED + 1))
			if [ $n = 1 ] && [ -n "$copy" ]; then
				# the friend shoots its copy of the owner's swarm
				shoot "$other" "$copy" "$TIMEOUT"; rc=$?; say "$phase: $other's player fired $SHOTS real clicks at its copy $copy of $who's $r: $([ $rc = 0 ] && echo dead there || echo "not dead there: $(refhp "$other" "$copy")")"
				check "$phase: $other's shots at $copy"
				if [ $rc = 0 ]; then SHOT=$((SHOT + 1)); landed "$phase: $other shot its copy $copy of $r" "$other" "$copy" "$who" "$r"
				elif _dead "$who" "$r"; then SHOT=$((SHOT + 1)); say "$phase: $r died in $who's game of $other's shots"; landed "$phase: $other's shots killed $r in $who's game" "$who" "$r" "$other" "$copy"; fi
			else
				# the owner shoots its own swarm
				shoot "$who" "$r" "$TIMEOUT"; rc=$?; say "$phase: $who's player fired $SHOTS real clicks at its own $r: $([ $rc = 0 ] && echo dead || echo "not dead: $(refhp "$who" "$r")")"
				check "$phase: $who's shots at $r"
				[ $rc = 0 ] && { SHOT=$((SHOT + 1)); landed "$phase: $who shot its own $r" "$who" "$r" "$other" "$copy"; }
			fi
		done
		sleep 5; check "$phase: 5 s after the shots"
		for r in $IDS; do dc "$who" "remove $r" >/dev/null; done
		sleep 3; check "$phase: the swarms removed"
		;;
	copy-host|copy-vm)
		# the game named kills its COPY of the other game's swarm first
		who=${phase#copy-}; other=$(other_game "$who")
		make_swarms "$other" 1 || { say "$phase: nothing made; skipped"; continue; }
		r=${IDS# }; copy=$(copy_of "$who" "$r")
		[ -n "$copy" ] || { say "$phase: $who's game has no copy of $other's $r in its log; skipped"; dc "$other" "remove $r" >/dev/null; continue; }
		say "$phase: $who's copy $copy of $other's $r is killed in $who's game by console damageav (a copy killed here first)"
		dc "$who" "console $copy.damageav health 5000" >/dev/null
		if wait_for 15 _dead "$who" "$copy"; then landed "$phase: $who killed its copy $copy of $r" "$who" "$copy" "$other" "$r"
		else check "$phase: the copy's death"; say "$phase: the copy did not die in $who's game: $(refhp "$who" "$copy") (the owner's health reports keep it?)"; fi
		sleep 5; check "$phase: 5 s after"
		dc "$other" "remove $r" >/dev/null; sleep 3; check "$phase: the swarm removed"
		;;
	placed)
		# the cell's placed swarms: the same reference in both games
		n=0
		for m in $MOUNDS; do
			n=$((n + 1)); who=host; [ $n = 2 ] && who=vm; other=$(other_game "$who")
			say "$phase: $m before: host [$(refhp host "$m")] vm [$(refhp vm "$m")]"
			grep -q 'dead=false' <<<"$(refhp "$who" "$m")" || { say "$phase: $m is not alive in $who's game; skipped"; continue; }
			MET=$((MET + 1)); say "$phase: the placed swarm $m is killed in $who's game by console damageav"
			dc "$who" "console $m.damageav health 5000" >/dev/null
			if wait_for 15 _dead "$who" "$m"; then landed "$phase: $m died in $who's game" "$who" "$m" "$other" "$m"
			else check "$phase: $m's death"; say "$phase: $m did not die in $who's game: $(refhp "$who" "$m")"; fi
		done
		sleep 5; check "$phase: 5 s after"
		;;
	death-host|death-vm)
		who=${phase#death-}; other=$(other_game "$who")
		make_swarms "$who" "$SWARMS" || { say "$phase: nothing made; skipped"; continue; }
		n=0
		for r in $IDS; do
			n=$((n + 1)); copy=$(copy_of "$other" "$r")
			if [ $n = 1 ]; then how="console damageav"; dc "$who" "console $r.damageav health 5000" >/dev/null
			else how="the console's own kill"; dc "$who" "console $r.kill" >/dev/null; fi
			say "$phase: $r is killed in $who's game by $how"
			if wait_for 15 _dead "$who" "$r"; then landed "$phase: $r died in $who's game ($how)" "$who" "$r" "$other" "$copy"
			else check "$phase: $r's death ($how)"; say "$phase: $r did not die in $who's game ($how): $(refhp "$who" "$r")"; fi
		done
		sleep 5; check "$phase: 5 s after the deaths"
		for r in $IDS; do dc "$who" "remove $r" >/dev/null; done
		sleep 3; check "$phase: the swarms removed"
		;;
	spawn-host|spawn-vm)
		who=${phase#spawn-}; other=$(other_game "$who")
		out=$(dc "$who" "spawnenemies $SWARM_BASE $SWARMS $RADIUS"); ids=$(grep -oE 'ids=[^ ]*' <<<"$out" | cut -d= -f2 | tr ',' ' ')
		say "$phase: $who made $SWARMS swarms round itself: $(cut -c1-160 <<<"$out")"
		[ -n "$ids" ] || { say "$phase: nothing made; skipped"; continue; }
		sleep 6; check "$phase: 6 s after the spawn"
		say "$phase: in $who's game [$(for r in $ids; do echo -n "$r $(refhp $who $r); "; done)]; the friend sees: [$(swarms_near $other | paste -sd'|' -)]"
		for r in $ids; do grep -q 'dead=' <<<"$(refhp $who $r)" && MET=$((MET + 1)); done
		# a step of real movement by each player while the swarms are on them (W, then S: the pad is not needed for a step)
		for w in host vm; do rkey "$w" 0x57 600; sleep 0.8; rkey "$w" 0x53 600; done
		check "$phase: both stepped"
		# shellcheck disable=SC2086
		fight "$TIMEOUT" $ids; say "$phase: fought, $FOUGHT clicks; now [$(for r in $ids; do echo -n "$r $(refhp $who $r); "; done)]"
		check "$phase: the fight"
		sleep 10; check "$phase: 10 s after the fight"
		for r in $ids; do dc "$who" "remove $r" >/dev/null; done
		sleep 3; check "$phase: the swarms removed"
		;;
	*) say "unknown phase $phase";;
	esac
done
new_crashes events && { kept "the end"; verdict YES "a crash during the run: $NEW; $(crash_path)"; }
rsince host > "$REPRO_OUT/run-host.log"; rsince vm > "$REPRO_OUT/run-vm.log"
say "swarm lines in the logs: host $(grep -ciE 'swarm|0138C2|0B02C|138BA|3B57D' "$REPRO_OUT/run-host.log"), vm $(grep -ciE 'swarm|0138C2|0B02C|138BA|3B57D' "$REPRO_OUT/run-vm.log")"
say "kill lines in the logs: host $(grep -cE '(Spawns|WorldSync): killed|KillBlasts: ' "$REPRO_OUT/run-host.log"), vm $(grep -cE '(Spawns|WorldSync): killed|KillBlasts: ' "$REPRO_OUT/run-vm.log"); net status: host [$(dc host 'net status' | grep -oE 'killBlast[A-Za-z]*=[a-z0-9]+|killsRefused=[0-9]+' | paste -sd' ' -)] vm [$(dc vm 'net status' | grep -oE 'killBlast[A-Za-z]*=[a-z0-9]+|killsRefused=[0-9]+' | paste -sd' ' -)]"
[ "$MET" -gt 0 ] || verdict VOID "no swarm was met in any phase"
SUM="$MET swarms met over the phases ($PHASES); $DIED died ($SHOT of $SHOT_TRIED tried were shot dead with real clicks), $LANDED of those deaths landed in the other game"
[ "$DIED" -gt 0 ] || verdict VOID "no crash, but no swarm died: $SUM"
[ "$LANDED" -ge "$DIED" ] || verdict VOID "no crash, but a death did not reach the other game: $SUM"
verdict NO "$SUM, and neither game crashed"
