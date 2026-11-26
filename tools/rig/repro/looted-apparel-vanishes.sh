#!/bin/bash
# looted-apparel-vanishes.sh [looter] [kinds]: weapons and armor a player loots from a dead enemy that then disappear. Player
# report (on 0.20.4): "Looted BOS apparel and armour appeared to disappear." <looter> is host (the default), vm or both (one
# after the other); <kinds> the enemies, one corpse each: bos (a Brotherhood soldier in combat armor, the default's first),
# raider (the default's second, to compare), scribe, bospa (a Brotherhood soldier in power armor) or an NPC base's form id.
# What the players do:
#   1. An enemy of each kind is made in front of both players by OWNER's game (the host's by default), so the other game has
#      its copy of it, and OWNER shoots it dead (real clicks). Each game then has a corpse of it with that game's own loot.
#   2. The looter walks to its corpse (a real walk), opens it with a real R (Transfer: the container menu) and takes what it
#      holds with real E presses, one stack a press, then closes the menu with a real Tab.
#   3. After each of STEPS the weapons and armor taken are counted in the looter's inventory (what it had before + what it
#      took), and each game's corpse and what it still holds are read:
#        wait   10 s after the take
#        equip  the looter wears the first armor piece it took (console equipitem)
#        osave  the other player saves (console save, a throwaway save)
#        lsave  the looter saves
#        cell   both leave for AWAY and come back (console coc)
#        lload  the looter loads the save it just made
#        old    (not in the default) the looter loads a save from before the fight, as a death's reload does: what was
#               looted since is expected to be gone, so this step is told and never counted
# Set-up, said so where it happens: both games to CELL by console coc; god mode; OWNER given a 10mm and ammo by console; the
# enemies made with the dev channel's spawnenemies, held where they stand (console setrestrained), made to fight when they are
# not hostile on their own (console startcombat: the session shares what fights) and brought to 5 health (console damageav).
# REPRO: YES when a weapon or armor piece the looter took is no longer in its inventory (no older save loaded), or when a
# corpse that still held a weapon or armor for its player is gone or emptied without that player taking it; NO when every
# piece is still there after every step; VOID when the scene could not be made (no corpse in both games, nothing to loot).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
LOOTERS=${1:-host}; KINDS=${2:-bos raider}; OWNER=${OWNER:-host}; STEPS=${STEPS:-wait equip osave lsave cell lload}
CELL=${CELL:-SanctuaryExt}; AWAY=${AWAY:-RedRocketExt}; SAVE=${SAVE:-f4mp_repro_loot}   # not Vault111Ext: nobody can host from inside Vault 111
PISTOL=00004822; AMMO=0001F276; PLAYER=00000014; VK_R=0x52
[ "$LOOTERS" = both ] && LOOTERS="host vm"
for w in $LOOTERS; do [ "$w" = host ] || [ "$w" = vm ] || verdict VOID "usage: looted-apparel-vanishes.sh [host|vm|both] [kinds]"; done
[ "$OWNER" = host ] || [ "$OWNER" = vm ] || verdict VOID "OWNER is host or vm"
need_session

