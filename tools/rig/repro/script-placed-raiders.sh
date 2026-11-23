#!/bin/bash
# script-placed-raiders.sh [after|together]: a raider a quest's script places (PlaceActorAtMe at its marker) in each game, seen
# and hit by both players. Every game's script runs on its own and makes its own raider, at the same marker, under its own runtime
# id. 0.21.1 shared each game's raider plain: each player saw their own raider and a copy of the friend's beside it (doubled), the
# friend's copy standing where the friend's raider stood. With the fix the second game's raider is the first's twin (same base,
# made within 512 u of where the first was shared, run by no other system) and is paired with it: one raider in each game, and a
# hit on it in either game reaches the other.
#   1. Both players go to the Special Delivery reinforcements' marker (0019A7F6; console player.moveto, said), each with a 10mm and
#      its rounds (console additem), and walk (the pad) about 800 u from it.
#   2. The raider is placed at the marker in each game with console 0019A7F6.placeatme 001AE38D 1, standing for the quest
#      script's PlaceActorAtMe (no quest names it: a script's PlaceAtMe makes a plain runtime NPC), and held where it stands
#      (console setrestrained 1) so the shots can land. after (the default): the host's first, the VM's 10 s later, once the
#      host's is shared (the friend's game waits for or pairs with the shared one). together: both at once (both games may share
#      theirs; the higher player id lets go of its own and pairs it).
#   3. The game whose raider is paired shoots it: the other game's raider must take the hit. Then the other game shoots its own:
#      the paired one must take it.
# REPRO: YES when a game holds a copy of the friend's raider beside its own (doubled), when neither raider is paired with the
# other, or when a landed hit stays in the shooter's game. NO when exactly one is paired, there is no copy, and both directions'
# hits reach the other game. VOID when the scene could not be made (no raider placed, no shot landing in 12, a click that fired
# nothing).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
MODE=${1:-after}
case "$MODE" in after|together) ;; *) echo "usage: script-placed-raiders.sh [after|together]"; exit 2 ;; esac
MARKER=${MARKER:-0019A7F6}  # Special Delivery's ReinforcementMarker: a persistent spot both games have
RAIDER=${RAIDER:-001AE38D}  # the raider base the rig's VATS check places
AWAY=${AWAY:-800}
run_cap 1500
need_session

info() { dc "$1" "refinfo $2"; }
hp() { field "$(info "$1" "$2")" hp; }
dead() { [ "$(field "$(info "$1" "$2")" dead)" = true ]; }
xy_of() { local i; i=$(info "$1" "$2"); echo "$(field "$i" x) $(field "$i" y)"; }
owner_of() { field "$(dc "$1" "ghosts $2")" owner; }
shots_of() { dc "$1" "net status" | grep -oE ' shots=[0-9]+' | head -1 | cut -d= -f2; }
lower() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a != "" && b != "" && a + 0 < b - 0.5) }'; }
# raiders <who>: the runtime (0xFF) NPCs named like a raider within 600 u of the marker in that game, switched on
raiders() {
	local r i d
	for r in $(dc "$1" "refs 45 6000 Raider" | tr ';' '\n' | grep -oE '^ ?FF[0-9A-F]{6}' | tr -d ' '); do
		i=$(info "$1" "$r"); [ "$(field "$i" disabled)" = true ] && continue
		d=$(dist_xy "$(field "$i" x) $(field "$i" y)" "$MXY"); [ -n "$d" ] && [ "$d" -le 600 ] && echo "$r"
	done
}
# place <who>: that game's raider at the marker; PLACED = the new one, held where it stands
place() {
	local w=$1 before r; before=$(raiders "$w"); PLACED=""
	dc "$w" "console $MARKER.placeatme $RAIDER 1" >/dev/null; sleep 2
	for r in $(raiders "$w"); do grep -qx "$r" <<<"$before" || { PLACED=$r; break; }; done
	[ -n "$PLACED" ] && dc "$w" "console $PLACED.setrestrained 1" >/dev/null
}
fire_until() {
	local s=$1 t=$2 h0=$3 i h; LANDED=""
	for i in $(seq 1 12); do
		[ $((i % 4)) -eq 1 ] && { rwalk $s "$t" 300 || true; }
		dc $s "turnto $t 300 aim" >/dev/null; sleep 0.5; dc $s "click 80" >/dev/null; sleep 1.2
		h=$(hp $s "$t"); if dead $s "$t" || lower "$h" "$h0"; then LANDED=$i; return 0; fi
	done
	return 1
}

