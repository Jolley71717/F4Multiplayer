#!/bin/bash
# quest-created-raiders.sh: a friend's shots on the raiders a story quest creates as it runs (a Nexus report on 0.21.0: in Concord,
# helping Preston, "my friend's shots just don't register"). When Freedom Calls (Min00, 001A001C) holds the raiders that come at
# the players in Concord's street in its aliases WaveRaider01 to 03, RaiderLookout and RaiderBoss: external aliases of DN092's (The
# Museum of Freedom), whose "create reference" aliases make them:
# every game's run of the quest makes its own, under that game's own runtime ids, at the same markers, and the quest's script
# enables them in each game as its player leaves the museum's roof. Each game shared its own as a spawn, so both games had two
# raiders for each alias (its own and a copy of the friend's), and a friend who shot the raider in front of them hurt only their
# own game's: the other game's raider of that alias never lost a point. Paired across games (src/game/QuestActors.h), the alias's
# raider is one NPC in the session, and any player's hit on it changes its health in both games.
#   1. Both games in Concord's street (console coc ConcordExt when not there, said), god mode (rig.sh join), a 10mm and its rounds
#      each (console additem). Started from fresh games joined in the museum (rig.sh join ConcordMuseum01 from the main menu:
#      the museum starts the quest in each game, as walking there does; a game put straight into the street has it not started
#      and nothing in its aliases): each alias's raider must be there and still off in both games, else VOID.
#   2. For each direction (the VM shoots, then the host shoots), the next alias of ALIASES (WaveRaider01 and 03 by default) is
#      switched on in both games (console <ref>.enable in each: it stands for the quest's own script, which does it in each game
#      as its player comes down from the roof). Both games' raiders of the alias must stand loaded and alive within 30 s.
#   3. The shooter walks (the pad) to within 350 u of its own game's raider of the alias and the other player 400 u away from it,
#      out of the line of fire; the shooter turns to its own game's
#      raider of the alias (dev turnto, a look) and fires real shots (clicks), up to 10,
#      until one lands (the raider's health drops in the shooter's game). The other game's raider of the alias is read 5 s on.
# REPRO: YES when a landed shot leaves the other game's raider of the alias at the health it had (its own raider, not the one
# shot); NO when every direction's landed shot changes it there too (lower, or dead). VOID when the scene could not be made (a
# raider not switched on or not loaded, no shot landing in 10, a click that fired nothing).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
QUEST=${QUEST:-001A001C}         # When Freedom Calls (Min00)
ALIASES=${ALIASES:-"20 22"}      # WaveRaider01, WaveRaider03 (ground level, near the street)
DIRECTIONS=${DIRECTIONS:-"vm host"}
CONCORD="-59350 72578"           # where coc ConcordExt puts a player
run_cap 1500
need_session

# aref <who> <alias>: that game's reference in the quest's alias (empty when none)
aref() { dc "$1" "aliases $QUEST" | tr '|' '\n' | grep -E "^ Ref $2 " | grep -oE 'here=[0-9A-F]{8}' | cut -d= -f2 | grep -v '^00000000$'; }
info() { dc "$1" "refinfo $2"; }
hp() { field "$(info "$1" "$2")" hp; }
dead() { [ "$(field "$(info "$1" "$2")" dead)" = true ]; }
shots_of() { dc "$1" "net status" | grep -oE ' shots=[0-9]+' | head -1 | cut -d= -f2; }
lower() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a != "" && b != "" && a + 0 < b - 0.5) }'; }

# 1. both in Concord's street, armed
for w in host vm; do
	d=$(dist_xy "$(pos_of $w)" "$CONCORD")
	if [ "$(field "$(dc $w status)" interior)" != false ] || [ -z "$d" ] || [ "$d" -gt 3000 ]; then
		say "set-up: $w is not in Concord's street ($(pos_of $w)): console coc ConcordExt"
		dc $w focus >/dev/null; dc $w "console coc ConcordExt" >/dev/null; sleep 12; wait_ingame $w 60
	fi
	dc $w "console player.additem 00004822 1" >/dev/null; dc $w "console player.additem 0001F276 200" >/dev/null
	dc $w "console player.equipitem 00004822" >/dev/null; dc $w "draw on" >/dev/null
done
say "set-up: both players have a 10mm and 200 rounds (console additem), drawn; host at $(pos_of host), vm at $(pos_of vm)"
for w in host vm; do say "set-up: $w's quest: $(dc $w "stages $QUEST")"; done
for a in $ALIASES; do
	for w in host vm; do
		r=$(aref $w "$a"); [ -n "$r" ] || verdict VOID "$w's game has nothing in alias $a of $QUEST (the quest is not running there: join in ConcordMuseum01 first)"
		[ "$(field "$(info $w "$r")" disabled)" = true ] || verdict VOID "$w's raider $r of alias $a is on already: start from fresh games (rig.sh join ConcordMuseum01 from the main menu)"
	done
