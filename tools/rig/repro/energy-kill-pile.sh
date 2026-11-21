#!/bin/bash
# energy-kill-pile.sh [killer] [scene] [weapon]: an enemy killed with an energy weapon is a pile of ash (laser) or goo (plasma)
# in one game and an ordinary body in the other. Player report (on 0.20.4): "a kill with a laser or plasma gun leaves ashes
# for one of us and a normal body for the other, depending on who made the kill".
# The pile is the weapon's critical effect: a script on the dying enemy fades it out and puts a pile (an activator that holds
# the enemy's things) where it fell. Outside VATS criticals the game gives a killing hit that effect one time in four
# (fCritEffectOnDeathPercent, 25).
#   killer   host (the default) or vm: the player who shoots; the other one is the friend
#   scene    spawn (the default): each round one raider made beside the players with the dev channel's spawnenemies (as a
#            random encounter's, shared by the session). Round 1: the killer's game makes it (the killer shoots an enemy its
#            own game runs). Round 2: the friend's game makes it (the killer shoots a copy of an enemy the friend's game runs).
#            placed: one of Corvega's lobby raiders, the same reference in both games; RUNNER=host|vm (default: the killer)
#            is the game that enters the lobby first and so runs them. One round.
#   weapon   laser (the default) or plasma
# Each round: the raider is held where it stands and brought down to 5 health, the killer shoots it with real clicks until its
# game says it is dead, then both games are asked for a pile where it fell (dev `refs 27 .. Pile`: the activators whose base
# is one of the game's ash or goo piles) and for what the body holds (dev `contents`). Then the friend walks to it (a real
# walk), looks at it and takes everything (real R, R, Tab: the transfer menu and its "take all"), and both games are asked
# again what the body holds.
# Set-up, said so where it happens: both games to the scene by console coc; god mode; the gun and its ammo by console
# additem and equipitem; fCritEffectOnDeathPercent set to 100 in both games by console setgs (every killing hit gets the
# weapon's critical effect, instead of one in four; put back to 25 at the end); the raider held by console setrestrained and
# hurt by console damageav; in the placed scene the raider put in view by console setpos.
# REPRO: YES when a round ended with a pile in one game and none in the other (or with the two games disagreeing on what the
# body holds after the friend took it all); NO when every round's pile was there in both games and the body's things agree;
# VOID when no round made a pile in either game or the scene could not be made.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
. "$RIG/hitslib.sh"
KILLER=${1:-host}; SCENE=${2:-spawn}; WEAPON=${3:-laser}; FRIEND=$(other_game "$KILLER"); RUNNER=${RUNNER:-$KILLER}
RAIDER=${RAIDER:-001AE38D}; DIST=${DIST:-350}; VK_R=0x52; GMST=fCritEffectOnDeathPercent
case "$WEAPON" in
laser) GUN=0009983B; AMMO=000C1897 ;;
plasma) GUN=00100AE9; AMMO=0001DBB7 ;;
*) verdict VOID "usage: energy-kill-pile.sh [host|vm] [spawn|placed] [laser|plasma]" ;;
esac
[ "$KILLER" = host ] || [ "$KILLER" = vm ] || verdict VOID "usage: energy-kill-pile.sh [host|vm] [spawn|placed] [laser|plasma]"
[ "$SCENE" = spawn ] || [ "$SCENE" = placed ] || verdict VOID "usage: energy-kill-pile.sh [host|vm] [spawn|placed] [laser|plasma]"
need_session

