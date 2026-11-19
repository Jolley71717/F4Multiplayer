#!/bin/bash
# workbench-not-owned.sh: a friend at a settlement the host owns, which the friend's own game does not, presses E on the workbench
# (player report: refused as "not allied" / not part of the settlement, so they cannot build with the host). The host's Sanctuary
# is made the host's by console (WorkshopScript.SetOwnedByPlayer, as the rig's bench and workshop-together checks do; a coc
# character owns no settlement), the host opens its workshop once with its real E (a real host has: the engine makes a
# workbench's workshop data on its first opening), and the friend's game must NOT own Sanctuary (a fresh coc character: restart
# the VM's game when an earlier run made it the friend's). The friend walks to the workbench (real walk) and presses E (real key).
# REPRO: YES when no workshop mode opens for the friend (the HUD's refusal is printed, and whether the shared workshop ever
# started in the friend's log); NO when the friend's E opens workshop mode at the host's settlement.
# Then (WORKAROUND=1, the default) the workaround a player would be told: the friend claims the settlement in their own game
# (here console SetOwnedByPlayer in the VM's game stands in for the normal claim), presses E again and builds a wood floor through
# the real menu; printed: whether workshop mode opened and whether the host's game built the piece as the host's.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE
need_session
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) say "set-up: $w is not at Sanctuary ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
[ "$(field "$(dc host rules)" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off"
if [ "$(field "$(dc vm "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" = true ]; then
	verdict VOID "the friend's game already owns Sanctuary (an earlier run's claim): restart the VM's game (a fresh coc character) and rejoin"
fi
if [ "$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
say "ownership: host OwnedByPlayer=$(field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer), friend's game OwnedByPlayer=$(field "$(dc vm "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer)"
if ! _ws_in host; then
	ws_enter_real host $BENCH || verdict VOID "the host's real E at its own workbench opened no workshop mode ($ENTER_SAID)"
	say "the host opened its workshop with its real E ($ENTER_SAID)"
fi
ws_leave host; rwalk host -79000 89950 100   # out of the friend's way (north of the bench)
rmark host; rmark vm
_ws_in vm && ws_leave vm
rwalk_bench vm $BENCH || verdict VOID "the friend did not get within reach of the workbench (${BENCH_DIST} u off: $WALKED)"
say "friend at the workbench: $(dist_xy "$(pos_of vm)" "$(field "$(dc vm "refinfo $BENCH")" x) $(field "$(dc vm "refinfo $BENCH")" y)") u"
IN=no; SAID=""
for try in 1 2 3; do
	rpress_e vm $BENCH; sleep 0.6
	SAID=$(rhud vm)
	say "friend's E (try $try): crosshair on '$ROLLOVER'; HUD now '$SAID'"
	if wait_for 6 _ws_in vm; then IN=yes; break; fi
	[ -n "$ROLLOVER" ] && break   # the bench was under the crosshair and the E was refused: no need to aim again
	dc vm "mouse 0 200" >/dev/null; sleep 0.5
done
sleep 1
V=$(rsince vm); H=$(rsince host)
started=$(grep -cE "Storage: viewing the host's workbench $BENCH|workshop mode at the host's settlement $BENCH" <<<"$V")
say "friend's log since: $(grep -E "$BENCH|WorkshopSync|Storage" <<<"$V" | grep -v 'Pad: \|gfx: ' | tail -4 | cut -c1-200 | paste -sd'|' -)"
say "the shared workshop started for the friend: $([ "$started" -gt 0 ] && echo yes || echo no) ($started lines)"
if [ "$IN" = yes ]; then RESULT="NO"; WHY="the friend's E opened workshop mode at the host's settlement (the friend's game owns it: $(field "$(dc vm "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer))"
else RESULT="YES"; WHY="the friend's E at the host's workbench opened no workshop mode; the game said '${SAID:-nothing on the HUD}'"; fi
rkeep host workbench-not-owned; rkeep vm workbench-not-owned

if [ "${WORKAROUND:-1}" = 1 ] && [ "$IN" = no ]; then
	say "workaround: the friend claims Sanctuary in their own game (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1 in the VM's game stands in for the normal claim)"
	dc vm "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
	rmark host w; rmark vm w
	if ws_enter_real vm $BENCH; then
		say "workaround: the friend's E opened workshop mode ($ENTER_SAID)"
		rwalk vm -78900 89950 80   # open ground north of the bench, where a floor fits (a real walk, in workshop mode)
		ws_top vm; ws_nav vm STRUCTURES WOOD FLOORS
		g=$(field "$(ws_state vm)" held)
		if [ -n "$g" ] && [ "$g" != 00000000 ] && ws_green vm && rkey vm $VK_ENTER && sleep 6; then
			a=$(rsince vm w | grep -oE "WorkshopSync: our $g \(base [0-9A-F]+\) is the host's piece now: adopted as our copy of player [0-9]+'s [0-9A-F]{8}" | tail -1)
			r=$(rsince vm w | grep -oE "WorkshopSync: request [0-9]+ \(build\) at workbench $BENCH: [^,;]*" | tail -1)
			say "workaround: the friend's floor $g: ${a:-not adopted as a piece of the host} (${r:-no answer line})"
		else say "workaround: the friend's floor was not put down ($(ws_state vm | cut -c1-160))"; fi
		ws_leave vm
	else say "workaround: the friend's E still opened no workshop mode ($ENTER_SAID)"; fi
	rkeep host workbench-not-owned-workaround; rkeep vm workbench-not-owned-workaround
fi
verdict "$RESULT" "$WHY"