# 1. both players near the marker, armed
for w in host vm; do
	say "set-up: $w goes to the marker (console player.moveto $MARKER)"
	dc $w focus >/dev/null; dc $w "console player.moveto $MARKER" >/dev/null; sleep 12; wait_ingame $w 90 || verdict VOID "$w is not in the game after the move"
	dc $w "console player.additem 00004822 1" >/dev/null; dc $w "console player.additem 0001F276 300" >/dev/null
	dc $w "console player.equipitem 00004822" >/dev/null; dc $w "draw on" >/dev/null
done
MXY=$(xy_of host "$MARKER"); [ -n "$(echo $MXY | tr -d ' ')" ] || verdict VOID "the host's game has no marker $MARKER"
sleep 5
mx=${MXY% *}; my=${MXY#* }; placed=""
for dir in "1 0" "0 1" "-1 0" "0 -1"; do
	dx=${dir% *}; dy=${dir#* }
	tx=$(awk -v m="$mx" -v d="$dx" -v a="$AWAY" 'BEGIN{printf "%d", m + d * a}'); ty=$(awk -v m="$my" -v d="$dy" -v a="$AWAY" 'BEGIN{printf "%d", m + d * a}')
	rwalk host "$tx" "$ty" 150 || true
	d=$(dist_xy "$(pos_of host)" "$MXY"); [ -n "$d" ] && [ "$d" -ge $((AWAY * 6 / 10)) ] && { placed="$tx $ty"; break; }
	say "set-up: the host's walk toward ($tx, $ty) ended $d u from the marker; another way"
done
[ -n "$placed" ] || verdict VOID "the host could not walk $AWAY u from the marker any way"
rwalk vm $(awk -v p="$(pos_of host)" -v dx="$dx" -v dy="$dy" 'BEGIN{split(p,q," "); printf "%d %d", q[1] + dy * 250, q[2] + dx * 250}') 150 || true
say "set-up: marker at $MXY; host $(dist_xy "$(pos_of host)" "$MXY") u from it, vm $(dist_xy "$(pos_of vm)" "$MXY") u (walked, the pad)"
HID=$(field "$(dc host 'net status')" id); VID=$(field "$(dc vm 'net status')" id)
say "set-up: player ids: host $HID, vm $VID; raiders at the marker before: host [$(raiders host | tr '\n' ' ')] vm [$(raiders vm | tr '\n' ' ')]"
rmark host run; rmark vm run

# 2. the raider placed in each game
if [ "$MODE" = after ]; then
	place host; RH=$PLACED; say "the host's raider placed (console $MARKER.placeatme $RAIDER 1, held): [$RH]"; sleep 10
	place vm; RV=$PLACED; say "the VM's raider placed 10 s later: [$RV]"
else
	BH=$(raiders host); BV=$(raiders vm)
	dc host "console $MARKER.placeatme $RAIDER 1" >/dev/null & dc vm "console $MARKER.placeatme $RAIDER 1" >/dev/null & wait
	sleep 1   # placed, not yet copied by the friend's game (a copy waits for the share, a second or more): a new one run by the friend is a copy
	RH=$(for r in $(raiders host | grep -vxF -f <(printf '%s\n' $BH x)); do [ "$(owner_of host "$r")" = "$VID" ] || echo "$r"; done | head -1)
	RV=$(for r in $(raiders vm | grep -vxF -f <(printf '%s\n' $BV x)); do [ "$(owner_of vm "$r")" = "$HID" ] || echo "$r"; done | head -1)
	[ -n "$RH" ] && dc host "console $RH.setrestrained 1" >/dev/null; [ -n "$RV" ] && dc vm "console $RV.setrestrained 1" >/dev/null
	say "both raiders placed at once: host's [$RH], VM's [$RV]"
fi
[ -n "$RH" ] && [ -n "$RV" ] || verdict VOID "a game placed no raider (host [$RH], VM [$RV])"
sleep 15   # shared or held, copies placed or discarded, pairs made (once a second)
OH=$(owner_of host "$RH"); OV=$(owner_of vm "$RV")
say "owners as the session has them: the host's $RH owner=$OH, the VM's $RV owner=$OV (the friend's id = paired with the friend's)"
CH=$(for r in $(raiders host); do [ "$r" = "$RH" ] && continue; [ "$(owner_of host "$r")" = "$VID" ] && ! dead host "$r" && echo "$r"; done | tr '\n' ' ')
CV=$(for r in $(raiders vm); do [ "$r" = "$RV" ] && continue; [ "$(owner_of vm "$r")" = "$HID" ] && ! dead vm "$r" && echo "$r"; done | tr '\n' ' ')
say "copies beside each game's own: host's of the VM's [$CH], VM's of the host's [$CV]"
rsince host run | grep -E "Spawns: .*(twin|lets go|player $VID's|$RH)" | tail -10 | sed 's/^/  host| /' | tee -a "$REPRO_OUT/run.log"
rsince vm run | grep -E "Spawns: .*(twin|lets go|player $HID's|$RV)" | tail -10 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
BAD=""
[ -n "$(echo $CH)" ] && BAD="$BAD the host's game holds a copy of the VM's raider [$CH] beside its own $RH (doubled);"
[ -n "$(echo $CV)" ] && BAD="$BAD the VM's game holds a copy of the host's raider [$CV] beside its own $RV (doubled);"
[ -n "$BAD" ] && { rkeep host script-placed-raiders; rkeep vm script-placed-raiders; verdict YES "a script-placed raider is each game's own, shared beside the friend's:$BAD"; }
if [ "$OV" = "$HID" ]; then P=vm; RP=$RV; S=host; RS=$RH
elif [ "$OH" = "$VID" ]; then P=host; RP=$RH; S=vm; RS=$RV
else rkeep host script-placed-raiders; rkeep vm script-placed-raiders; verdict YES "neither raider is paired with the other (host's $RH owner=$OH, VM's $RV owner=$OV), and no copy stands beside them: each player fights a raider the other cannot see"
fi
say "the $P's raider $RP is paired with the $S's $RS"

# 3. the paired side shoots, then the shared side
DONE=0
for turn in "$P $RP $S $RS" "$S $RS $P $RP"; do
	set -- $turn; sh=$1; t=$2; o=$3; ot=$4
	dead "$sh" "$t" && { say "[$sh shoots] $t is dead already: turn skipped"; continue; }
	rmark host $sh; rmark vm $sh
	h0=$(hp "$sh" "$t"); oh0=$(hp "$o" "$ot")
	say "[$sh shoots] its $t at $h0 health; the $o's $ot at $oh0"
	step_aside "$o" "$t" 400 || true
	n0=$(shots_of "$sh"); fire_until "$sh" "$t" "$h0"; n1=$(shots_of "$sh")
	[ -n "$n0" ] && [ -n "$n1" ] && [ "$n1" -gt "$n0" ] || verdict VOID "the $sh's clicks fired nothing (shots=$n0 before, $n1 after)"
	[ -n "$LANDED" ] || verdict VOID "none of the $sh's 12 shots at $t landed (shots=$n0 -> $n1)"
	sleep 5
	oh1=$(hp "$o" "$ot")
	say "[$sh shoots] shot $LANDED landed: the $sh's $t $h0 -> $(hp "$sh" "$t"); the $o's $ot $oh0 -> $oh1$(dead "$o" "$ot" && echo ' (dead)')"
	rsince "$o" "$sh" | grep -E "CopyWatch|damage|health|$ot" | tail -6 | sed "s/^/  $o| /" | tee -a "$REPRO_OUT/run.log"
	if dead "$o" "$ot" || lower "$oh1" "$oh0"; then DONE=$((DONE + 1)); say "[$sh shoots] the hit reached the $o's game"
	else BAD="$BAD the $sh's landed shot on $t left the $o's $ot as it was;"; fi
done
rkeep host script-placed-raiders; rkeep vm script-placed-raiders
[ -n "$BAD" ] && verdict YES "the paired raiders did not share a hit:$BAD"
verdict NO "one raider in each game, paired ($P's $RP with $S's $RS, no copy beside them), and each landed shot ($DONE turn(s)) reached the other game"