declare -A T PILE HELD0 HELD1
shots_of() { dc "$1" "net status" | grep -oE ' shots=[0-9]+' | head -1 | cut -d= -f2; }
dead_in() { field "$(dc "$1" "refinfo $2")" dead; }
raider_ids() { dc "$1" "actors ${2:-2500}" | tr ';' '\n' | grep "'Raider'" | sed -E 's/^ *([0-9A-F]{8}).*/\1/'; }
# piles_of <who>: that game's ash and goo piles within 2500 u, one a line: "ref base editor-id x y"
piles_of() { dc "$1" "refs 27 2500 Pile" | tr ';' '\n' | awk '$1 ~ /^[0-9A-F]+$/ && NF >= 6 { print $1, substr($2, 6), $(NF - 4), $(NF - 2), $(NF - 1) }'; }
# new_pile <who> <piles before>: the first pile that game has now and had not before
new_pile() { piles_of "$1" | awk -v had="$2" 'BEGIN { n = split(had, h, "\n"); for (i = 1; i <= n; i++) { split(h[i], f, " "); seen[f[1]] = 1 } } !($1 in seen) { print; exit }'; }
_pile_now() { PILE[$1]=$(new_pile "$1" "$2"); [ -n "${PILE[$1]}" ]; }
# held <who> <ref>: what that game's body holds, its stacks sorted ("empty" for none; "?" when the game does not know the ref)
held() { local c; c=$(dc "$1" "contents $2"); case "$c" in *empty*) echo empty ;; *:*) grep -oE '[0-9A-F]{8}x-?[0-9]+' <<<"$c" | sort | paste -sd' ' - ;; *) echo "?" ;; esac; }
kit() {
	dc "$1" "console player.additem $GUN 1" >/dev/null; dc "$1" "console player.additem $AMMO 300" >/dev/null
	dc "$1" "console player.equipitem $GUN" >/dev/null; sleep 1; dc "$1" "draw on" >/dev/null
}
# spawn_one <game>: one raider made by that game in front of its player, held where it stands; T[game] is its reference there,
# T[other] the other game's copy of it (the raider that game did not list before)
spawn_one() {
	local s=$1 o before new="" i
	o=$(other_game "$s"); before=$(raider_ids "$o" | sort)
	T[$s]=$(dc "$s" "spawnenemies $RAIDER 1 $DIST" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | cut -d, -f1)
	[ -n "${T[$s]}" ] || return 1
	dc "$s" "console ${T[$s]}.setrestrained 1" >/dev/null
	for i in $(seq 1 20); do new=$(comm -13 <(printf '%s\n' "$before") <(raider_ids "$o" | sort) | head -1); [ -n "$new" ] && break; sleep 1; done
	T[$o]=$new; [ -n "$new" ]
}
# placed_one: one of the lobby's raiders alive in both games (the same reference in both), put in view of both players by the
# runner's console and held there
placed_one() {
	local a sp x y z
	for a in $(raider_ids "$RUNNER" 3000); do
		[ "$(dead_in host "$a")" = false ] && [ "$(dead_in vm "$a")" = false ] || continue
		sp=$(spot_for "$(dc "$KILLER" pos)" "$(dc "$FRIEND" pos)" "" 1 "$LOBBY_BLOCKED $LOBBY_PROPS"); [ -n "$sp" ] || return 1
		read -r x y z _ <<<"$sp"
		dc "$RUNNER" "console $a.setrestrained 1" >/dev/null
		dc "$RUNNER" "console $a.setpos x $x" >/dev/null; dc "$RUNNER" "console $a.setpos y $y" >/dev/null; dc "$RUNNER" "console $a.setpos z $z" >/dev/null
		sleep 2
		[ "$(dead_in host "$a")" = false ] && [ "$(dead_in vm "$a")" = false ] || continue
		T[host]=$a; T[vm]=$a; return 0
	done
	return 1
}
# shoot: the killer fires real clicks at its raider until its own game says it is dead (8 at most). CLICKS, FIRED (its shots
# counter's rise)
shoot() {
	local r=${T[$KILLER]} s0 s1 i
	s0=$(shots_of "$KILLER"); CLICKS=0
	for i in 1 2 3 4 5 6 7 8; do
		dc "$KILLER" "turnto $r 300 aim" >/dev/null; sleep 0.5; dc "$KILLER" focus >/dev/null; dc "$KILLER" "click 80" >/dev/null; CLICKS=$i; sleep 1.5
		[ "$(dead_in "$KILLER" "$r")" = true ] && break
	done
	s1=$(shots_of "$KILLER"); FIRED="?"; is_int "$s0" && is_int "$s1" && FIRED=$((s1 - s0))
	wait_for 6 eval '[ "$(dead_in "$KILLER" "$r")" = true ]'
}
# take_all <who> <what to look at>: that player walks to it, looks at it and takes everything: R opens the transfer menu from
# the quick list (E when R did not), R again takes all, Tab closes. TOOK says how it went
take_all() {
	local w=$1 at=$2
	rwalk "$w" "$at" 90 0.7; rpress_look "$w" "$at"
	rkey "$w" $VK_R 100
	if ! wait_for 4 menu_open "$w" ContainerMenu; then rkey "$w" "$VK_E" 100; wait_for 4 menu_open "$w" ContainerMenu || { TOOK="its transfer menu did not open (it looked at '$ROLLOVER', $WALKED)"; return 1; }; fi
	sleep 0.8; rkey "$w" $VK_R 100; sleep 1.2; rkey "$w" "$VK_TAB" 100
	wait_for 5 eval '! menu_open "$w" ContainerMenu' || rkey "$w" "$VK_TAB" 100
	TOOK="looked at '$ROLLOVER', took all in the transfer menu"
}
rpress_look() { dc "$1" "turnto $2 300 aim" >/dev/null; sleep 0.8; ROLLOVER=$(rrollover "$1"); }

