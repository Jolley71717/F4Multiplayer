#!/bin/bash
# workbench-transfer-closes.sh: a friend in workshop mode at the host's settlement opens the workbench's Transfer menu and it
# closes by itself (player report on 0.20.4, the friend's game on Linux/Proton: "I as the client am unable to transfer items from
# the red workbench; when I try, the inventory UI closes", so what they scrapped is stuck in it, and "can't share items in the
# workbench"). Set-up by console, said so: both games coc SanctuaryExt when not there; the host's Sanctuary made the host's
# (WorkshopScript.SetOwnedByPlayer, as the rig's other workshop scripts do); the host opens its workshop once with its real E and
# leaves, staying near. The friend walks to the workbench (real walk), presses E (real key: workshop mode at the host's settlement),
# looks up so nothing is highlighted and presses workshop mode's Transfer key (the XButton event, the game's own binding read from
# the friend's control map: R on a keyboard, which is Scrap on a highlighted piece and Transfer on none). The container menu is
# watched for WATCH seconds (default 8): whether it opened, and whether it is still open at the end with nobody touching it.
# The host's bSharedStorage is printed (the rules): run once with it off and once with it on (the host's ini, restarted).
# WAY picks which container menu of the class is tried: ws (default: workshop mode's Transfer key, as above), bench (the
# friend stands at the workbench outside workshop mode and presses R, the rollover's Transfer) or ref (REF=<container>: the
# friend walks to that container and presses E). The menu's own open and close counts (dev `menus`) are read before and after,
# so a menu closed in the frame it opened is seen too.
# REPRO: YES when the menu opened and closed by itself (how long it stayed, the HUD and the friend's Storage lines printed);
# NO when it stayed open (then closed with Cancel); VOID when the key opened no container menu. Both games joined first.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE
WATCH=${WATCH:-8}
WAY=${WAY:-ws}; VK_R=0x52
# BUDGET: the seconds the set-up may take before the run stops itself with VOID (a slow rig), so a turn stays short.
# HOST_OPENED=1: the host opened its workshop earlier in this session (a second WAY in one turn), so it is not opened again
BUDGET=${BUDGET:-420}; _late() { [ "$SECONDS" -le "$BUDGET" ] || verdict VOID "no verdict within $BUDGET s ($SECONDS s gone at: $1): the rig is too slow this turn"; }
_cm() { dc vm menus | grep -oE "ContainerMenu:[0-9]+/[0-9]+" | head -1 | cut -d: -f2; }   # opens/closes
need_session
say "workbench-transfer-closes: out $REPRO_OUT"
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) say "set-up: $w is not at Sanctuary ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
RULES=$(dc host rules)
SHARED=$(field "$RULES" sharedStorage)
say "host rules: sharedStorage=$SHARED sharedWorkshop=$(field "$RULES" sharedWorkshop) sharedWorkshopEdit=$(field "$RULES" sharedWorkshopEdit)"
if [ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
if [ -z "$HOST_OPENED" ] && ! _ws_in host; then
	ws_enter_real host $BENCH || verdict VOID "the host's real E at its own workbench opened no workshop mode ($ENTER_SAID)"
	say "the host opened its workshop with its real E ($ENTER_SAID)"
fi
ws_leave host; rwalk host -79000 89950 100   # out of the friend's way (north of the bench), near enough for its workbench to be shared
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
OPENED=""; CLOSED=""; T0=$SECONDS
for i in $(seq 1 $((WATCH * 4))); do
	if menu_open vm ContainerMenu; then [ -z "$OPENED" ] && OPENED=$((SECONDS - T0))
	elif [ -n "$OPENED" ]; then CLOSED=$((SECONDS - T0)); break; fi
	sleep 0.25
done
STILL=$(menus vm); HUD=$(rhud vm); C1=$(_cm); C1=${C1:-0/0}
# a menu closed between two looks: its own counts say it opened and closed
if [ -z "$OPENED" ] && [ "${C1%/*}" -gt "${C0%/*}" ]; then OPENED=0; [ "${C1#*/}" -gt "${C0#*/}" ] && CLOSED=0; fi
SLINES=$(rsince vm | grep -E "Storage:|ContainerMenu|Equipment: the container menu|MenuWatch" | tail -6 | cut -c1-220 | paste -sd'|' -)
say "after the key (way $WAY): container menu opens/closes $C0 -> $C1; opened=${OPENED:-never} closed=${CLOSED:-no}; menus now '$STILL'; HUD '$HUD'"
say "friend's log since: ${SLINES:-no Storage lines}"
say "host's log since: $(rsince host | grep -E "Storage:" | tail -3 | cut -c1-200 | paste -sd'|' -)"
if [ -z "$OPENED" ]; then
	RESULT=VOID; WHY="the key (way $WAY) opened no container menu for the friend (rollover '$(rrollover vm)'; menus '$STILL'; HUD '$HUD')"
elif [ -n "$CLOSED" ]; then
	RESULT=YES; WHY="the friend's container menu (way $WAY, sharedStorage=$SHARED) opened at ${OPENED}s and closed by itself at ${CLOSED}s; the game said '${HUD:-nothing}'; log: ${SLINES:-none}"
else
	RESULT=NO; WHY="the friend's container menu (way $WAY, sharedStorage=$SHARED) stayed open $WATCH s (then closed with Cancel)"
	rkey vm "$VK_CANCEL"; sleep 1.5
	menu_open vm ContainerMenu && { rkey vm "$VK_TAB"; sleep 1.5; }
fi
_ws_in vm && ws_leave vm; ws_leave host
rkeep host workbench-transfer-closes; rkeep vm workbench-transfer-closes
[ "$RESULT" = VOID ] && verdict VOID "$WHY"
verdict "$RESULT" "$WHY"