done

BAD=""; DONE=0
set -- $ALIASES
for s in $DIRECTIONS; do
	o=$(other_game $s); a=$1; shift
	[ -n "$a" ] || verdict VOID "no alias left for the $s's turn (ALIASES='$ALIASES')"
	RS=$(aref $s "$a"); RO=$(aref $o "$a")
	rmark $s "$s"; rmark $o "$s"
	# 2. switched on in both games, as the quest's script does in each
	say "[$s shoots] alias $a: $s's raider $RS, $o's $RO; switched on in both games (console enable, standing for the quest's script)"
	dc $s "console $RS.enable" >/dev/null; dc $o "console $RO.enable" >/dev/null
	_up() { local i; for i in "$s $RS" "$o $RO"; do i=$(info $i); [ "$(field "$i" 3d)" = true ] && [ "$(field "$i" dead)" = false ] && [ "$(field "$i" disabled)" = false ] || return 1; done; }
	scene_live "both games' raiders of alias $a stand loaded and alive" 30 _up
	sleep 8   # the spawns shared, and copies placed or the raiders paired
	for w in host vm; do say "[$s shoots] $w: $(dc $w "mirror $([ $w = $s ] && echo $RS || echo $RO)" | cut -c1-90); raiders near: $(dc $w 'actors 4000' | tr ';' '\n' | grep -c "'Raider' dead=false")"; done
	H0S=$(hp $s $RS); H0O=$(hp $o $RO)
	say "[$s shoots] before: $s's raider at $H0S health, $o's at $H0O"
	# 3. real fire at the shooter's own game's raider of the alias, until a shot lands. First the shooter walks (the pad) to
	# within 350 u of it, and the other player 400 u further from it: the two stand side by side after the set-up, and the
	# other's stand-in in the shooter's game could be in the line of fire (the host's 10 shots at WaveRaider03 all missed with
	# the VM 50 u ahead of it), or a player left where the last turn's walk put them could have a wreck between them and the
	# raider (the host's next 10 hit a static, 0001D563, 2026-11-22)
	rwalk $s "$RS" 350 || true
	step_aside $o "$RO" 400 || true
	say "[$s shoots] $s walked to $(dist_xy "$(pos_of $s)" "$(field "$(info $s $RS)" x) $(field "$(info $s $RS)" y)") u of its raider, $o aside: $(dist_xy "$(pos_of $s)" "$(pos_of $o)") u from $s"
	landed=""; n0=$(shots_of $s)
	for i in $(seq 1 10); do
		dc $s "turnto $RS 300 aim" >/dev/null; sleep 0.5; dc $s "click 80" >/dev/null; sleep 1.2
		h=$(hp $s $RS); if dead $s $RS || lower "$h" "$H0S"; then landed=$i; break; fi
	done
	n1=$(shots_of $s)
	[ -n "$n0" ] && [ -n "$n1" ] && [ "$n1" -gt "$n0" ] || verdict VOID "$s's clicks fired nothing (shots=$n0 before, $n1 after)"
	[ -n "$landed" ] || verdict VOID "none of $s's 10 shots at $RS landed (its health stayed $H0S in $s's game; shots=$n0 -> $n1)"
	sleep 5
	H1S=$(hp $s $RS); H1O=$(hp $o $RO); DS=$(field "$(info $s $RS)" dead); DO=$(field "$(info $o $RO)" dead)
	say "[$s shoots] shot $landed landed: $s's raider $H0S -> $H1S (dead=$DS); $o's raider of the alias $H0O -> $H1O (dead=$DO)"
	rsince $s "$s" | grep -E "Spawns: .*(FF[0-9A-F]{6}|alias)|CopyWatch: hit|damage on" | tail -8 | sed "s/^/  $s| /" | tee -a "$REPRO_OUT/run.log"
	rsince $o "$s" | grep -E "Spawns: .*(FF[0-9A-F]{6}|alias)|CopyWatch|damage on" | tail -8 | sed "s/^/  $o| /" | tee -a "$REPRO_OUT/run.log"
	if [ "$DO" = true ] || lower "$H1O" "$H0O"; then say "[$s shoots] the hit reached $o's game"; DONE=$((DONE + 1))
	else BAD="$BAD $s's landed shot on its raider of alias $a ($H0S -> $H1S) left $o's raider of that alias at $H1O (from $H0O);"; fi
done
rkeep host quest-created-raiders; rkeep vm quest-created-raiders
[ -n "$BAD" ] && verdict YES "a hit on a quest's created raider stayed in the shooter's game:$BAD"
verdict NO "each landed shot ($DONE direction(s): $DIRECTIONS) changed the alias's raider's health in the other game too"