base_of() { case "$1" in bos) echo 0023D8D2;; raider) echo 001AE38D;; scribe) echo 00240108;; bospa) echo 00240109;; *) echo "$1";; esac; }
dead_in() { field "$(dc "$1" "combat $2")" dead; }
joined() { dc vm 'net status' | grep -q 'status=joined'; }
# ask <who> <command>: a dev-channel answer, asked up to three times when none comes (a game busy saving or loading); it reads
# nothing from the caller's input (it is called inside read loops)
ask() { local r i; for i in 1 2 3; do r=$(dc "$1" "$2" </dev/null); [ -n "$r" ] && break; sleep 1.5; done; echo "$r"; }
# ---- contents (as tools/rig/steps.sh reads them) ------------------------------------------------------------------------------
stacks_of() { grep -oE "[0-9A-F]{8}x[0-9]+" <<<"$1" | tr x ' '; }   # contents -> "<item> <count>" lines
count_in() { stacks_of "$1" | awk -v i="$2" '$1==i {n+=$2} END{print n+0}'; }
total_in() { stacks_of "$1" | awk '{n+=$2} END{print n+0}'; }
_CONTENTS_RE='^[0-9A-F]{8}:( [0-9A-F]{8}x-?[0-9]+)* (\(([0-9]+) stacks\)|empty)$'
contents_ok() { [[ $1 =~ $_CONTENTS_RE ]]; }
# is_gear <item>: a weapon or armor (dev `edid`: ARMO 29, WEAP 43), the kind each player keeps for itself (LootScope)
declare -A ITYPE
is_gear() { [ -n "${ITYPE[$1]}" ] || ITYPE[$1]=$(field "$(ask host "edid $1")" type); [ "${ITYPE[$1]}" = 29 ] || [ "${ITYPE[$1]}" = 43 ]; }
gear_lines() { local i n; while read -r i n; do [ -n "$i" ] || continue; is_gear "$i" && echo "$i $n"; done <<<"$(stacks_of "$1")"; }
gear_said() { local i n o=""; while read -r i n; do [ -n "$i" ] && o="$o ${i}x$n"; done <<<"$(gear_lines "$1")"; echo "${o:- none}"; }
# ---- menus (as tools/rig/steps.sh closes them) --------------------------------------------------------------------------------
clear_menus() {
	local i m
	for i in 1 2 3 4; do
		m=$(dc "$1" menus | grep -oE 'open=[^ ]*')
		if grep -q 'MessageBoxMenu:' <<<"$m"; then rkey "$1" $VK_ENTER 60
		elif grep -qE '(PipboyMenu|TerminalMenu|ContainerMenu):' <<<"$m"; then rkey "$1" $VK_TAB 60
		else return 0; fi
		sleep 1.2
	done
	return 1
}
# ref_state <who> <corpse>: there (a dead actor, neither deleted nor disabled), deleted, disabled, gone (no such reference, or
# the id names something else now: a game-made id is used again), or unread (no answer)
ref_state() {
	local r; r=$(ask "$1" "refinfo $2")
	case "$r" in "") echo unread; return;; error*) echo gone; return;; esac
	[ "$(field "$r" deleted)" = true ] && { echo deleted; return; }
	[ "$(field "$r" disabled)" = true ] && { echo disabled; return; }
	[ "$(field "$r" dead)" = true ] && echo there || echo gone
}
kit() {
	dc "$1" "console player.additem $PISTOL 1" >/dev/null; dc "$1" "console player.additem $AMMO 300" >/dev/null
	dc "$1" "console player.equipitem $PISTOL" >/dev/null; sleep 2; dc "$1" "draw on" >/dev/null
}
# _copy_at <who> <x> <y>: that game's actor standing there that another game runs (its copy of the enemy); COPY
_copy_at() { COPY=$(dc "$1" "enemies like any $2 $3 400" | grep 'made=true' | grep -oE '^[0-9A-F]{8}'); [ -n "$COPY" ]; }

