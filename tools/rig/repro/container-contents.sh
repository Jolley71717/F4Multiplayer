#!/bin/bash
# container-contents.sh [bases...]: the friend builds each kind of workshop container through workshop mode's own menu at the host's
# settlement (shared workshop on), and what each box holds is read in both games (player report on 0.22.0, 4 players: "a friend puts a
# box down in front of the host and some boxes spawn random items inside"). A box the workshop builds starts empty.
# Set-up by console, said so: Sanctuary made the host's (console WorkshopScript.SetOwnedByPlayer, as the rig's checks do); both games
# at Sanctuary and joined first (rig.sh join). The friend is in workshop mode by its real E, picks FURNITURE > CONTAINERS > <kind>
# with the menu's keys, turns the ghost green with real mouse moves and puts it down with Enter.
# For each kind: the friend's piece, and the host's game's piece for it (the friend's game's adoption of a host piece, or the host's copy,
# as workshop-build-together reads it). A game's box is read twice: `contents` (what the engine has made of it: nothing until
# something reads it), then `count` (a read of the box's items, which makes the list as opening the box would), then `contents` again.
# The friend's own box in the friend's game is not judged: with PLACE=console (the friend's game makes the box by console, wsreport place/wsfinish: faster,
# said so) the engine's console placement stocks a base's things, which the workshop's own build does not; the host's box is what is judged: the host's game
# made it itself (shared workshop: the friend's build is the host's piece) or holds a copy of the friend's, and it must be empty in every kind.
# The host's workshop is opened once first (a host that never opened it answers a build "hasn't opened that workshop yet" and keeps a copy).
# REPRO: YES when any host's box holds anything after its build, or none reached the host; NO when every host's box is empty.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
SANCT=000250FE
run_cap 6000
BASES=${*:-"0009F42A 00049D9F 00049DA0 00049DA1 00049DA2 00049D9C 0022B17C 000BD53C 001A6E48 000913FA 000913FB 000BD52E 000BD531 00049622 0001F450 00175500 0023D2C8 00198CBE 002381C2 0022B17B 0022B17E 0022B17D 002381BA"}
need_session
[ "$(field "$(dc host rules)" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off"
say "container-contents: bases $BASES; out $REPRO_OUT"
rmark host; rmark vm
for w in host vm; do case "$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")" in *Sanctuary*) ;; *) verdict VOID "$w is not at Sanctuary";; esac; done
[ "$(field "$(dc host "scriptvars $SANCT WorkshopScript OwnedByPlayer")" OwnedByPlayer)" = true ] || { say "set-up: the host's Sanctuary made the host's (console $SANCT.cf WorkshopScript.SetOwnedByPlayer 1)"; dc host "console $SANCT.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3; }
# the host's workshop opened once and left (dev workshop enter / exit, as the rig's other checks do): the engine makes a workbench's workshop data
# as it is first opened, and a host that never opened it answers a friend's build "hasn't opened that workshop yet" (the copy path, not the host's own piece)
dc host "workshop enter $SANCT" >/dev/null; sleep 5
for i in 1 2 3 4 5 6; do
	menu_open host MessageBoxMenu && { dc host focus >/dev/null; sleep 0.3; dc host "key 0x0D 80" >/dev/null; sleep 1.5; }
	dc host "workshop exit" >/dev/null; sleep 3
	[ "$(field "$(dc host workshop)" menu)" = true ] || break
done
say "the host's workshop opened once and left (in it now: $(field "$(dc host workshop)" menu))"
[ "${PLACE:-menu}" = console ] && ws_leave vm
[ "${PLACE:-menu}" = console ] || _ws_in vm || ws_enter_real vm $SANCT || verdict VOID "the friend's real E opened no workshop mode ($ENTER_SAID)"
dc vm "pad unplug" >/dev/null; rwalk vm ${FRIEND_SPOT:--78900 89950} 60
say "friend in workshop mode ($(dc vm wsmode | cut -c1-120))"

_pick() {  # _pick <menu path,...> <base>
	local i g; GHOST=""
	ws_top vm || { PICK_WHY="menu-top"; return 1; }
	# shellcheck disable=SC2046
	ws_nav vm $(tr ',' ' ' <<<"$1") || { PICK_WHY="menu ($WS_NAV)"; return 1; }
	for i in $(seq 1 30); do [ "$(field "$(ws_state vm)" heldBase)" = "$2" ] && break; rkey vm $VK_RIGHT; sleep 0.7; done
	# the menu remembers where it was in the category (the last item: Right stops there): back to the start with Left
	if [ "$(field "$(ws_state vm)" heldBase)" != "$2" ]; then for i in $(seq 1 30); do [ "$(field "$(ws_state vm)" heldBase)" = "$2" ] && break; rkey vm $VK_LEFT; sleep 0.7; done; fi
	[ "$(field "$(ws_state vm)" heldBase)" = "$2" ] || { PICK_WHY="base $2 never held"; return 1; }
	g=$(field "$(ws_state vm)" held); [ -n "$g" ] && [ "$g" != 00000000 ] || { PICK_WHY="no ghost"; return 1; }
	GHOST=$g
}
_put() {
	local g=$GHOST; PUT=""
	ws_green vm || { PUT_WHY="never green ($(ws_state vm | grep -oE 'canPlace=[a-z]+ placeStatus=[0-9]+'))"; return 1; }
	rkey vm $VK_ENTER; sleep 1.5
	if [ "$(field "$(ws_state vm)" held)" = "$g" ] || [ "$(field "$(dc vm "refinfo $g")" deleted)" != false ]; then PUT_WHY="Enter put nothing down"; return 1; fi
	PUT=$g
}
_fate() {  # the host's piece for the friend's $1: HOSTREF
	local p=$1 a h; HOSTREF=""
	a=$(dc vm "wspower copies" | grep -oE "(^|[ :,])$p(\s\(not loaded\))?=[0-9]+:[0-9A-F]{8}" | grep -oE '[0-9A-F]{8}$' | head -1)
	if [ -n "$a" ] && [ "$(field "$(dc host "refinfo $a")" deleted)" = false ]; then HOSTREF=$a; FATE=adopted; return 0; fi
	h=$(ws_copy_of host "$p"); [ -n "$h" ] && { HOSTREF=$h; FATE=copy; return 0; }
	return 1
}
_read() {  # _read <who> <ref>: contents before and after a count
	local w=$1 r=$2 a b; a=$(dc $w "contents $r" | cut -d: -f2-); dc $w "count $r 0000000F" >/dev/null; sleep 0.5; b=$(dc $w "contents $r" | cut -d: -f2-); echo "before:[${a# }] after:[${b# }]"
}
_open() {  # _open <who> <ref>: that player opens the box (the activate, as E), reads what it holds, closes it (Tab). "opened:[...]"
	local w=$1 r=$2 a menu
	dc $w "activate $r" >/dev/null; wait_for 8 menu_open $w ContainerMenu; menu=$?
	sleep 1; a=$(dc $w "contents $r" | cut -d: -f2-)
	if menu_open $w ContainerMenu; then rkey $w $VK_TAB; sleep 1; menu_open $w ContainerMenu && { dc $w "pad button b" >/dev/null; sleep 1; }; fi
	echo "opened:[${a# }]${menu:+ (menu $menu)}"
}
BAD=0; TOTAL=0; RESULTS=""
SPOTS=("-78900 89950" "-79300 90100" "-79750 89150" "-79500 89500" "-78700 90100" "-79100 89700"); N=0
for b in $BASES; do
	# a new open spot for each (a ghost does not go green among the boxes put down before it): a real walk
	[ "${PLACE:-menu}" = console ] || rwalk vm ${SPOTS[$((N++ % ${#SPOTS[@]}))]} 60; dc vm "pad unplug" >/dev/null
	if [ "${PLACE:-menu}" = console ]; then
		# console placement, said so: the friend's game makes the box a step in front of its player (wsreport place ... quiet) and finishes it with the
		# engine's own placement steps (wsfinish), as the rig's workshop checks do; the placed event goes to the host as a build does
		out=$(dc vm "wsreport place $b $SANCT quiet"); PUT=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
		[ -n "$PUT" ] || { say "$b: console placement made nothing ($out)"; RESULTS="$RESULTS $b:void"; continue; }
		sleep 2; dc vm "wsfinish $PUT $SANCT" >/dev/null; sleep 3
	else
	if ! _pick FURNITURE,CONTAINERS "$b"; then say "$b: $PICK_WHY"; RESULTS="$RESULTS $b:void"; continue; fi
	if ! _put; then say "$b: $PUT_WHY"; RESULTS="$RESULTS $b:void"; continue; fi
	fi
	p=$PUT; TOTAL=$((TOTAL + 1))
	if ! wait_for 20 _fate "$p"; then say "$b: friend's $p reached no host piece in 20 s"; RESULTS="$RESULTS $b:MISSING"; BAD=$((BAD + 1)); continue; fi
	sleep 3
	fr=$(_read vm "$p"); hr=$(_read host "$HOSTREF")
	ho=$(_open host "$HOSTREF"); hr="$hr $ho"
	say "$b ($(dc vm "edid $b" | cut -c1-40)): friend's $p: $fr | host's $FATE $HOSTREF: $hr"
	if grep -qE '(after|opened):\[[^]]*[0-9A-F]{8}x' <<<"$hr"; then BAD=$((BAD + 1)); RESULTS="$RESULTS $b:FULL"; else RESULTS="$RESULTS $b:empty"; fi
	grep -qE 'before:\[[^]]*[0-9A-F]{8}x' <<<"$hr" \&\& say "$b: the host's box held things before anything read it"
done
ws_leave vm
rkeep host container-contents; rkeep vm container-contents
for w in host vm; do grep -E 'comes with things|WorkshopSync: not placing|emptied' "$REPRO_OUT/container-contents-$w.log" | tail -8 | while read -r l; do say "$w's log: ${l:0:220}"; done; done
say "boxes:$RESULTS"
[ "$TOTAL" -gt 0 ] || verdict VOID "no box was put down"
[ "$BAD" -gt 0 ] && verdict YES "$BAD of $TOTAL boxes held items or never reached the host"
verdict NO "all $TOTAL boxes empty in both games"
