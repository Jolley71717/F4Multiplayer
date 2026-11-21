#!/bin/bash
# friend-save-host-leftovers.sh: a friend's save made in a session keeps the friend's copies of the host's pieces at a settlement the
# host owns, and loaded alone, outside multiplayer, they stand there for good: out of workshop mode's reach (the block keyword is
# saved with each), linked to no workbench, never removed (player report, with Sim Settlements 2: the host's city planner's desk
# stood in the friend's own game, where nobody could use it or scrap it). The maintainer's rule: a friend's save keeps nothing of
# the host's settlement. The vanilla equivalent, as a player does it:
#   1. Both games at Sanctuary (console coc SanctuaryExt when not, said), joined. Sanctuary is the host's (console
#      WorkshopScript.SetOwnedByPlayer, as the rig's checks do; a coc character owns no settlement), and the friend's game does not
#      own it (a fresh coc character). The host opens its workshop with its real E (its workshop data made, as a real host's is).
#   2. The host places a generator and a bed (console placement, wsreport place + wsfinish: the engine's own finish, said) and
#      makes Codsworth Sanctuary's settler and gives him the bed (console AddActorToWorkshopPUBLIC and AssignActorToObjectPUBLIC,
#      said). The friend's game gets its copies.
#   3. What the friend's game holds of Sanctuary before (the workbench's owner, Codsworth's workshop) is noted.
#   4. The friend saves (console save, a throwaway save named below), leaves the session (net leave) and loads that save alone.
#   5. 30 s on (the cell loaded, the copies' 3D in), each copy is looked up under its id in the friend's game: still there (not
#      deleted, the same base, blocked) or gone. Its scripts' workshopID is printed (the copy's scripts ran in the friend's game).
#      The workbench's owner and Codsworth's workshop are compared with step 3.
#   6. (CLAIM=1, the default) The friend claims Sanctuary in their own game (console SetOwnedByPlayer in the VM's game stands in for
#      the normal claim), presses E (real key) and builds a wood floor through the real menu: printed whether workshop mode opened
#      and the floor was built (their own workbench works where they own the settlement).
# REPRO: YES when any copy of the host's pieces still stands in the friend's game loaded alone; NO when every one is gone and
# nothing else of the host's (the workbench's owner, Codsworth's workshop) changed in the friend's save.
# The throwaway save ($LEFTOVER_SAVE) stays on the VM: the rig turn moves it aside after (never deleted).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE; CODSWORTH=0001CA7D; GENERATOR=00030386; BED=00118F57
LEFTOVER_SAVE=${LEFTOVER_SAVE:-f4mp_repro_friend_leftovers}
need_session
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) say "set-up: $w is not at Sanctuary ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
r=$(dc host rules)
[ "$(field "$r" sharedWorkshop)" = true ] || verdict VOID "the host's bSharedWorkshop is off"
owned() { field "$(dc "$1" "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer; }
[ "$(owned vm)" = true ] && verdict VOID "the friend's game already owns Sanctuary (an earlier run's claim): restart the VM's game (a fresh coc character) and rejoin"
if [ "$(owned host)" != true ]; then
	say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"
	dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
fi
[ "$(owned host)" = true ] || verdict VOID "the host's Sanctuary is not the host's after SetOwnedByPlayer"
if ! _ws_in host; then
	ws_enter_real host $BENCH || verdict VOID "the host's real E at its own workbench opened no workshop mode ($ENTER_SAID)"
	say "the host opened its workshop with its real E ($ENTER_SAID)"
fi
ws_leave host

# 2. the host's pieces and settler; the friend's copies
place() {
	local out ref; out=$(dc host "wsreport place $1 $BENCH"); ref=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1)
	[ -n "$ref" ] || return 1
	sleep 2; dc host "wsfinish $ref $BENCH" >/dev/null; sleep 2; echo "$ref"
}
say "set-up: the host places a generator and a bed (console placement: wsreport place + wsfinish)"
HG=$(place $GENERATOR) || verdict VOID "the host's generator was not placed"
HB=$(place $BED) || verdict VOID "the host's bed was not placed"
echo "$HG" > "$REPRO_OUT/host-placed.txt"; echo "$HB" >> "$REPRO_OUT/host-placed.txt"
codsworth_ws() { field "$(dc "$1" "scriptvars $CODSWORTH WorkshopNPCScript workshopID")" workshopID; }
V_CODS0=$(codsworth_ws vm)
say "set-up: Codsworth made Sanctuary's settler in the host's game and given the host's bed (console cqf WorkshopParent AddActorToWorkshopPUBLIC, AssignActorToObjectPUBLIC)"
dc host "console cqf WorkshopParent \"WorkshopParentScript.AddActorToWorkshopPUBLIC\" $CODSWORTH $BENCH" >/dev/null; sleep 4
dc host "console cqf WorkshopParent \"WorkshopParentScript.AssignActorToObjectPUBLIC\" $CODSWORTH $HB" >/dev/null; sleep 4
say "host: generator $HG, bed $HB (owner $(dc host "wsinfo $HB" | grep -oE ' owner=[0-9A-F]+' | cut -d= -f2)); Codsworth's workshopID $(codsworth_ws host)"
has_copies() { [ -n "$(ws_copy_of vm "$HG")" ] && [ -n "$(ws_copy_of vm "$HB")" ]; }
wait_for 20 has_copies || verdict VOID "the friend's game made no copy of the host's generator and bed ($(dc vm 'wspower copies' | cut -c1-200))"
VG=$(ws_copy_of vm "$HG"); VB=$(ws_copy_of vm "$HB")
sleep 5
info() { local w; w=$(dc vm "wsinfo $1"); echo "base=$(field "$w" base) blocked=$(field "$w" blocked) deleted=$(field "$w" deleted) workshopID=$(field "$(dc vm "scriptvars $1 WorkshopObjectScript workshopID")" workshopID)"; }
say "the friend's copies: generator $VG ($(info "$VG")), bed $VB ($(info "$VB"))"
V_OWN0=$(owned vm)
say "the friend's game before its save: Sanctuary OwnedByPlayer=$V_OWN0, Codsworth's workshopID=${V_CODS0:-none}"