declare -A C HELD HELDGEAR LOST WANT TOOK
# hold <who> <kind>: what that game's corpse holds now is what it is expected to hold from here on
hold() {
	local c; c=$(ask "$1" "contents ${C[$1:$2]}")
	contents_ok "$c" || { say "  $1's corpse of the $2 (${C[$1:$2]}): contents not read ('$c')"; return 1; }
	HELD[$1:$2]=$(total_in "$c"); HELDGEAR[$1:$2]=$(gear_said "$c")
}
# make_corpse <kind>: 1. of the header for one enemy; C[<game>:<kind>] names each game's corpse of it. 1 = the scene was not made
make_corpse() {
	local k=$1 base other r info x y hp i; base=$(base_of "$1"); other=$(other_game "$OWNER")
	r=$(dc "$OWNER" "spawnenemies $base 1 350" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | cut -d, -f1)
	[ -n "$r" ] || { say "set-up: $OWNER's spawnenemies $base made nothing"; return 1; }
	dc "$OWNER" "console $r.setrestrained 1" >/dev/null; sleep 2
	info=$(dc "$OWNER" "refinfo $r"); x=$(field "$info" x); y=$(field "$info" y); x=${x%.*}; y=${y%.*}
	say "set-up: $OWNER's game made the $k $r (base $base) by spawnenemies, held where it stands by console setrestrained"
	if ! wait_for 10 _copy_at "$other" "$x" "$y"; then
		say "set-up: the $k is not hostile on its own: made to fight $OWNER's player by console startcombat"
		dc "$OWNER" "console $r.startcombat 14" >/dev/null
		wait_for 20 _copy_at "$other" "$x" "$y" || { say "set-up: $other's game got no copy of the $k $r ($(dc "$OWNER" "combat $r"))"; return 1; }
	fi
	C[$OWNER:$k]=$r; C[$other:$k]=$COPY
	hp=$(field "$(dc "$OWNER" "refinfo $r")" hp); hp=${hp%.*}
	is_int "$hp" && [ "$hp" -gt 5 ] && dc "$OWNER" "console $r.damageav health $((hp - 5))" >/dev/null
	say "set-up: $other's copy of it is $COPY; brought to 5 health by console damageav (it had $hp)"
	for i in 1 2 3 4 5 6 7 8; do
		[ "$(dead_in "$OWNER" "$r")" = true ] && break
		dc "$OWNER" focus >/dev/null; dc "$OWNER" "turnto $r 200 aim" >/dev/null; sleep 0.4; dc "$OWNER" "click 120" >/dev/null; sleep 1.2
	done
	[ "$(dead_in "$OWNER" "$r")" = true ] || { say "set-up: $OWNER's shots (8 real clicks) did not kill the $k $r ($(dc "$OWNER" "combat $r"))"; return 1; }
	_dead_there() { [ "$(dead_in "$other" "$COPY")" = true ]; }
	wait_for 20 _dead_there || { say "set-up: $other's copy $COPY of the $k did not die ($(dc "$other" "combat $COPY"))"; return 1; }
	sleep 3
	for i in host vm; do
		hold "$i" "$k" || return 1
		say "the $k dead in $i's game: ${C[$i:$k]} holds ${HELD[$i:$k]} items, weapons and armor:${HELDGEAR[$i:$k]}; worn: $(dc "$i" "equipped ${C[$i:$k]}" | grep -oE "[0-9A-F]{8} '[^']*'" | paste -sd' ' -)"
	done
}
# loot <who> <kind>: 2. of the header. 1 = nothing could be taken
loot() {
	local who=$1 k=$2 c=${C[$1:$2]} b a n i cnt presses=0 maxp roll opened=0
	b=$(ask "$who" "contents $c"); contents_ok "$b" || { say "loot: $who's corpse of the $k ($c): contents not read ('$b')"; return 1; }
	[ -n "$(gear_lines "$b")" ] || { say "loot: $who's corpse of the $k ($c) holds no weapon or armor ($b)"; return 1; }
	while read -r i cnt; do [ -n "${WANT[$i]}" ] || WANT[$i]=$(ask "$who" "count $PLAYER $i"); done <<<"$(gear_lines "$b")"
	rwalk "$who" "$c" 110; say "loot: $who walked to its corpse of the $k ($WALKED)"
	clear_menus "$who"
	for i in 1 2; do
		dc "$who" "turnto $c 300 aim" >/dev/null; sleep 0.8; roll=$(rrollover "$who"); rkey "$who" $VK_R 100
		wait_for 4 menu_open "$who" ContainerMenu && { opened=1; break; }
	done
	if [ $opened = 1 ]; then say "loot: a real R on '$roll' opened the container menu"
	else
		say "loot: a real R on '$roll' opened no container menu: opened by console activate instead"
		dc "$who" focus >/dev/null; dc "$who" "console $c.activate player" >/dev/null
		wait_for 5 menu_open "$who" ContainerMenu || { say "loot: no container menu for $who's corpse $c (menus: $(menus "$who"))"; return 1; }
	fi
	sleep 1.5   # the menu's movie takes no key while it loads
	a=$b; maxp=$(( $(stacks_of "$b" | wc -l) + 4 ))
	while [ $presses -lt $maxp ] && [ -n "$(gear_lines "$a")" ] && menu_open "$who" ContainerMenu; do
		rkey "$who" $VK_E 80; presses=$((presses + 1)); sleep 0.9; n=$(ask "$who" "contents $c")
		if [ "$n" = "$a" ]; then rkey "$who" $VK_ENTER 80; sleep 0.9; n=$(ask "$who" "contents $c"); fi   # a stack's "how many" box: all of it
		say "  E $presses: the corpse holds ${n#*: }"; a=$n
	done
	if [ -n "$(gear_lines "$a")" ] && menu_open "$who" ContainerMenu; then rkey "$who" $VK_R 100; sleep 1.5; say "  a real R (take all) for what the E presses left"; fi
	menu_open "$who" ContainerMenu && { rkey "$who" $VK_TAB 80; sleep 1; }
	clear_menus "$who"
	a=$(ask "$who" "contents $c"); contents_ok "$a" || { say "loot: $who's corpse $c: contents not read after the take ('$a')"; return 1; }
	while read -r i cnt; do
		n=$((cnt - $(count_in "$a" "$i"))); TOOK[$i]=$(( ${TOOK[$i]:-0} + n )); is_int "${WANT[$i]}" && WANT[$i]=$(( ${WANT[$i]} + n ))
	done <<<"$(gear_lines "$b")"
	hold "$who" "$k"
	say "loot: $who took from the $k:$(gear_said "$b") -> left on it:$(gear_said "$a") ($presses E presses)"
}
# tally <who> <label> [why not counted]: every weapon and armor piece taken, in the looter's inventory now against what it should hold
tally() {
	local who=$1 i have line="" bad=""
	for i in "${!TOOK[@]}"; do
		[ "${TOOK[$i]}" -gt 0 ] || continue
		have=$(ask "$who" "count $PLAYER $i")
		if ! is_int "$have" || ! is_int "${WANT[$i]}"; then line="$line $i=unread"; continue; fi
		line="$line $i=$have/${WANT[$i]}"; [ "$have" -lt "${WANT[$i]}" ] && bad="$bad $i ($have of ${WANT[$i]}; $(dc "$who" "itemmods $i" | cut -c1-120))"
	done
	say "$2: $who's inventory, have/should have:${line:- nothing taken}"
	[ -z "$bad" ] && return 0
	if [ -n "$3" ]; then say "  missing, not counted ($3):$bad"; else LOSS_INV="$LOSS_INV [$who, $2:$bad]"; say "  MISSING from $who's inventory:$bad"; fi
}
# corpses <label> [why not counted]: each game's corpse of each enemy, and what it holds against what it held at its last reading
corpses() {
	local g k c st cont n
	for k in $MADE; do for g in host vm; do
		c=${C[$g:$k]}; [ -n "${LOST[$g:$k]}" ] && continue
		st=$(ref_state "$g" "$c"); [ "$st" = unread ] && { say "  $1: $g's corpse of the $k ($c) not read"; continue; }
		n=""; if [ "$st" = there ]; then cont=$(ask "$g" "contents $c"); contents_ok "$cont" && n=$(total_in "$cont"); fi
		if [ "$st" = there ] && { [ -z "$n" ] || [ "$n" -ge "${HELD[$g:$k]}" ]; }; then
			say "  $1: $g's corpse of the $k ($c) is there, holding ${n:-?} items, weapons and armor:$(gear_said "$cont")"; continue
		fi
		LOST[$g:$k]=$1; [ "$st" = there ] && st="there but holds $n items"
		say "  $1: $g's corpse of the $k ($c) is $st; it held ${HELD[$g:$k]} items, weapons and armor:${HELDGEAR[$g:$k]}"
		[ "${HELDGEAR[$g:$k]}" = " none" ] && continue
		if [ -n "$2" ]; then say "  not counted ($2)"; else LOSS_CORPSE="$LOSS_CORPSE [$1: $g's corpse of the $k $st, with${HELDGEAR[$g:$k]} on it]"; fi
	done; done
}
# save_by <who> <name>, load_by <who> <name>: a console save and load in the session (the save is a throwaway: the rig turn moves it aside)
save_by() { dc "$1" "console save $2" >/dev/null; sleep 8; wait_ingame "$1" 30; }
load_by() { dc "$1" focus >/dev/null; dc "$1" "console load $2" >/dev/null; sleep 20; wait_ingame "$1" 60; }

