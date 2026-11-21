#!/bin/bash
# nuka-ant-swarm.sh [phases...]: a crash to desktop whenever a flying ant swarm is met in Nuka-World, only in a session. Player
# report (on 0.20.1, two players, 25-30 hours in): every flying ant swarm they met crashed the game, never in single player,
# regular ants fine. No crash log came with it.
# The swarm is Nuka-World's DLC04_SwarmRace (a cloud of ants on its own skeleton and behaviour graph, with the race spell whose
# script dismembers it part by part as it is hit); its bases are DLC04_lvlSwarm (two placed) and DLC04_lvlSwarmAmbush (about
# twenty, each in a mound it bursts out of as a player comes near).
# What crashed (game/KillBlast.h): a death in one game is landed on the other game's copy with the console's kill, and that
# command crashes the engine on an actor whose torso is set to always explode but has no limb value, as the swarm's is. So the
# crash needs a swarm to DIE in one game with the other player near; meeting and shooting them does nothing (the first run of
# this script, with no death, said NO).
# Phases (default: death-host death-vm; after each step both games must answer and no new crash log may be on either machine):
#   death-host the host makes SWARMS swarms round itself (dev spawnenemies: made at runtime as an ambush's are, so the session
#              shares them with the friend). The first dies in the host's game of console damageav (a death there with no
#              console kill: the dev channel's single clicks never killed a flying swarm in 45 s), and the VM's game must land
#              that death on its copy (the mod's console kill) and go on running. The second dies of the console's own kill in
#              the host's game (the command itself, on its owner's swarm), and again both games must go on.
#   death-vm   the same with the VM making them (the host gets the copies)
#   ambush     both players to DLC04POIJS06's two ambush mounds: each walks to them on the pad's stick (real movement), the
#              swarms burst out and attack; both fire real clicks at them
#   spawn-host / spawn-vm   that game makes SWARMS swarms round itself; both fire real clicks at them for TIMEOUT s
# Set-up, said so where it happens: both games to the cell by console coc; a 10mm and ammo each (console additem, equipitem); god
# mode, so the swarms' stings end no run.
# The swarm's base ids are DLCNukaWorld.esm's at load index 4 (both rig machines: Fallout4, DLCRobot, DLCCoast, DLCworkshop02,
# DLCNukaWorld), SWARM_BASE and the mound refs below; set them for another load order. SWARM_BASE can name another creature of the
# class (KillBlast.h) for the death phases.
# REPRO: YES when a game crashed or stopped answering (the step and the logs named); NO when every phase ran with both games up,
# no crash, and (death phases) each death landed on the other game's copy; VOID when the scene could not be reached (no swarm
# met, or a death that never reached the other game: not this bug, and no proof either way).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
PHASES=${*:-death-host death-vm}
CELL=${CELL:-DLC04POIJS06}
SWARM_BASE=${SWARM_BASE:-040138C2}                 # DLC04_lvlSwarm
MOUNDS=${MOUNDS:-"040560B4 040560AE"}              # DLC04_lvlSwarmAmbush refs in the cell (their mounds)
MOUND_AT=${MOUND_AT:-"-34650 6950"}                # between the two mounds (x y)
SWARMS=${SWARMS:-2}; RADIUS=${RADIUS:-350}; TIMEOUT=${TIMEOUT:-45}; LAND=${LAND:-40}
PISTOL=00004822; AMMO=0001F276
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
		rkeep host nuka-ant-swarm; rkeep vm nuka-ant-swarm
		verdict YES "a game stopped answering at $1 ${NEW:+($NEW)}"
	fi
	if new_crashes; then rkeep host nuka-ant-swarm; rkeep vm nuka-ant-swarm; verdict YES "a crash at $1: $NEW"; fi
}
kit() {
	dc "$1" "console player.additem $PISTOL 1" >/dev/null; dc "$1" "console player.additem $AMMO 300" >/dev/null
	dc "$1" "console player.equipitem $PISTOL" >/dev/null; dc "$1" "draw on" >/dev/null
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
MET=0; LANDED=0
# copy_of <who> <owner's spawn id>: that game's own reference for the other game's spawn (its log's "placed ... as our copy of"
# line since the run's mark), or empty
copy_of() { rsince "$1" | grep -oE "Spawns: placed [0-9A-F]{8} .* as our copy of player [0-9]+'s $2" | tail -1 | awk '{print $3}'; }
_dead() { [ "$(field "$(dc "$1" "refinfo $2")" dead)" = true ]; }
# landed <phase> <owner> <other> <id> <how it died>: the death must show in the owner's game and land on the other game's copy
# within LAND s, both games up; LANDED counts the deaths that did
landed() {
	local phase=$1 who=$2 other=$3 id=$4 how=$5 copy
	copy=$(copy_of "$other" "$id")
	if ! wait_for 15 _dead "$who" "$id"; then
		check "$phase: $id's death ($how)"; say "$phase: $id did not die in $who's game ($how): $(refhp "$who" "$id")"; return 1
	fi
	say "$phase: $id died in $who's game ($how); $other's copy of it: ${copy:-none found in its log}"
	sleep 4; check "$phase: 4 s after $id's death ($how)"
	[ -n "$copy" ] || return 1
	if wait_for "$LAND" _dead "$other" "$copy"; then
		LANDED=$((LANDED + 1))
		say "$phase: the death landed in $other's game: $copy $(refhp "$other" "$copy"); its log: $(rsince "$other" | grep -E "Spawns: killed $copy|KillBlasts: $copy" | cut -c16-200 | paste -sd'|' -)"
	else
		say "$phase: the death did NOT land in $other's game in $LAND s: $copy $(refhp "$other" "$copy")"
	fi
	check "$phase: $id's death landed ($how)"
}

# ---- set-up ------------------------------------------------------------------------------------------------------------------------
say "nuka-ant-swarm: phases $PHASES; cell $CELL; swarm base $SWARM_BASE; mounds $MOUNDS; out $REPRO_OUT"
say "games: host $(dc host 'net status' | grep -oE 'status=[a-z]+|hash=[0-9A-F]+' | tr '\n' ' ') | vm $(dc vm 'net status' | grep -oE 'status=[a-z]+|hash=[0-9A-F]+' | tr '\n' ' ')"
crashes; CH0=$CH; CV0=$CV; T0=$(date +%s)
[ -n "$CH0" ] && [ -n "$CV0" ] || verdict VOID "a crash folder could not be read (host '$(head -1 <<<"$CH0")', vm '$(head -1 <<<"$CV0")')"
say "crash logs before: host $(crash_count "$CH0") (newest $(crash_stamp "$CH0")), vm $(crash_count "$CV0") (newest $(crash_stamp "$CV0"))"
rmark host; rmark vm
say "set-up: both games to $CELL by console coc; a 10mm and ammo each by console; god mode"
enter_both "$CELL" 16
for w in host vm; do _answers "$w" || verdict VOID "$w is not in the game after the coc to $CELL"; dc "$w" 'god on' >/dev/null; kit "$w"; done
say "arrived: host $(pos_of host) vm $(pos_of vm); base $SWARM_BASE in the host's game: $(dc host "forms $SWARM_BASE" | cut -c1-160)"
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
	death-host|death-vm)
		who=${phase#death-}; other=$(other_game "$who")
		out=$(dc "$who" "spawnenemies $SWARM_BASE $SWARMS $RADIUS"); ids=$(grep -oE 'ids=[^ ]*' <<<"$out" | cut -d= -f2 | tr ',' ' ')
		say "$phase: $who made $SWARMS swarms round itself: $(cut -c1-160 <<<"$out")"
		[ -n "$ids" ] || { say "$phase: nothing made; skipped"; continue; }
		sleep 8; check "$phase: 8 s after the spawn"
		say "$phase: in $who's game [$(for r in $ids; do echo -n "$r $(refhp "$who" "$r"); "; done)]; the friend sees: [$(swarms_near "$other" | paste -sd'|' -)]"
		n=0
		for r in $ids; do
			grep -q 'dead=' <<<"$(refhp "$who" "$r")" || continue
			MET=$((MET + 1)); n=$((n + 1))
			if [ $n = 1 ]; then
				say "$phase: $r is killed in $who's game by console damageav (no console kill there)"
				dc "$who" "console $r.damageav health 5000" >/dev/null; landed "$phase" "$who" "$other" "$r" "console damageav"
			else
				say "$phase: $r is killed in $who's game by the console's own kill"
				dc "$who" "console $r.kill" >/dev/null; landed "$phase" "$who" "$other" "$r" "the console's kill"
			fi
		done
		sleep 8; check "$phase: 8 s after the deaths"
		for r in $ids; do dc "$who" "remove $r" >/dev/null; done
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
new_crashes events && { rkeep host nuka-ant-swarm; rkeep vm nuka-ant-swarm; verdict YES "a crash during the run: $NEW"; }
rsince host > "$REPRO_OUT/run-host.log"; rsince vm > "$REPRO_OUT/run-vm.log"
say "swarm lines in the logs: host $(grep -ciE 'swarm|0138C2|0B02C|138BA|3B57D' "$REPRO_OUT/run-host.log"), vm $(grep -ciE 'swarm|0138C2|0B02C|138BA|3B57D' "$REPRO_OUT/run-vm.log")"
[ "$MET" -gt 0 ] || verdict VOID "no swarm was met in any phase"
WANT=0; for phase in $PHASES; do case $phase in death-*) WANT=$((WANT + SWARMS));; esac; done
[ "$LANDED" -ge "$WANT" ] || verdict VOID "no crash, but only $LANDED of $WANT deaths landed on the other game's copy ($PHASES): the kill this bug is about was not reached every time"
verdict NO "$MET swarms met over the phases ($PHASES), $LANDED of $WANT deaths landed on the other game's copy, and neither game crashed"