# round <name> <the game that runs the raider>
ROUNDS=0; PILED=0; DIFFER=0; LOOTDIFF=0; NOTES=""
round() {
	local name=$1 run=$2 g hp b0h b0v both
	ROUNDS=$((ROUNDS + 1)); T=(); PILE=()
	if [ "$SCENE" = spawn ]; then
		spawn_one "$run" || { say "$name: the raider was not made in both games (host '${T[host]}', vm '${T[vm]}')"; return 1; }
	else
		placed_one || { say "$name: no lobby raider alive in both games could be put in view"; return 1; }
	fi
	hp=$(field "$(dc "$run" "refinfo ${T[$run]}")" hp); hp=${hp%.*}
	is_int "$hp" && [ "$hp" -gt 5 ] && dc "$run" "console ${T[$run]}.damageav health $((hp - 5))" >/dev/null
	sleep 2
	b0h=$(piles_of host); b0v=$(piles_of vm)
	say "$name: $run's game runs the raider (host ${T[host]}, vm ${T[vm]}; health $hp brought to 5 by console); $KILLER shoots it with the $WEAPON gun"
	if ! shoot; then say "$name: $KILLER's game never said the raider was dead ($CLICKS clicks, its shots counter rose by $FIRED; $(dc "$KILLER" "refinfo ${T[$KILLER]}" | grep -oE 'hp=[-0-9.]+ dead=[a-z]+'))"; return 1; fi
	wait_for 10 eval '[ "$(dead_in "$FRIEND" "${T[$FRIEND]}")" = true ]'
	# the script puts the pile down about a second after the death and ends some seconds later: both games get 12 s
	wait_for 12 _pile_now host "$b0h"; wait_for 12 _pile_now vm "$b0v"
	[ -n "${PILE[host]}" ] && [ -z "${PILE[vm]}" ] && { sleep 8; _pile_now vm "$b0v"; }
	[ -n "${PILE[vm]}" ] && [ -z "${PILE[host]}" ] && { sleep 8; _pile_now host "$b0h"; }
	for g in host vm; do HELD0[$g]=$(held "$g" "${T[$g]}"); done
	say "$name: dead: host=$(dead_in host "${T[host]}") vm=$(dead_in vm "${T[vm]}") after $CLICKS clicks (shots +$FIRED)"
	for g in host vm; do say "$name: $g's game: $([ -n "${PILE[$g]}" ] && echo "a PILE (${PILE[$g]})" || echo "NO pile: an ordinary body"); the body holds: ${HELD0[$g]}; $(dc "$g" "refinfo ${T[$g]}" | grep -oE '(crit|pile)=[^ ]*' | paste -sd' ' -)"; done
	both=0; [ -n "${PILE[host]}" ] && [ -n "${PILE[vm]}" ] && both=1
	if [ -n "${PILE[host]}${PILE[vm]}" ]; then
		PILED=$((PILED + 1))
		[ $both = 1 ] || { DIFFER=$((DIFFER + 1)); NOTES="$NOTES $name: a pile in $([ -n "${PILE[host]}" ] && echo host || echo vm)'s game only;"; }
	else
		NOTES="$NOTES $name: no pile in either game;"
	fi
	# the friend takes everything: through its pile when it has one, from the body when not
	local at=${PILE[$FRIEND]%% *}; [ -n "$at" ] || at=${T[$FRIEND]}
	take_all "$FRIEND" "$at"; sleep 3
	for g in host vm; do HELD1[$g]=$(held "$g" "${T[$g]}"); done
	say "$name: $FRIEND $TOOK ($([ -n "${PILE[$FRIEND]}" ] && echo "its pile" || echo "the body")); after it the body holds: host: ${HELD1[host]} | vm: ${HELD1[vm]}"
	if [ "${HELD1[host]}" != "${HELD1[vm]}" ]; then LOOTDIFF=$((LOOTDIFF + 1)); NOTES="$NOTES $name: after $FRIEND took all, the body holds different things in the two games;"; fi
}