# ---- the run, once for each looter ----------------------------------------------------------------------------------------------
LOSS_INV=""; LOSS_CORPSE=""; RAN=""
for LOOTER in $LOOTERS; do
	O=$(other_game "$LOOTER"); MADE=""; LOOTED=""; SAVED=""
	for k in "${!C[@]}"; do unset "C[$k]"; done; for k in "${!LOST[@]}"; do unset "LOST[$k]"; done
	for k in "${!WANT[@]}"; do unset "WANT[$k]"; done; for k in "${!TOOK[@]}"; do unset "TOOK[$k]"; done
	say "==== looter: $LOOTER (the enemies are $OWNER's game's; the other player is $O) ===="
	say "set-up: both games to $CELL by console coc; god mode; $OWNER given a 10mm and ammo by console"
	enter_both "$CELL" 16
	joined || verdict VOID "the friend is not in the session after the cell's load ($(dc vm 'net status' | grep -oE 'status=[a-z]+'))"
	for w in host vm; do dc "$w" 'god on' >/dev/null; clear_menus "$w"; done
	kit "$OWNER"; rmark host "$LOOTER"; rmark vm "$LOOTER"
	for w in host vm; do say "$w before: $(dc "$w" 'net status' | grep -oE 'spawns: ours=[^|]*' | cut -c1-200)"; done
	case " $STEPS " in *" old "*) say "set-up: $LOOTER saves before the fight (console save ${SAVE}_${LOOTER}_old)"; save_by "$LOOTER" "${SAVE}_${LOOTER}_old";; esac
	n=0
	for k in $KINDS; do
		[ $n -gt 0 ] && { dc "$OWNER" focus >/dev/null; dc "$OWNER" "mouse 800 0" >/dev/null; sleep 1; }   # the next one stands apart: a real turn
		n=$((n + 1)); if make_corpse "$k"; then MADE="$MADE $k"; else say "the $k is left out"; fi
	done
	[ -n "$MADE" ] || verdict VOID "no enemy lay dead in both games"
	for k in $MADE; do loot "$LOOTER" "$k" && LOOTED="$LOOTED $k"; done
	[ -n "$LOOTED" ] || verdict VOID "$LOOTER could take nothing from a corpse"
	RAN="$RAN $LOOTER:$(echo $LOOTED | tr ' ' '+')"
	tally "$LOOTER" "after the take"; corpses "after the take"
	for s in $STEPS; do
		joined || say "the friend is no longer in the session ($(dc vm 'net status' | grep -oE 'status=[a-z]+'))"
		case "$s" in
		wait) sleep 10; tally "$LOOTER" "10 s after the take"; corpses "10 s after the take";;
		equip)
			piece=""; for i in "${!TOOK[@]}"; do [ "${TOOK[$i]}" -gt 0 ] && [ "${ITYPE[$i]}" = 29 ] && { piece=$i; break; }; done
			[ -n "$piece" ] || { say "equip: no armor piece was taken"; continue; }
			dc "$LOOTER" "console player.equipitem $piece" >/dev/null; sleep 5
			say "equip: $LOOTER wears $piece by console equipitem: $(dc "$LOOTER" equipped | grep -oE "$piece '[^']*'" | head -1)"
			tally "$LOOTER" "after wearing a piece"; corpses "after wearing a piece";;
		osave) say "$O saves (console save ${SAVE}_$O)"; save_by "$O" "${SAVE}_$O"; tally "$LOOTER" "after the other player's save"; corpses "after the other player's ($O) save";;
		lsave) say "$LOOTER saves (console save ${SAVE}_$LOOTER)"; save_by "$LOOTER" "${SAVE}_$LOOTER"; SAVED=1; tally "$LOOTER" "after the looter's save"; corpses "after the looter's ($LOOTER) save";;
		cell)
			say "both leave for $AWAY and come back to $CELL (console coc)"
			enter_both "$AWAY" 20; sleep 10; enter_both "$CELL" 20; sleep 5
			tally "$LOOTER" "after the cell change"; corpses "after the cell change";;
		lload)
			[ -n "$SAVED" ] || { say "lload: no save was made by the looter (lsave first)"; continue; }
			say "$LOOTER loads the save it made after the take (console load ${SAVE}_$LOOTER)"
			load_by "$LOOTER" "${SAVE}_$LOOTER" || say "lload: $LOOTER's game did not come back from the load"
			wait_for 60 joined || say "lload: the friend is not in the session after the load"
			sleep 5; tally "$LOOTER" "after loading that save"; corpses "after the looter's ($LOOTER) load";;
		old)
			say "$LOOTER loads the save from before the fight (console load ${SAVE}_${LOOTER}_old), as a death's reload does"
			load_by "$LOOTER" "${SAVE}_${LOOTER}_old" || say "old: $LOOTER's game did not come back from the load"
			sleep 5; tally "$LOOTER" "after loading the older save" "an older save was loaded"; corpses "after loading the older save" "an older save was loaded";;
		*) say "unknown step '$s'";;
		esac
	done
	for w in host vm; do
		say "$w after: $(dc "$w" 'net status' | grep -oE 'spawns: ours=[^|]*' | cut -c1-200)"
		rsince "$w" "$LOOTER" > "$REPRO_OUT/run-$LOOTER-$w.log"
	done
done
[ -n "$LOSS_INV" ] && verdict YES "looted weapons or armor left the looter's inventory:$LOSS_INV${LOSS_CORPSE:+; and corpses lost with gear on them:$LOSS_CORPSE}"
[ -n "$LOSS_CORPSE" ] && verdict YES "nothing left a looter's inventory, but a corpse was lost with weapons or armor still on it for its player:$LOSS_CORPSE"
verdict NO "every weapon and armor piece taken stayed in the looter's inventory, and every corpse kept what its player had not taken ($RAN; steps: $STEPS)"