# 4. the friend saves in the session, leaves, and loads the save alone
rmark vm save
say "the friend saves (console save $LEFTOVER_SAVE), leaves (net leave) and loads that save alone (console load)"
dc vm "console save $LEFTOVER_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
dc vm "net leave" >/dev/null; sleep 4
dc vm "console load $LEFTOVER_SAVE" >/dev/null; sleep 20; wait_ingame vm 60 || verdict VOID "the friend's game did not come back from the load"
sleep 10
say "the friend's game loaded alone: $(dc vm 'net status' | grep -oE 'status=[a-z]+' | head -1), at $(dc vm status | grep -oE "location=[0-9A-F]+ '[^']*'")"

# 5. what the friend's game holds now
LEFT=""
look() {  # look <copy> <host's ref> <name>
	local ri; ri=$(dc vm "refinfo $1")
	case "$ri" in error*|"") say "loaded alone: the friend's copy of the host's $3 ($1) is gone (no reference under its id)"; return;; esac
	if [ "$(field "$ri" deleted)" = true ] || [ "$(field "$ri" disabled)" = true ]; then say "loaded alone: the friend's copy of the host's $3 ($1) is gone (deleted=$(field "$ri" deleted) disabled=$(field "$ri" disabled))"; return; fi
	LEFT="$LEFT $1"
	say "loaded alone: the friend's copy of the host's $3 ($1) STILL STANDS: $(info "$1"), at ($(field "$ri" x), $(field "$ri" y))"
}
look "$VG" "$HG" generator
look "$VB" "$HB" bed
V_OWN1=$(owned vm); V_CODS1=$(codsworth_ws vm)
say "the friend's game loaded alone: Sanctuary OwnedByPlayer=$V_OWN1 (before $V_OWN0), Codsworth's workshopID=${V_CODS1:-none} (before ${V_CODS0:-none})"
say "the friend's saved-copies counters: $(dc vm wspower | grep -oE 'savedCopies=.*' | cut -c1-160)"
rsince vm save > "$REPRO_OUT/friend-after-save.log"
grep -E "a save held|record names|could not be read" "$REPRO_OUT/friend-after-save.log" | head -6 | while read -r l; do say "friend: ${l:0:240}"; done
OTHER=""
[ "$V_OWN1" != "$V_OWN0" ] && OTHER="$OTHER Sanctuary's owner changed ($V_OWN0 -> $V_OWN1);"
[ "$V_CODS1" != "$V_CODS0" ] && OTHER="$OTHER Codsworth's workshop changed (${V_CODS0:-none} -> ${V_CODS1:-none});"

# 6. the friend's own workbench where they own the settlement
if [ "${CLAIM:-1}" = 1 ]; then
	say "claim: the friend claims Sanctuary in their own game (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1 stands in for the normal claim)"
	dc vm "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
	rmark vm claim
	if ws_enter_real vm $BENCH; then
		say "claim: the friend's real E opened workshop mode at their own Sanctuary ($ENTER_SAID)"
		rwalk vm -78900 89950 80
		ws_top vm; ws_nav vm STRUCTURES WOOD FLOORS
		g=$(field "$(ws_state vm)" held)
		if [ -n "$g" ] && [ "$g" != 00000000 ] && ws_green vm && rkey vm $VK_ENTER && sleep 4; then
			say "claim: the friend built a wood floor through the real menu ($g: $(dc vm "wsinfo $g" | cut -c1-120))"
		else say "claim: the friend's floor was not put down ($(ws_state vm | cut -c1-160))"; fi
		ws_leave vm
	else say "claim: the friend's real E opened no workshop mode ($ENTER_SAID)"; fi
fi
rkeep vm friend-save-host-leftovers
[ -n "$LEFT" ] && verdict YES "the friend's save, loaded alone, still holds copies of the host's pieces:$LEFT${OTHER:+ (also:$OTHER)}"
[ -n "$OTHER" ] && verdict YES "no copy is left, but the friend's save changed:$OTHER"
verdict NO "loaded alone, the friend's save holds no copy of the host's pieces and nothing else of the host's changed"
