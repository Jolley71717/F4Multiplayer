#!/bin/bash
# workshop-real-menu.sh [phases...]: the host's builds through workshop mode's own menu reach the friend's game (player report on
# 0.17.0: "when building together my builds as the host don't show for the other player"). The rig's workshop-together check
# places through the dev channel (wsreport/wsfinish); this one uses the real menu: the host walks to Sanctuary's workbench and
# presses E, picks each piece from the build menu with its keys (Right/Left, Enter, Tab), turns it green with real mouse moves and
# puts it down with Enter; the generator and the light are wired with workshop mode's wire key (Space) on each in turn.
# Pieces: a wood floor, a wood wall, a small generator, a construction light, the wire between them, a bed, a turret.
# Phases (default all three, in this order):
#   near-in    the friend in workshop mode at the same workbench, a few metres off. To get in, the friend's own game must own
#              Sanctuary: console WorkshopScript.SetOwnedByPlayer in the VM's game stands in for the friend claiming it, then the
#              friend's real E (a friend whose game doesn't own it is refused: workbench-not-owned.sh)
#   near-out   the friend out of workshop mode, standing near
#   far        the friend out of workshop mode, walked about 1500 u off
# For each piece: the friend's game holds a copy of the host's reference (dev `wspower copies`) within 10 s.
# Set-up by console, said so: both games coc SanctuaryExt when not there; the host's Sanctuary made the host's
# (WorkshopScript.SetOwnedByPlayer, as the bench and workshop-together checks do). Both games joined first (rig.sh join).
# REPRO: YES when any host piece of any phase never reached the friend's game; NO when every one did.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE
PHASES=${*:-near-in near-out far}
PIECES=${PIECES:-"floor:STRUCTURES,WOOD,FLOORS wall:STRUCTURES,WOOD,WALLS generator:POWER,GENERATORS light:POWER,LIGHTS:000D38D4 wire:- bed:FURNITURE,BEDS turret:DEFENSE,TURRETS"}
need_session
say "workshop-real-menu: phases $PHASES; out $REPRO_OUT"
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) say "set-up: $w is not at Sanctuary ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
[ "$(field "$(dc host rules)" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off (the rules say sharedWorkshop=$(field "$(dc host rules)" sharedWorkshop))"
if [ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
rmark host; rmark vm
if ! _ws_in host; then
	ws_enter_real host $BENCH || verdict VOID "the host's real E at its workbench opened no workshop mode ($ENTER_SAID; rollover '$ROLLOVER')"
	say "the host is in workshop mode by its real E ($ENTER_SAID)"
fi
# the host walks (a real walk) to open ground west of the bench, so its ghosts have room (HOST_SPOT="x y" for another spot, as a
# rerun in the same world finds its earlier pieces there)
dc host "pad unplug" >/dev/null; rwalk host ${HOST_SPOT:--79750 89150} 60

MISSED=0; TOTAL=0; RESULTS=""
# _place <phase> <kind> <menu path,...> [base]: the piece from the menu, green, Enter; the friend's copy within 10 s
_place() {
	local ph=$1 kind=$2 path=$3 base=$4 g gb i
	ws_top host || { say "$ph $kind: the menu did not go back to its top ($(ws_path host); $(ws_state host | cut -c1-120))"; RESULTS="$RESULTS $ph/$kind:void(menu-top)"; return 1; }
	# shellcheck disable=SC2046
	ws_nav host $(tr ',' ' ' <<<"$path") || { say "$ph $kind: $WS_NAV"; RESULTS="$RESULTS $ph/$kind:void(menu)"; return 1; }
	if [ -n "$base" ]; then for i in $(seq 1 12); do [ "$(field "$(ws_state host)" heldBase)" = "$base" ] && break; rkey host $VK_RIGHT; sleep 0.7; done; fi
	g=$(field "$(ws_state host)" held); gb=$(field "$(ws_state host)" heldBase)
	[ -n "$g" ] && [ "$g" != 00000000 ] || { say "$ph $kind: no ghost held ($WS_NAV)"; RESULTS="$RESULTS $ph/$kind:void(ghost)"; return 1; }
	ws_green host || { say "$ph $kind: the ghost $g never green ($(ws_state host | grep -oE 'canPlace=[a-z]+ placeStatus=[0-9]+'))"; RESULTS="$RESULTS $ph/$kind:void(green)"; return 1; }
	rkey host $VK_ENTER; sleep 1.5
	if [ "$(field "$(ws_state host)" held)" = "$g" ] || [ "$(field "$(dc host "refinfo $g")" deleted)" != false ]; then say "$ph $kind: the host's Enter put no piece down ($(ws_state host | cut -c1-160))"; RESULTS="$RESULTS $ph/$kind:void(place)"; return 1; fi
	PLACED=$g; TOTAL=$((TOTAL + 1))
	if wait_for 10 _has_copy "$g"; then say "$ph $kind: host $g (base $gb) -> friend's copy $(ws_copy_of vm "$g")"; RESULTS="$RESULTS $ph/$kind:ok"
	else MISSED=$((MISSED + 1)); say "$ph $kind: host $g NOT in the friend's game after 10 s; friend: $(dc vm 'net status' | grep -oE 'workshop: [^|]*' | cut -c1-260)"; RESULTS="$RESULTS $ph/$kind:MISSING"; fi
}
_has_copy() { [ -n "$(ws_copy_of vm "$1")" ]; }
# _near_host: the friend walks (a real walk) to a few metres from the host
_near_host() { local x y; read -r x y <<<"$(pos_of host)"; rwalk vm "$(( ${x%.*} + 250 ))" "$(( ${y%.*} + 150 ))" 80; }
# _wire <phase> <generator> <light>: the host looks at each end and presses the wire key on it
_wire() {
	local ph=$1 a=$2 b=$3 c0 wire
	ws_top host
	[ "$(field "$(ws_state host)" held)" = 00000000 ] || { rkey host $VK_TAB; sleep 1; }
	dc host "turnto $a 300 aim" >/dev/null; sleep 1
	# out of the highlight's reach: the host walks (real walk) to about 150 u of the generator and looks again
	[ "$(field "$(ws_state host)" selected)" = "$a" ] || { rwalk host "$a" 150 0.5; dc host "turnto $a 300 aim" >/dev/null; sleep 1; }
	[ "$(field "$(ws_state host)" selected)" = "$a" ] || { say "$ph wire: the generator $a is not highlighted ($(ws_state host | grep -oE 'selected=[0-9A-F]+'))"; RESULTS="$RESULTS $ph/wire:void(aim)"; return 1; }
	rkey host $VK_SPACE; sleep 1.5
	wire=$(field "$(ws_state host)" held)
	# the far end: turned to by its position (aiming at the light's bound middle left the wire's target on the generator), then the
	# look swept down until the game takes it as the wire's end (wireStatus=ok)
	dc host "turnto $b 600" >/dev/null; sleep 1
	for p in 15 25 35 45; do pitch_down host $p; sleep 0.5; [ "$(field "$(ws_state host)" wireStatus)" = ok ] && break; done
	rkey host $VK_SPACE; sleep 2
	if [ "$(field "$(ws_state host)" held)" != 00000000 ] || [ "$(field "$(dc host "refinfo $wire")" deleted)" != false ]; then say "$ph wire: not strung ($(ws_state host | cut -c1-200))"; RESULTS="$RESULTS $ph/wire:void(string)"; [ "$(field "$(ws_state host)" held)" != 00000000 ] && rkey host $VK_TAB; return 1; fi
	TOTAL=$((TOTAL + 1))
	if wait_for 10 _has_copy "$wire"; then say "$ph wire: host $wire -> friend's copy $(ws_copy_of vm "$wire")"; RESULTS="$RESULTS $ph/wire:ok"
	else MISSED=$((MISSED + 1)); say "$ph wire: host $wire NOT in the friend's game after 10 s"; RESULTS="$RESULTS $ph/wire:MISSING"; fi
}

for ph in $PHASES; do
	case "$ph" in
	near-in)
		if [ "$(field "$(dc vm "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
			say "set-up: the friend's own game made to own Sanctuary (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1 in the VM's game: the stand-in for the friend claiming it)"
			dc vm "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
		fi
		_ws_in vm || ws_enter_real vm $BENCH || { say "near-in: the friend's real E opened no workshop mode ($ENTER_SAID)"; RESULTS="$RESULTS near-in:void"; continue; }
		_near_host; say "near-in: the friend is in workshop mode (walked over: $WALKED), $(dist_xy "$(pos_of host)" "$(pos_of vm)") u from the host" ;;
	near-out)
		ws_leave vm
		_ws_in vm && { say "near-out: the friend did not leave workshop mode"; RESULTS="$RESULTS near-out:void"; continue; }
		_near_host; say "near-out: the friend is out of workshop mode ($WS_LEFT), $(dist_xy "$(pos_of host)" "$(pos_of vm)") u from the host" ;;
	far)
		ws_leave vm
		_ws_in vm && { say "far: the friend did not leave workshop mode"; RESULTS="$RESULTS far:void"; continue; }
		rwalk vm -80000 88900 150; rwalk vm -80800 88200 150
		d=$(dist_xy "$(pos_of host)" "$(pos_of vm)")
		say "far: the friend walked off ($WALKED), out of workshop mode ($WS_LEFT), $d u from the host"
		[ "${d:-0}" -ge 1200 ] || { say "far: the friend is only ${d:-?} u off, not about 1500"; RESULTS="$RESULTS far:void(walk)"; continue; } ;;
	esac
	GEN=""; LIGHT=""
	for p in $PIECES; do
		kind=${p%%:*}; rest=${p#*:}; path=${rest%%:*}; base=""; [ "$rest" != "$path" ] && base=${rest#*:}
		case "$kind" in
		wire) [ -n "$GEN" ] && [ -n "$LIGHT" ] && _wire "$ph" "$GEN" "$LIGHT" ;;
		*) PLACED=""; _place "$ph" "$kind" "$path" "$base"; [ "$kind" = generator ] && GEN=$PLACED; [ "$kind" = light ] && LIGHT=$PLACED ;;
		esac
	done
done
ws_leave host; ws_leave vm
rkeep host workshop-real-menu; rkeep vm workshop-real-menu
say "pieces: $RESULTS"
grep -E 'WorkshopSync: not placing|refused' "$REPRO_OUT/workshop-real-menu-vm.log" | tail -5 | while read -r l; do say "friend's log: ${l:0:240}"; done
[ "$TOTAL" -gt 0 ] || verdict VOID "no host piece was put down"
if [ "$MISSED" -gt 0 ]; then verdict YES "$MISSED of $TOTAL host pieces placed through the real menu never reached the friend's game"; fi
verdict NO "all $TOTAL host pieces placed through the real menu reached the friend's game"
