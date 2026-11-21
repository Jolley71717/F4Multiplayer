#!/bin/bash
# dc-accusation.sh [first]: Diamond City's synth accusation (FFDiamondCity12, 0014BF48) played once for the session, or once in
# each game. The quest is a run-once misc quest the story manager starts as a game's player enters Diamond City (its node
# DiamondCitySpecialSceneNode, on the change of location, while IntroSceneDiamondCitySalon's 10 is done and the quest's own 10 is
# not); its stage 10 enables Kyle (0014BF68, who holds his brother at gunpoint) and Riley (0014BF65), placed NPCs every game has,
# and creates two guards; two trigger boxes in the market set 15 and 20 as that game's own player walks in, which start its
# scenes; security shoots Kyle (30), and the quest stops (its shutdown stage 1000 deletes Kyle, Riley and the guards) once that
# player leaves Diamond City. Up to 0.20.3 every game plays its own run over its own copies of the two: a friend who arrives
# later starts the quest again in their game and walks into their own triggers.
#
# <first> (host or vm; default host) arrives first: its player comes to Diamond City and walks up to Kyle (real movement), and
# the scene is watched until Kyle is dead there (or WATCH s). Then the other player arrives (the late friend) and walks up to Kyle
# too, watched for LATE s. Every 3 s each game's side is read: the quest's done stages, Kyle's and Riley's state, and the actors
# its game has in a scene of the quest (dev scenes). Console set-up, said so where it happens: a first visit of both players to
# Diamond City and back to SanctuaryExt by coc (the salon's intro takes the first coming), each game's coc to DiamondCityOrigin
# (the market's cell), and only in the first game, when the story manager has not started the quest 20 s after it came, the
# engine's own start through `dev queststart` (the console's startquest and setstage refuse a quest the story manager starts
# from an event: this one's is the change of location; started so, with the player already in the triggers, they never fire),
# named in the output. The late friend's game is never started by hand. A throwaway coc world: the quest is run-once, so a game
# that has its 15 done is VOID.
# The story manager starts the quest on a coc from another place into Diamond City (the change of location), not on the
# first load from the main menu (no location before it): the rig's join loads both games at SanctuaryExt first. A friend's
# stand-in coming into Diamond City can start it in a game whose player is elsewhere (0.20.3, 2026-11-20: the VM's 10 done
# while its player stood in Sanctuary), which is why the late game's stages are read before its player comes.
# REPRO: YES when the late friend's game sets a stage of the scene (15, 20, 30, 40, 50) by its own play after its player
# came (its own triggers, its own scene, its own security's shot), or has actors in a scene of the quest of its own, or Kyle
# ends dead or gone in one game and standing in the other. NO when the late game had the scene's stages before its player
# came (a friend's run, followed) and set none of its own, and Kyle ends the same in both. VOID when the first game's
# triggers never set the scene's 15 or 20.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
FIRST=${1:-host}; LATE_WHO=$(other_game "$FIRST")
WATCH=${WATCH:-240}; LATE=${LATE:-150}; START_WAIT=${START_WAIT:-20}
Q=0014BF48; KYLE=0014BF68; RILEY=0014BF65; SALON=00005801; CELL=${CELL:-DiamondCityOrigin}
need_session

