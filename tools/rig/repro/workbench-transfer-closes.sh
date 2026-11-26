#!/bin/bash
# workbench-transfer-closes.sh: a friend in workshop mode at the host's settlement opens the workbench's Transfer menu and it
# closes, by itself or when they take an item (player report on 0.20.4, the friend's game on Linux/Proton: "I as the client am
# unable to transfer items from the red workbench; when I try, the inventory UI closes", so what they scrapped is stuck in
# it, and "can't share items in the
# workbench"). Set-up by console, said so: both games coc SanctuaryExt when not there (PLACE=rr: Red Rocket, the report's
# settlement, by player.moveto to its bench); the bench stocked (additem); the host's settlement made the host's
# (WorkshopScript.SetOwnedByPlayer, as the rig's other workshop scripts do); the host opens its workshop once with its real E and
# leaves, staying near. The friend walks to the workbench (real walk), presses E (real key: workshop mode at the host's settlement),
# looks up so nothing is highlighted and presses workshop mode's Transfer key (the XButton event, the game's own binding read from
# the friend's control map: R on a keyboard, which is Scrap on a highlighted piece and Transfer on none). The container menu is
# watched for WATCH seconds (default 8): whether it opened, and whether it is still open at the end with nobody touching it.
# Then the friend takes an item from it with a real E (TAKE below) and the menu is watched again.
# The host's bSharedStorage is printed (the rules): run once with it off and once with it on (the host's ini, restarted).
# WAY picks which container menu of the class is tried: ws (default: workshop mode's Transfer key, as above), bench (the
# friend stands at the workbench outside workshop mode and presses R, the rollover's Transfer) or ref (REF=<container>: the
# friend walks to that container and presses E). The menu's own open and close counts (dev `menus`) are read before and after,
# so a menu closed in the frame it opened is seen too.
# REPRO: YES when the menu opened and closed by itself, untouched or at the take (when, the HUD and the friend's Storage lines
# printed); NO when it stayed open through the take (the counts say whether the item moved; then closed with Cancel);
# VOID when the key opened no container menu. Both games joined first.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
# PLACE: sanctuary (default) or rr (Red Rocket, the report's settlement: both brought there by console player.moveto, said)
PLACE=${PLACE:-sanctuary}
case "$PLACE" in rr) BENCH=00054BAE; WHERE="Red Rocket";; *) BENCH=000250FE; WHERE="Sanctuary";; esac
WATCH=${WATCH:-8}
WAY=${WAY:-ws}; VK_R=0x52
# The take: the bench is stocked first (console additem, said: ITEM, steel, STOCK of them in the host's bench, and in the friend's
# own copy of it too when the host's shared storage is off, where each game keeps its own bench). When the menu stays open
# untouched, the friend takes with real keys: TAKE=one (default; E: the highlighted row), all (R: Take All), both (E, then R) or
# none. Only E is the default: whether a game alone keeps the menu open after Take All is not known here. The menu is watched 4 s after each key; the friend's count of ITEM and both benches' counts are printed before and after.
ITEM=${ITEM:-000731A4}; STOCK=${STOCK:-3}; TAKE=${TAKE:-one}
_n() { dc "$1" "count $2 $ITEM" | grep -oE '^[0-9]+' | head -1; }
# BUDGET: the seconds the set-up may take before the run stops itself with VOID (a slow rig), so a turn stays short.
# HOST_OPENED=1: the host opened its workshop earlier in this session (a second WAY in one turn), so it is not opened again
BUDGET=${BUDGET:-420}; _late() { [ "$SECONDS" -le "$BUDGET" ] || verdict VOID "no verdict within $BUDGET s ($SECONDS s gone at: $1): the rig is too slow this turn"; }
_cm() { dc vm menus | grep -oE "ContainerMenu:[0-9]+/[0-9]+" | head -1 | cut -d: -f2; }   # opens/closes
# Red Rocket: the players are put at the bench by console, so the walk to it is the last few steps (lib.sh's routes are Sanctuary's)
[ "$PLACE" = rr ] && rwalk_bench() {
	local r; r=$(dc "$1" "refinfo $2"); rwalk "$1" "$2" 95 0.6
	BENCH_DIST=$(dist_xy "$(pos_of "$1")" "$(field "$r" x | cut -d. -f1) $(field "$r" y | cut -d. -f1)"); [ "${BENCH_DIST:-9999}" -le 160 ]
}
need_session
say "workbench-transfer-closes: out $REPRO_OUT"
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"$WHERE"*) ;; *)
		if [ "$PLACE" = rr ]; then say "set-up: $w is not at $WHERE ($loc): console player.moveto $BENCH"; dc $w focus >/dev/null; dc $w "console player.moveto $BENCH" >/dev/null
		else say "set-up: $w is not at $WHERE ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; fi
		sleep 6; wait_ingame $w 60;; esac
