#!/bin/bash
# workshop-build-together.sh [phases...]: both players build at once, through workshop mode's own menu, and each one's pieces reach
# the other's game (player report on 0.17.0: "when building together my builds as the host don't show for the other player").
# workshop-real-menu.sh has the host build with the friend standing by; here both build, turn about, and the friend is holding a
# ghost of its own in workshop mode each time a piece of the host's goes down. Every piece: picked from the build menu with its keys
# (Right/Left, Enter, Tab), turned green with real mouse moves, put down with Enter; each player walks (a real walk) and presses E.
# Phases (default both, in this order):
#   together      Sanctuary, which the host's game owns (console WorkshopScript.SetOwnedByPlayer, as the rig's checks do) and the
#                 friend's does not (a fresh coc character: the friend's E opens it as the host's, 0.20.2). Both in workshop mode.
#                 Each round: the friend picks a piece and holds its ghost; the host puts its own piece down; then the friend puts
#                 its piece down.
#   friend-owned  Red Rocket, which the friend's game owns (console SetOwnedByPlayer in the VM's game: the stand-in for their claim)
#                 and the host's does not. Both brought there by console (player.moveto the workbench, said so). The friend builds;
#                 then the host claims it (its real E; if that opens nothing, console SetOwnedByPlayer, said so) and builds with
#                 the friend still in workshop mode; then the friend builds again.
# Checks: a host piece: the friend's game holds a copy of it (dev `wspower copies`) within 10 s. A friend's piece: within 20 s either
# the friend's game adopted it as its copy of a host piece the host's game has (the shared workshop), or the host's game holds a
# copy of it (the friend's own piece): which one is printed.
# REPRO: YES when any host piece never reached the friend's game; NO when every one did (a friend's piece missing is printed, and
# also makes it YES: the report's other direction).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
SANCT=000250FE; RR=00054BAE
PHASES=${*:-together friend-owned}
PIECES=${PIECES:-"floor:STRUCTURES,WOOD,FLOORS wall:STRUCTURES,WOOD,WALLS generator:POWER,GENERATORS light:POWER,LIGHTS:000D38D4 bed:FURNITURE,BEDS"}
need_session
say "workshop-build-together: phases $PHASES; out $REPRO_OUT"
[ "$(field "$(dc host rules)" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off"
HOST_ID=$(dc host 'net status' | grep -oE 'id=[0-9]+' | head -1 | cut -d= -f2)
say "host player id $HOST_ID; friend $(dc vm 'net status' | grep -oE 'id=[0-9]+' | head -1)"
rmark host; rmark vm
H_MISSED=0; H_TOTAL=0; F_MISSED=0; F_TOTAL=0; RESULTS=""

_owned() { [ "$(field "$(dc "$1" "scriptvars $2 WorkshopScript OwnedByPlayer")" OwnedByPlayer)" = true ]; }
_own() { dc "$1" "console $2.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3; }
_ownership() { echo "host OwnedByPlayer=$(field "$(dc host "scriptvars $1 WorkshopScript OwnedByPlayer")" OwnedByPlayer), friend's game OwnedByPlayer=$(field "$(dc vm "scriptvars $1 WorkshopScript OwnedByPlayer")" OwnedByPlayer)"; }

# _pick <who> <menu path,...> [base]: the piece's ghost held from the menu. GHOST: the ghost; PICK_WHY: why not
_pick() {
	local who=$1 path=$2 base=$3 i g
	GHOST=""
	ws_top "$who" || { PICK_WHY="menu-top ($(ws_path "$who"))"; return 1; }
	# shellcheck disable=SC2046
	ws_nav "$who" $(tr ',' ' ' <<<"$path") || { PICK_WHY="menu ($WS_NAV)"; return 1; }
	if [ -n "$base" ]; then for i in $(seq 1 12); do [ "$(field "$(ws_state "$who")" heldBase)" = "$base" ] && break; rkey "$who" $VK_RIGHT; sleep 0.7; done; fi
	g=$(field "$(ws_state "$who")" held)
	[ -n "$g" ] && [ "$g" != 00000000 ] || { PICK_WHY="no ghost ($WS_NAV)"; return 1; }
	GHOST=$g
}
# _put <who>: the held ghost turned green and put down with Enter. PUT: the piece; PUT_WHY: why not
_put() {
	local who=$1 g=$GHOST
	PUT=""
	ws_green "$who" || { PUT_WHY="never green ($(ws_state "$who" | grep -oE 'canPlace=[a-z]+ placeStatus=[0-9]+'))"; return 1; }
	rkey "$who" $VK_ENTER; sleep 1.5
	if [ "$(field "$(ws_state "$who")" held)" = "$g" ] || [ "$(field "$(dc "$who" "refinfo $g")" deleted)" != false ]; then PUT_WHY="Enter put nothing down ($(ws_state "$who" | cut -c1-140))"; return 1; fi
	PUT=$g
}
_host_copy() { [ -n "$(ws_copy_of vm "$1")" ]; }
# _friend_fate <friend's piece>: "adopted <host ref>" (the friend's game took it as its copy of the host's piece, and the host's game
# has that piece), "own <host's copy>" (the friend's own piece, copied in the host's game), or nothing yet
_friend_fate() {
	local p=$1 a h
	a=$(dc vm "wspower copies" | grep -oE "(^|[ :,])$p(\s\(not loaded\))?=[0-9]+:[0-9A-F]{8}" | grep -oE '[0-9A-F]{8}$' | head -1)
	if [ -n "$a" ] && [ "$(field "$(dc host "refinfo $a")" deleted)" = false ]; then FATE="adopted as the host's $a"; return 0; fi
	h=$(ws_copy_of host "$p"); [ -n "$h" ] && { FATE="the friend's own, the host's copy $h"; return 0; }
	return 1
}
# _host_piece <phase> <kind> <path> [base]: the host puts one down; the friend's copy within 10 s
_host_piece() {
	local ph=$1 kind=$2
	_pick host "$3" "$4" || { say "$ph host $kind: $PICK_WHY"; RESULTS="$RESULTS $ph/host-$kind:void"; return 1; }
	_put host || { say "$ph host $kind: $PUT_WHY"; RESULTS="$RESULTS $ph/host-$kind:void"; return 1; }
	H_TOTAL=$((H_TOTAL + 1))
	local fs; fs=$(ws_state vm | grep -oE 'menu=[a-z]+|held=[0-9A-F]+' | tr '\n' ' ')
	if wait_for 10 _host_copy "$PUT"; then say "$ph host $kind: host $PUT -> friend's copy $(ws_copy_of vm "$PUT") (friend: $fs)"; RESULTS="$RESULTS $ph/host-$kind:ok"
	else H_MISSED=$((H_MISSED + 1)); say "$ph host $kind: host $PUT NOT in the friend's game after 10 s (friend: $fs); friend's net: $(dc vm 'net status' | grep -oE 'workshop: [^|]*' | cut -c1-260)"; RESULTS="$RESULTS $ph/host-$kind:MISSING"; fi
}
# _friend_put <phase> <kind>: the friend puts the ghost it holds down; its fate within 20 s
_friend_put() {
	local ph=$1 kind=$2
	_put vm || { say "$ph friend $kind: $PUT_WHY"; RESULTS="$RESULTS $ph/friend-$kind:void"; return 1; }
	F_TOTAL=$((F_TOTAL + 1)); local p=$PUT
	if wait_for 20 _friend_fate "$p"; then say "$ph friend $kind: friend's $p: $FATE"; RESULTS="$RESULTS $ph/friend-$kind:ok"
	else F_MISSED=$((F_MISSED + 1)); say "$ph friend $kind: friend's $p reached neither game's other side in 20 s; host net: $(dc host 'net status' | grep -oE 'workshop: [^|]*' | cut -c1-200)"; RESULTS="$RESULTS $ph/friend-$kind:MISSING"; fi
}
_friend_piece() {
	_pick vm "$3" "$4" || { say "$1 friend $2: $PICK_WHY"; RESULTS="$RESULTS $1/friend-$2:void"; return 1; }
	_friend_put "$1" "$2"
}
_in_ws() {  # _in_ws <who> <bench> <phase>: in workshop mode by a real E
	_ws_in "$1" && return 0
	ws_enter_real "$1" "$2" && { say "$3: $1 in workshop mode by its real E ($ENTER_SAID)"; return 0; }
	say "$3: $1's real E opened no workshop mode ($ENTER_SAID; rollover '$ROLLOVER')"; return 1
}

for ph in $PHASES; do
	case "$ph" in
	together)
		for w in host vm; do
			case "$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")" in *Sanctuary*) ;; *) say "set-up: $w to Sanctuary (console coc SanctuaryExt)"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
		done
		_owned host $SANCT || { say "set-up: the host's Sanctuary made the host's (console $SANCT.cf WorkshopScript.SetOwnedByPlayer 1)"; _own host $SANCT; }
		say "together: Sanctuary: $(_ownership $SANCT)"
		_in_ws host $SANCT together || { RESULTS="$RESULTS together:void"; continue; }
		dc host "pad unplug" >/dev/null; rwalk host ${HOST_SPOT:--79750 89150} 60
		_in_ws vm $SANCT together || { RESULTS="$RESULTS together:void"; continue; }
		rwalk vm ${FRIEND_SPOT:--78900 89950} 60
		say "together: both in workshop mode, $(dist_xy "$(pos_of host)" "$(pos_of vm)") u apart; friend's wsmode: $(dc vm wsmode | cut -c1-160)"
		for p in $PIECES; do
			kind=${p%%:*}; rest=${p#*:}; path=${rest%%:*}; base=""; [ "$rest" != "$path" ] && base=${rest#*:}
			if _pick vm "$path" "$base"; then F_HOLD=$GHOST; else say "together friend $kind: $PICK_WHY"; F_HOLD=""; fi
			_host_piece together "$kind" "$path" "$base"
			if [ -n "$F_HOLD" ]; then GHOST=$F_HOLD; _friend_put together "$kind"; else RESULTS="$RESULTS together/friend-$kind:void"; fi
		done
		ws_leave host; ws_leave vm ;;
	friend-owned)
		# RR_FROM_CLAIM=1: both at Red Rocket already and the friend built there (a run stopped at the host's claim): from the claim on
		if [ "${RR_FROM_CLAIM:-0}" != 1 ]; then
			for w in host vm; do
				say "set-up: $w brought to Red Rocket (console player.moveto $RR)"; dc $w focus >/dev/null; dc $w "console player.moveto $RR" >/dev/null; sleep 6; wait_ingame $w 60
			done
			_owned vm $RR || { say "set-up: the friend's game made to own Red Rocket (console $RR.cf WorkshopScript.SetOwnedByPlayer 1 in the VM's game: the stand-in for their claim)"; _own vm $RR; }
			_owned host $RR && verdict VOID "the host's game already owns Red Rocket: a fresh host game is needed for this phase"
			say "friend-owned: Red Rocket: $(_ownership $RR)"
			_in_ws vm $RR friend-owned || { RESULTS="$RESULTS friend-owned:void"; continue; }
		fi
		read -r bx by <<<"$(field "$(dc vm "refinfo $RR")" x) $(field "$(dc vm "refinfo $RR")" y)"
		if [ "${RR_FROM_CLAIM:-0}" != 1 ]; then
			rwalk vm "${FRIEND_RR:-$(( ${bx%.*} + 500 )) $(( ${by%.*} - 300 ))}" 80
			for p in floor:STRUCTURES,WOOD,FLOORS wall:STRUCTURES,WOOD,WALLS; do _friend_piece friend-owned-before "${p%%:*}" "${p#*:}"; done
		fi
		# the friend out of the host's way: its stand-in at the bench would take the host's E (a real walk off)
		rwalk vm "${FRIEND_RR:-$(( ${bx%.*} + 500 )) $(( ${by%.*} - 300 ))}" 80
		say "friend-owned: the friend is $(dist_xy "$(pos_of vm)" "${bx%.*} ${by%.*}") u from the workbench ($WALKED)"
		# the host claims it with its real E (a cleared Red Rocket is claimed by the workbench's own script); console only if not
		if ! _in_ws host $RR friend-owned; then
			say "set-up: the host's Red Rocket made the host's (console $RR.cf WorkshopScript.SetOwnedByPlayer 1: the stand-in for the claim)"; _own host $RR
			_in_ws host $RR friend-owned || { RESULTS="$RESULTS friend-owned/host:void"; continue; }
		fi
		_in_ws vm $RR friend-owned || { RESULTS="$RESULTS friend-owned/friend:void"; continue; }
		say "friend-owned: after the host's claim: $(_ownership $RR); friend's wsmode: $(dc vm wsmode | cut -c1-160)"
		rwalk host "${HOST_RR:-$(( ${bx%.*} - 500 )) $(( ${by%.*} - 300 ))}" 80
		for p in $PIECES; do
			kind=${p%%:*}; rest=${p#*:}; path=${rest%%:*}; base=""; [ "$rest" != "$path" ] && base=${rest#*:}
			if _pick vm "$path" "$base"; then F_HOLD=$GHOST; else say "friend-owned friend $kind: $PICK_WHY"; F_HOLD=""; fi
			_host_piece friend-owned "$kind" "$path" "$base"
			if [ -n "$F_HOLD" ]; then GHOST=$F_HOLD; _friend_put friend-owned "$kind"; else RESULTS="$RESULTS friend-owned/friend-$kind:void"; fi
		done
		ws_leave host; ws_leave vm ;;
	esac
done
rkeep host workshop-build-together; rkeep vm workshop-build-together
say "pieces: $RESULTS"
for w in host vm; do grep -E 'WorkshopSync: not placing|refused|not the host' "$REPRO_OUT/workshop-build-together-$w.log" | tail -6 | while read -r l; do say "$w's log: ${l:0:240}"; done; done
[ "$H_TOTAL" -gt 0 ] || verdict VOID "no host piece was put down"
say "host pieces: $((H_TOTAL - H_MISSED)) of $H_TOTAL reached the friend; friend's pieces: $((F_TOTAL - F_MISSED)) of $F_TOTAL reached the host"
if [ "$H_MISSED" -gt 0 ] || [ "$F_MISSED" -gt 0 ]; then verdict YES "$H_MISSED of $H_TOTAL host pieces and $F_MISSED of $F_TOTAL friend's pieces, built together through the real menu, never reached the other game"; fi
verdict NO "all $H_TOTAL host pieces and $F_TOTAL friend's pieces, built together through the real menu, reached the other game"