done_of() { dc "$1" "stages ${2:-$Q}" | sed -nE 's/.* done\([0-9]+\)=([0-9,]+|none).*/\1/p'; }
has_stage_of() { case ",$(done_of "$1" "$2")," in *",$3,"*) return 0;; esac; return 1; }
has_stage() { case ",$(done_of "$1")," in *",$2,"*) return 0;; esac; return 1; }
in_scene() {  # the actors that game has in a scene of the quest, as ref@scene, sorted ("none")
	local s
	s=$(dc "$1" "scenes 6000" | tr -d '\r' | sed 's/; /\n/g' | grep -F " quest=$Q " | sed -E 's/^([0-9A-F]+) .* scene=([0-9A-F]+) .*/\1@\2/' | sort | paste -sd, -)
	echo "${s:-none}"
}
actor_of() {  # <who> <ref>: dead=,disabled=,owner= of that reference there
	local g r; g=$(dc "$1" "ghosts $2"); r=$(dc "$1" "refinfo $2")
	echo "dead=$(field "$g" dead),disabled=$(field "$r" disabled),loaded=$(field "$g" loaded),owner=$(field "$g" owner)"
}
snap() { echo "done=$(done_of "$1") kyle:$(actor_of "$1" $KYLE) riley:$(actor_of "$1" $RILEY) inScene=$(in_scene "$1")"; }
kyle_gone() {  # <who>: Kyle dead or switched off there
	local a; a=$(actor_of "$1" $KYLE)
	grep -q 'dead=true' <<<"$a" || grep -q 'disabled=true' <<<"$a"
}
SCENES_FIRST=0; SCENES_LATE=0; T0=$SECONDS
# watch <seconds> [until-command...]: each game's side every 3 s, every change said with its second
declare -A LAST
watch() {
	local end=$((SECONDS + $1)) w s; shift
	while [ $SECONDS -lt $end ]; do
		for w in host vm; do
			s=$(snap "$w")
			if [ "$s" != "${LAST[$w]}" ]; then say "+$((SECONDS - T0))s $w: $s"; LAST[$w]=$s; fi
			case "$s" in *"inScene=none"*) ;; *) if [ "$w" = "$FIRST" ]; then SCENES_FIRST=$((SCENES_FIRST + 1)); else SCENES_LATE=$((SCENES_LATE + 1)); fi;; esac
		done
		[ $# -gt 0 ] && "$@" && return 0
		sleep 3
	done
	return 1
}
# arrive <who> [start]: that game's player comes to the market's cell (console coc, said) and the quest is started there by the
# story manager, or (with start) by the engine's start through dev queststart when it has not after START_WAIT s (said)
arrive() {
	local w=$1 force=${2:-}
	say "set-up: $w comes to Diamond City by console (coc $CELL)"
	dc "$w" focus >/dev/null; dc "$w" "console coc $CELL" >/dev/null; sleep 8
	wait_ingame "$w" 60 || verdict VOID "$w did not come back into the game after its coc"
	if wait_for "$START_WAIT" has_stage "$w" 10; then
		say "$w: the quest started on its own as the player came (its 10 done): $(done_of "$w")"; STARTED[$w]=story
	else
		STARTED[$w]=none
		say "$w: the story manager did not start the quest in $START_WAIT s of coming (done=$(done_of "$w")); its node's conditions there: MQ104 130 done=$(has_stage_of "$w" 0001F25E 130 && echo yes || echo no), FFDiamondCity10 5 done=$(has_stage_of "$w" 001764DF 5 && echo yes || echo no), the salon's 10 done=$(has_stage_of "$w" $SALON 10 && echo yes || echo no)"
		if [ -n "$force" ]; then
			say "set-up: $w: the quest started by the engine's start (dev queststart, console set-up): $(dc "$w" "queststart $Q" | tr -d '\r' | cut -c1-200)"
			if wait_for 10 has_stage "$w" 10; then STARTED[$w]=console; say "$w: started (its 10 done): $(done_of "$w")"; fi
		fi
	fi
}
declare -A STARTED
for w in host vm; do
	has_stage "$w" 15 && verdict VOID "$w has the quest's 15 done already (a throwaway world from before the market's first visit)"
	rmark "$w" run
done
# The first visit: Diamond City's salon intro (IntroSceneDiamondCitySalon) takes the story manager's change of location on
# a player's first coming (0.20.3, 2026-11-20: a coc straight from Sanctuary with the salon's 10 set by console started no
# accusation; after a first visit, the next coming did). Both players come by console (coc, said) and leave again to
# SanctuaryExt, so the accusation is left for the next coming. Skipped where the salon's 100 is done already.
for w in host vm; do
	if ! has_stage_of "$w" $SALON 100; then
		say "set-up: $w: a first visit to Diamond City by console (coc $CELL) for the salon's intro"
		dc "$w" focus >/dev/null; dc "$w" "console coc $CELL" >/dev/null
	fi