done
RULES=$(dc host rules)
SHARED=$(field "$RULES" sharedStorage)
say "host rules: sharedStorage=$SHARED sharedWorkshop=$(field "$RULES" sharedWorkshop) sharedWorkshopEdit=$(field "$RULES" sharedWorkshopEdit)"
if [ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's $WHERE made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
if [ -z "$HOST_OPENED" ] && ! _ws_in host; then
	ws_enter_real host $BENCH || verdict VOID "the host's real E at its own workbench opened no workshop mode ($ENTER_SAID)"
	say "the host opened its workshop with its real E ($ENTER_SAID)"
fi
read -r BX BY <<<"$(field "$(dc host "refinfo $BENCH")" x) $(field "$(dc host "refinfo $BENCH")" y)"
# out of the friend's way, near enough for its workbench to be shared
ws_leave host; if [ "$PLACE" = rr ]; then rwalk host $(( ${BX%.*} - 500 )) $(( ${BY%.*} - 300 )) 100; else rwalk host -79000 89950 100; fi
if [ "$TAKE" != none ]; then
	dc host "console $BENCH.additem $ITEM $STOCK" >/dev/null
	[ "$SHARED" = true ] || dc vm "console $BENCH.additem $ITEM $STOCK" >/dev/null
	sleep 1.5
	say "set-up: $STOCK of $ITEM added to the host's bench by console$([ "$SHARED" = true ] || echo " and to the friend's own copy of it (shared storage is off)"): the host's bench holds $(_n host $BENCH), the friend's game's $(_n vm $BENCH)"
fi
_late "the host's set-up"
sleep 3
say "host's storage: $(dc host storage | cut -c1-200)"
KEYS=$(dc vm "controls workshop")
VK_X=$(grep -oE "(^| )XButton=[0-9A-F]+" <<<"$KEYS" | head -1 | sed 's/.*=/0x/'); VK_CANCEL=$(grep -oE "(^| )Cancel=[0-9A-F]+" <<<"$KEYS" | head -1 | sed 's/.*=/0x/')
[ -n "$VK_X" ] && [ -n "$VK_CANCEL" ] || verdict VOID "workshop mode's XButton (Transfer) or Cancel key is not in the friend's control map ($(cut -c1-200 <<<"$KEYS"))"
say "friend's workshop keys: Transfer/Scrap (XButton)=$VK_X Cancel=$VK_CANCEL"

_ws_in vm && ws_leave vm
C0=$(_cm); C0=${C0:-0/0}
if [ "$WAY" = ws ]; then
	ws_enter_real vm $BENCH || verdict VOID "the friend's real E at the host's workbench opened no workshop mode ($ENTER_SAID; rollover '$ROLLOVER')"
	say "the friend is in workshop mode at the host's workbench ($ENTER_SAID); friend's storage: $(dc vm storage | cut -c1-240)"
	sleep 3
	rmark host; rmark vm
	ws_top vm || verdict VOID "the friend's menu did not go back to its top ($(ws_path vm))"
	# nothing highlighted (the look is up at the sky after ws_top): the XButton is then Transfer, not Scrap
	[ "$(field "$(ws_state vm)" selected)" = 00000000 ] || { dc vm "mouse 0 -1500" >/dev/null; sleep 0.8; }
	SEL=$(field "$(ws_state vm)" selected)
	[ "$SEL" = 00000000 ] || verdict VOID "the friend's crosshair still highlights $SEL: the key would scrap it, not open Transfer"
	BEFORE=$(menus vm)
	menu_open vm ContainerMenu && verdict VOID "a container menu is open already ($BEFORE)"
	rkey vm "$VK_X"
elif [ "$WAY" = bench ]; then
	rwalk_bench vm $BENCH || verdict VOID "the friend did not reach the workbench ($BENCH_DIST u)"
	rmark host; rmark vm
	dc vm "turnto $BENCH 300 aim" >/dev/null; sleep 0.8
	say "the friend at the workbench, outside workshop mode: rollover '$(rrollover vm)'; presses R (Transfer)"
	rkey vm $VK_R 100
else
	[ -n "$REF" ] || verdict VOID "WAY=ref needs REF=<container>"
	rwalk vm "$REF" 110 0.6
	rmark host; rmark vm
	rpress_e vm "$REF"
	say "the friend at the container $REF: rollover '$ROLLOVER'; pressed E"
fi
# _watch <seconds>: 0 when the container menu is open at their end; OPENED and CLOSED are seconds since the key
_watch() {
	local i
	for i in $(seq 1 $(($1 * 4))); do
		if menu_open vm ContainerMenu; then [ -z "$OPENED" ] && OPENED=$((SECONDS - T0))
		elif [ -n "$OPENED" ]; then CLOSED=$((SECONDS - T0)); return 1; fi
		sleep 0.25
	done
	[ -n "$OPENED" ]
}
OPENED=""; CLOSED=""; WHEN="untouched"; T0=$SECONDS
_watch "$WATCH"
STILL=$(menus vm); HUD=$(rhud vm); C1=$(_cm); C1=${C1:-0/0}
# a menu closed between two looks: its own counts say it opened and closed
if [ -z "$OPENED" ] && [ "${C1%/*}" -gt "${C0%/*}" ]; then OPENED=0; [ "${C1#*/}" -gt "${C0#*/}" ] && CLOSED=0; fi
SLINES=$(rsince vm | grep -E "Storage:|ContainerMenu|Equipment: the container menu|MenuWatch" | tail -6 | cut -c1-220 | paste -sd'|' -)
say "after the key (way $WAY): container menu opens/closes $C0 -> $C1; opened=${OPENED:-never} closed=${CLOSED:-no}; menus now '$STILL'; HUD '$HUD'"
say "friend's log since: ${SLINES:-no Storage lines}"
say "host's log since: $(rsince host | grep -E "Storage:" | tail -3 | cut -c1-200 | paste -sd'|' -)"
# the take: real keys in the open menu
if [ -n "$OPENED" ] && [ -z "$CLOSED" ] && [ "$TAKE" != none ]; then
	P0=$(_n vm 00000014); H0=$(_n host $BENCH); V0=$(_n vm $BENCH)
	if [ "$TAKE" != all ]; then WHEN="after E (take the highlighted row)"; rkey vm "$VK_E" 100; _watch 4; fi
	if [ -z "$CLOSED" ] && [ "$TAKE" != one ]; then WHEN="after R (Take All)"; rkey vm $VK_R 100; _watch 4; fi
	[ -n "$CLOSED" ] || WHEN="through the take ($TAKE)"
	sleep 1; HUD=$(rhud vm)
	SLINES=$(rsince vm | grep -E "Storage:|ContainerMenu|Equipment: the container menu|MenuWatch|Workshop: .*(left|ended|closed)" | tail -8 | cut -c1-220 | paste -sd'|' -)
	TOOK="the friend's $ITEM $P0 -> $(_n vm 00000014); the host's bench $H0 -> $(_n host $BENCH); the friend's game's bench $V0 -> $(_n vm $BENCH)"
	say "the take ($TAKE): menu $([ -n "$CLOSED" ] && echo "closed $WHEN" || echo "still open"); menus '$(menus vm)'; HUD '$HUD'; $TOOK"
	say "friend's log since: ${SLINES:-no Storage lines}"
	say "host's log since: $(rsince host | grep -E "Storage:" | tail -4 | cut -c1-200 | paste -sd'|' -)"
fi
if [ -z "$OPENED" ]; then
	RESULT=VOID; WHY="the key (way $WAY) opened no container menu for the friend (rollover '$(rrollover vm)'; menus '$STILL'; HUD '$HUD')"
elif [ -n "$CLOSED" ]; then
	RESULT=YES; WHY="the friend's container menu (way $WAY, sharedStorage=$SHARED) opened at ${OPENED}s and closed by itself at ${CLOSED}s ($WHEN); ${TOOK:-no take tried}; the game said '${HUD:-nothing}'; log: ${SLINES:-none}"
else
	RESULT=NO; WHY="the friend's container menu (way $WAY, sharedStorage=$SHARED) stayed open $WATCH s untouched and $WHEN; ${TOOK:-no take tried} (then closed with Cancel)"
	rkey vm "$VK_CANCEL"; sleep 1.5
	menu_open vm ContainerMenu && { rkey vm "$VK_TAB"; sleep 1.5; }
fi
_ws_in vm && ws_leave vm; ws_leave host
rkeep host workbench-transfer-closes; rkeep vm workbench-transfer-closes
[ "$RESULT" = VOID ] && verdict VOID "$WHY"
verdict "$RESULT" "$WHY"