# ---- set-up ------------------------------------------------------------------------------------------------------------------------
if [ "$SCENE" = spawn ]; then
	CELL=${CELL:-Vault111Ext}
	say "set-up: both games to $CELL by console coc; god mode; a $WEAPON gun and ammo each by console"
	enter_both "$CELL" 16
else
	say "set-up: both games to Corvega's lobby by console coc ($RUNNER first: its game runs the raiders); god mode; a $WEAPON gun and ammo each by console"
	dc "$RUNNER" "console coc CorvegaAssemblyPlant01" >/dev/null; sleep 25; wait_ingame "$RUNNER" 30
	dc "$(other_game "$RUNNER")" "console coc CorvegaAssemblyPlant01" >/dev/null; sleep 25; wait_ingame "$(other_game "$RUNNER")" 30
fi
for g in host vm; do dc "$g" 'god on' >/dev/null; kit "$g"; done
for g in host vm; do dc "$g" "console setgs $GMST 100" >/dev/null; done
say "set-up: $GMST set to 100 in both games by console (every killing hit gets the weapon's critical effect; the game's own is 25): host '$(dc host "gs $GMST" | cut -c1-80)', vm '$(dc vm "gs $GMST" | cut -c1-80)'"
say "$KILLER holds: $(dc "$KILLER" 'equipped 00000014' | cut -c1-120)"
dc "$KILLER" 'equipped 00000014' | grep -qi "$GUN" || verdict VOID "$KILLER does not hold the $WEAPON gun ($GUN)"
CH0=$(crash_list_host); CV0=$(crash_list_vm)
rmark host; rmark vm

if [ "$SCENE" = spawn ]; then
	round "round 1 ($KILLER kills its own game's raider)" "$KILLER"
	round "round 2 ($KILLER kills a copy of $FRIEND's raider)" "$FRIEND"
else
	round "round 1 ($KILLER kills a placed raider that $RUNNER's game runs)" "$RUNNER"
fi

for g in host vm; do dc "$g" "console setgs $GMST 25" >/dev/null; done
say "$GMST put back to 25 in both games: host '$(dc host "gs $GMST" | cut -c1-80)'"
rsince host > "$REPRO_OUT/run-host.log"; rsince vm > "$REPRO_OUT/run-vm.log"
NH=$(crash_new "$CH0" "$(crash_list_host)"); NV=$(crash_new "$CV0" "$(crash_list_vm)")
[ -n "$NH$NV" ] && say "NEW CRASH LOGS: host '$NH' vm '$NV'"
say "rounds $ROUNDS, with a pile $PILED, pile in one game only $DIFFER, body's things differing after the loot $LOOTDIFF.$NOTES"
[ "$PILED" -gt 0 ] || verdict VOID "no round made a pile in either game ($ROUNDS rounds).$NOTES"
[ "$DIFFER" -gt 0 ] && verdict YES "an energy-weapon kill left a pile in one game and an ordinary body in the other in $DIFFER of $PILED rounds.$NOTES"
[ "$LOOTDIFF" -gt 0 ] && verdict YES "the pile was there in both games, but the body's things differ after the friend took them.$NOTES"
verdict NO "every pile ($PILED of $ROUNDS rounds) was there in both games, and the body holds the same in both after the friend took it all.$NOTES"