done
sleep 25
for w in host vm; do
	wait_ingame "$w" 60 || verdict VOID "$w did not come back into the game after its coc"
	say "set-up: $w after the first visit: salon done=$(done_of "$w" $SALON), accusation done=$(done_of "$w")"
	has_stage "$w" 10 && verdict VOID "$w's accusation started on the first visit (done=$(done_of "$w"))"
	dc "$w" focus >/dev/null; dc "$w" "console coc SanctuaryExt" >/dev/null
done
sleep 10
for w in host vm; do wait_ingame "$w" 60 || verdict VOID "$w did not come back into the game after its coc to SanctuaryExt"; done
sleep 5

# the first player arrives and walks up to Kyle
arrive "$FIRST" start
[ "${STARTED[$FIRST]}" = none ] && verdict VOID "the quest could not be started in the first game ($FIRST)"
say "$FIRST's player walks up to Kyle (real movement)"
rwalk "$FIRST" "$KYLE" 250 0.8; say "$FIRST: walk: $WALKED"
watch "$WATCH" kyle_gone "$FIRST"
{ has_stage "$FIRST" 15 || has_stage "$FIRST" 20; } || verdict VOID "the first game's ($FIRST) triggers never set the scene's 15 or 20 in $WATCH s of its player coming to Kyle: $(snap "$FIRST")"
say "+$((SECONDS - T0))s Kyle in $FIRST: $(actor_of "$FIRST" $KYLE); watching on 20 s"
watch 20

# the late friend arrives and walks up to Kyle; what its quest had done before its player came is kept
BEFORE=$(done_of "$LATE_WHO")
say "$LATE_WHO before its player comes: done=$BEFORE"
arrive "$LATE_WHO"
say "$LATE_WHO's player walks up to Kyle (real movement)"
rwalk "$LATE_WHO" "$KYLE" 250 0.8; say "$LATE_WHO: walk: $WALKED"
watch "$LATE"

rsince host run > "$REPRO_OUT/host.log"; rsince vm run > "$REPRO_OUT/vm.log"
say "the quest's lines in each log (kept in $REPRO_OUT):"
for w in host vm; do grep -E "$Q|$KYLE|$RILEY|PlacedScenes" "$REPRO_OUT/$w.log" | cut -c1-240 | head -30 | sed "s/^/  $w: /" | tee -a "$REPRO_OUT/run.log"; done
F=$(snap "$FIRST"); L=$(snap "$LATE_WHO")
say "end: $FIRST: $F"
say "end: $LATE_WHO: $L"
AFTER=$(done_of "$LATE_WHO")
# The late game's own triggers (or its own scene) set a stage of the scene after its player came: its own run.
OWN=""
for s in 15 20 30 40 50; do
	case ",$BEFORE," in *",$s,"*) continue;; esac
	case ",$AFTER," in *",$s,"*) OWN="$OWN $s";; esac
done
kf=$(kyle_gone "$FIRST" && echo gone || echo standing); kl=$(kyle_gone "$LATE_WHO" && echo gone || echo standing)
say "the late friend's game ($LATE_WHO): done before its player came: $BEFORE; after: $AFTER; set by its own play after it came:${OWN:- none}; polls with actors in a scene of the quest: $SCENES_LATE; Kyle $kf in $FIRST's game, $kl in $LATE_WHO's"
[ -n "$OWN" ] && verdict YES "the late friend's game ($LATE_WHO) played its own run: its stages$OWN were set there after its player came (its own triggers and scene), over its own Kyle and Riley: two accusations"
[ "$SCENES_LATE" -gt 0 ] && verdict YES "the late friend's game ($LATE_WHO) had actors in a scene of the quest of its own ($SCENES_LATE polls): two accusations"
[ "$kf" != "$kl" ] && verdict YES "Kyle ends $kf in $FIRST's game and $kl in $LATE_WHO's"
verdict NO "one accusation: $FIRST's game played it; $LATE_WHO's had the scene's stages before its player came (${BEFORE}) and set none of its own; Kyle $kf in both"
