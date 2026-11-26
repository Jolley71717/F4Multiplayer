#!/bin/bash
# shared-build-move.sh: a friend's own build at the host's settlement becomes the host's piece (the shared workshop, by design),
# and the friend then can't pick it up again with workshop mode's Accept key (player report on 0.20.1: "if I build a house as a
# client, the game will think my house's parts are the host's, and so I won't be able to move them with E; scrapping works").
# Set-up by console, said so: both games coc SanctuaryExt when not there; the host's Sanctuary made the host's
# (WorkshopScript.SetOwnedByPlayer, as the rig's other workshop scripts do); the host opens its workshop once with its real E (the
# engine makes a workbench's workshop data on its first opening) and leaves. The friend walks to open ground north of the bench
# (real walk; placed there by console, said, when the walk falls short), holds the workshop key (real key; its E at the workbench
# when that opens nothing), builds a wood floor through the real menu (its keys, real mouse moves, Enter), and its game adopts the floor as the
# host's piece (a copy). Then the friend aims at the floor and presses workshop mode's Accept key (the game's own binding, read from
# the friend's control map: E on a keyboard) to pick it up, as a player moving their own build would.
# REPRO: YES when the key picks nothing up (the HUD's refusal and the friend's log line are printed); NO when the engine holds the
# copy. With it held, the move itself is tried too, as the proof that the piece can be moved: the friend turns (real mouse moves; real
# steps when turning finds no spot) until the game says the floor can be put down a step away, presses Accept, and the host's game is asked to move its floor there
# (the request's answer and where the host's floor stands are printed; a carry that found no spot is said, and the floor let go
# with Cancel).  Both games joined first (rig.sh join).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE
SPOT=${FRIEND_SPOT:--78900 89950}; VK_WORKSHOP=${VK_WORKSHOP:-0x56}
# BUDGET: the seconds the set-up may take before the run stops itself with VOID (a slow rig), so a turn stays short
BUDGET=${BUDGET:-540}; _late() { [ "$SECONDS" -le "$BUDGET" ] || verdict VOID "no verdict within $BUDGET s ($SECONDS s gone at: $1): the rig is too slow this turn"; }
need_session
say "shared-build-move: out $REPRO_OUT"
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) say "set-up: $w is not at Sanctuary ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
RULES=$(dc host rules)
[ "$(field "$RULES" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off (sharedWorkshop=$(field "$RULES" sharedWorkshop))"
say "host rules: sharedWorkshop=$(field "$RULES" sharedWorkshop) sharedWorkshopEdit=$(field "$RULES" sharedWorkshopEdit) sharedStorage=$(field "$RULES" sharedStorage); friend's grab switch (wsmode): grab=$(field "$(dc vm wsmode)" grab)"
if [ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
if ! _ws_in host; then
	ws_enter_real host $BENCH || verdict VOID "the host's real E at its own workbench opened no workshop mode ($ENTER_SAID)"
	say "the host opened its workshop with its real E ($ENTER_SAID)"
fi
ws_leave host   # the host stays by its workbench: the friend builds 400 u north of it
# the friend's workshop mode keys, from its own control map (the events are the pad's names: Accept grabs, Cancel lets go)
KEYS=$(dc vm "controls workshop")
VK_ACCEPT=$(grep -oE "(^| )Accept=[0-9A-F]+" <<<"$KEYS" | head -1 | sed 's/.*=/0x/'); VK_CANCEL=$(grep -oE "(^| )Cancel=[0-9A-F]+" <<<"$KEYS" | head -1 | sed 's/.*=/0x/')
[ -n "$VK_ACCEPT" ] && [ -n "$VK_CANCEL" ] || verdict VOID "workshop mode's Accept or Cancel key is not in the friend's control map ($(cut -c1-200 <<<"$KEYS"))"
say "friend's workshop keys: Accept=$VK_ACCEPT Cancel=$VK_CANCEL"

# The friend goes to open ground north of the bench first (a real walk, outside workshop mode: the pad's stick did not move a
# friend inside it on 2026-11-21), looks a little down, and opens workshop mode there with the workshop key held (V: the game's
# own key, which the mod lets open the host's settlement too). When the key opens nothing, by the E at the workbench instead.
_ws_in vm && ws_leave vm
for wp in ${FRIEND_WAY:-"-79600 90300" "-79100 90250"} "$SPOT"; do
	[ "$(dist_xy "$(pos_of vm)" "$SPOT")" -le 150 ] 2>/dev/null && break
	rwalk vm $wp 70; _late "the friend's walk to its build spot"
done
FAR=$(dist_xy "$(pos_of vm)" "$SPOT")
if ! [ "${FAR:-9999}" -le 250 ] 2>/dev/null; then
	read -r sx sy <<<"$SPOT"
	say "set-up: the friend's walk ended $FAR u from its build spot ($WALKED): placed there by console (player.setpos)"
	dc vm focus >/dev/null; dc vm "console player.setpos x $sx" >/dev/null; dc vm "console player.setpos y $sy" >/dev/null; sleep 3
	FAR=$(dist_xy "$(pos_of vm)" "$SPOT")
fi
say "the friend stands $FAR u from its build spot ($SPOT), $(dist_xy "$(pos_of vm)" "$(pos_of host)") u from the host"
pitch_down vm 25
rkey vm "$VK_WORKSHOP" 1500; sleep 2
if wait_for 6 _ws_in vm; then
	OPENED_BY="the workshop key held (real key ${VK_WORKSHOP#0x})"
else
	say "the friend's held workshop key opened no workshop mode (hud='$(rhud vm)'): by the E at the workbench instead"
	ws_enter_real vm $BENCH || verdict VOID "the friend's real E at the host's workbench opened no workshop mode either ($ENTER_SAID; rollover '$ROLLOVER')"
	OPENED_BY="its real E at the workbench ($ENTER_SAID), $(dist_xy "$(pos_of vm)" "$SPOT") u from the build spot"
fi
sleep 1.5; BOX=$(rmsgbox vm); [ -n "$BOX" ] && { rkey vm $VK_ENTER; sleep 1; }
say "the friend is in workshop mode by $OPENED_BY; menus '$(menus vm)'${BOX:+; a message box closed with Enter}"
_late "the friend's workshop mode"
# the look must answer the mouse before the menu's long walk through its keys: a rig that doesn't turn it is said at once
RZ0=$(field "$(dc vm 'refinfo 00000014')" rz); dc vm "mouse 400 0" >/dev/null; sleep 1; RZ1=$(field "$(dc vm 'refinfo 00000014')" rz)
TURNED=$(awk -v a="$RZ0" -v b="$RZ1" 'BEGIN { d = b - a; if (d < 0) d = -d; if (d > 180) d = 360 - d; printf "%d", d }')
[ "${TURNED:-0}" -ge 8 ] || verdict VOID "the friend's look does not answer a real mouse move in workshop mode (400 px turned it $TURNED degrees, $RZ0 to $RZ1; menus '$(menus vm)'): the rig's, no verdict"
rmark host; rmark vm
ws_top vm || verdict VOID "the friend's menu did not go back to its top ($(ws_path vm))"
ws_nav vm STRUCTURES WOOD FLOORS || verdict VOID "the friend's menu: $WS_NAV"
G=$(field "$(ws_state vm)" held)
[ -n "$G" ] && [ "$G" != 00000000 ] || verdict VOID "the friend holds no floor ghost ($WS_NAV)"
_late "the friend's build menu"
ws_green vm || verdict VOID "the friend's floor ghost $G never green ($(ws_state vm | grep -oE 'canPlace=[a-z]+ placeStatus=[0-9]+'); it stands at $(pos_of vm))"
rkey vm $VK_ENTER; sleep 6
ADOPTED=$(rsince vm | grep -oE "WorkshopSync: our $G \(base [0-9A-F]+\) is the host's piece now: adopted as our copy of player [0-9]+'s [0-9A-F]{8}" | tail -1)
ANSWER=$(rsince vm | grep -oE "WorkshopSync: request [0-9]+ \(build\) at workbench $BENCH: [^,;]*" | tail -1)
say "the friend's floor $G: ${ADOPTED:-NOT adopted as a piece of the host} (${ANSWER:-no answer line})"
[ -n "$ADOPTED" ] || verdict VOID "the friend's floor did not become the host's piece (the bug needs that): $(rsince vm | grep -E "WorkshopSync: (request|our $G)" | tail -3 | cut -c1-200 | paste -sd'|' -)"
HOSTREF=$(grep -oE "[0-9A-F]{8}$" <<<"$ADOPTED")
COPY=$(ws_copy_of vm "$HOSTREF"); COPY=${COPY:-$G}
HOST_AT="$(field "$(dc host "refinfo $HOSTREF")" x) $(field "$(dc host "refinfo $HOSTREF")" y)"
say "the host's floor is $HOSTREF at $HOST_AT; the friend's copy of it is $COPY ($(dc vm "wsinfo $COPY" | grep -oE 'blocked=[a-z]+'))"

# the friend looks at its floor (now the host's copy) until workshop mode highlights it, and presses Accept
_aimed() { [ "$(field "$(ws_state vm)" selected)" = "$COPY" ]; }
dc vm focus >/dev/null
for p in 0 25 35 15 45; do
	dc vm "turnto $COPY 300 aim" >/dev/null; sleep 0.8
	[ "$p" != 0 ] && { pitch_down vm "$p"; sleep 0.5; }
	_aimed && break
done
_aimed || verdict VOID "workshop mode never highlighted the friend's floor $COPY under the crosshair ($(ws_state vm | cut -c1-160))"
rmark vm k; rmark host k
HERE="$(field "$(dc vm "refinfo $COPY")" x) $(field "$(dc vm "refinfo $COPY")" y)"
rkey vm "$VK_ACCEPT"; sleep 1.5
HELD=$(field "$(ws_state vm)" held); HUD=$(rhud vm)
GRAB_RE="WorkshopSync: (the grab of our copy $COPY[^;]*|workshop mode.s grab of copy $COPY refused[^;]*)"
GRABLINE=$(rsince vm k | grep -oE "$GRAB_RE" | tail -1)
say "friend's Accept on its floor: held=$HELD; HUD '$HUD'; log: ${GRABLINE:-no grab line}"

RESULT=""; WHY=""
if [ "$HELD" != "$COPY" ]; then
	RESULT=YES; WHY="the friend's Accept on its own floor (adopted as the host's copy $COPY) picked nothing up; the game said '${HUD:-nothing}'; log: ${GRABLINE:-none}"
else
	RESULT=NO; WHY="the friend's Accept picks its own floor (the host's copy $COPY) up"
	# the move: the friend turns (real mouse moves) until the floor may be put down a step away, and Accept asks the host
	WHERE=""
	_carried() {
		NOW="$(field "$(dc vm "refinfo $COPY")" x) $(field "$(dc vm "refinfo $COPY")" y)"
		[ "$(field "$(ws_state vm)" canPlace)" = true ] && [ "${NOW%% *}" != "" ] && [ "$(dist_xy "$HERE" "$NOW")" -ge 60 ] 2>/dev/null
	}
	for i in 1 2 3 4 5 6; do dc vm "mouse 300 0" >/dev/null; sleep 1; _carried && { WHERE=$NOW; CARRY="real mouse turns"; break; }; done
	# no spot by turning: the friend steps back and sideways with its real movement keys (S, then A), the floor carried along
	[ -z "$WHERE" ] && for k in 0x53 0x53 0x41 0x41; do rkey vm $k 700; sleep 1.2; _carried && { WHERE=$NOW; CARRY="real steps (S, A)"; break; }; done
	if [ -z "$WHERE" ]; then
		rkey vm "$VK_CANCEL"; sleep 2
		WHY="$WHY; the carry found no spot the engine lets it be put down at (canPlace never true a step away: $(ws_state vm | grep -oE 'canPlace=[a-z]+ placeStatus=[0-9]+')), so it was let go with Cancel (held=$(field "$(ws_state vm)" held))"
	else
		rkey vm "$VK_ACCEPT"; sleep 2
		_asked() { rsince vm k | grep -qE "WorkshopSync: the move of our copy $COPY of player"; }
		ASKED=$(wait_for 10 _asked && echo yes || echo no)
		sleep 8
		MOVELINE=$(rsince vm k | grep -oE "WorkshopSync: the move of our copy $COPY of player [0-9]+'s [0-9A-F]{8} at workbench [0-9A-F]{8} from [^;]*asked of the host as request [0-9]+" | tail -1)
		ANSWERED=$(rsince vm k | grep -oE "WorkshopSync: request [0-9]+ \(move\) of our copy $COPY at workbench [0-9A-F]{8}: [^;]*" | tail -1 | cut -c1-200)
		HOST_NOW="$(field "$(dc host "refinfo $HOSTREF")" x) $(field "$(dc host "refinfo $HOSTREF")" y)"
		COPY_NOW="$(field "$(dc vm "refinfo $COPY")" x) $(field "$(dc vm "refinfo $COPY")" y)"
		say "the move: carried by $CARRY, put down at $WHERE (asked=$ASKED): ${MOVELINE:-no ask line}; answer: ${ANSWERED:-none}; host's floor $HOST_AT -> $HOST_NOW; friend's copy now $COPY_NOW; HUD '$(rhud vm)'"
		if [ -n "$MOVELINE" ] && grep -q ": applied" <<<"$ANSWERED" && [ "$(dist_xy "$HOST_NOW" "$COPY_NOW")" -le 5 ] 2>/dev/null && [ "$(dist_xy "$HOST_AT" "$HOST_NOW")" -ge 40 ] 2>/dev/null; then
			WHY="$WHY, and put down a step away it is moved: the host's game moved its floor $HOSTREF from $HOST_AT to $HOST_NOW (answer applied), where the friend's copy stands"
		else
			WHY="$WHY, but the move was NOT made: ${MOVELINE:-nothing asked}; answer '${ANSWERED:-none}'; host's floor $HOST_AT -> $HOST_NOW, copy $COPY_NOW"
		fi
	fi
fi
[ "$(field "$(ws_state vm)" held)" = "$COPY" ] && { rkey vm "$VK_CANCEL"; sleep 2; }
# the same class, the next key: the friend stores its floor (workshop mode's Store: Cancel on the highlighted piece, the box
# answered with Accept), which is asked of the host as the move is. Said, not part of the verdict. LAST=none leaves it out.
if [ "${LAST:-store}" = store ] && [ "$RESULT" = NO ]; then
	rmark vm s
	for p in 0 25 35 15 45; do
		dc vm "turnto $COPY 300 aim" >/dev/null; sleep 0.8
		[ "$p" != 0 ] && { pitch_down vm "$p"; sleep 0.5; }
		_aimed && break
	done
	if _aimed; then
		rkey vm "$VK_CANCEL"; sleep 1.5
		BOX=$(rmsgbox vm)
		if [ -n "$BOX" ]; then rkey vm "$VK_ACCEPT"; sleep 1; menu_open vm MessageBoxMenu && { rkey vm $VK_E; sleep 1; }; fi
		sleep 8
		STORED=$(rsince vm s | grep -oE "WorkshopSync: (the store of our copy $COPY [^;]*|request [0-9]+ \(store\)[^;]*)" | cut -c1-170 | paste -sd"|" -)
		say "the store of the same floor (real Cancel, box '${BOX:-none}'): ${STORED:-nothing asked}; HUD '$(rhud vm)'; the host has its floor: $(dc host "refinfo $HOSTREF" | cut -c1-80)"
		menu_open vm MessageBoxMenu && { rkey vm $VK_TAB; sleep 1; }
	else
		say "the store: workshop mode did not highlight the floor $COPY again ($(ws_state vm | cut -c1-120)); not tried"
	fi
fi
ws_leave vm; ws_leave host
rkeep host shared-build-move; rkeep vm shared-build-move
say "friend's log (grabs/moves): $(grep -E "WorkshopSync: (the grab of our copy|workshop mode's grab|the move of our copy|request [0-9]+ \(move\)|our copy $COPY let go)" "$REPRO_OUT/shared-build-move-vm.log" | tail -4 | cut -c1-220 | paste -sd'|' -)"
verdict "$RESULT" "$WHY"
