#!/bin/bash
# mansion-doors.sh: an invisible wall stops the host in a doorway the friend walks through. Player report (0.21.2, Nuka-World's
# Grandchester Mystery Mansion): the doors look open in both games, the friend goes through, the host is stopped in the doorway by
# nothing to be seen. The quest (DLC04Grandchester) is each game's own (sQuests = story): its stage 13 unlocks and opens the dining
# room's door and disables a CollisionMarker in its doorway (DLC04_FireplaceRoomCollisionRef). The door's state is shared (DoorShare),
# the marker's was not.
# Set-up, said so where it happens: both games to the mansion by console coc; both players placed 130 units from the doorway by
# console setpos; the friend's stage 13 set by console (the quest's own scene sets it when its dialogue ends, in the friend's game
# only). Movement is real: each player walks (the pad's stick) through the doorway, east.
# REPRO: YES when the friend walks through and the host is stopped in the doorway; NO when both walk through; VOID when the
# scene could not be made (the baseline: before the stage, both must be stopped by the wall).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
run_cap 900
need_session
CELL=${CELL:-DLC04GrandchesterMansion01}
say "set-up: both games to $CELL by console coc"
enter_both "$CELL" 16
PFX=$(cell_of host); PFX=${PFX:0:2}
WALL=${PFX}050556; DOOR=${PFX}02FA11; QUEST=${PFX}000808
say "refs: wall $WALL, door $DOOR, quest $QUEST (the load order's byte $PFX)"
say "set-up: both players placed west of the doorway by console setpos (not movement)"
dc host "setpos 14 2540 3616 262" >/dev/null; dc vm "setpos 14 2540 3590 262" >/dev/null; sleep 4

through() {  # <who>: walks east through the doorway; 0 = past it (x > 2720)
	local x
	bash "$RIG/walkto.sh" "$1" 2840 3616 0.6 40 25 >/dev/null 2>&1
	x=$(pos_of "$1" | cut -d' ' -f1); x=${x%.*}
	say "$1 ended at x=$x (the wall is at 2680)"
	is_int "$x" && [ "$x" -gt 2720 ]
}
state() { say "$1: wall $(dc "$1" "refinfo $WALL" | grep -oE 'disabled=[a-z]+'), door $(dc "$1" "openstate $DOOR" | grep -oE 'open=[a-z]+') $(dc "$1" "refinfo $DOOR" | grep -oE 'locked=[a-z]+')"; }

state host; state vm
say "baseline: before the friend's stage both are stopped by the wall"
through host && verdict VOID "the host walked through before any stage (the wall was down already)"
through vm && verdict VOID "the friend walked through before any stage (the wall was down already)"
dc host "setpos 14 2540 3616 262" >/dev/null; dc vm "setpos 14 2540 3590 262" >/dev/null; sleep 3

say "the friend's stage 13 by console (set-up): the door unlocked and open, its wall disabled, in the friend's game"
dc vm "console setstage ${QUEST} 13" >/dev/null
sleep 6
state host; state vm
rlog host 400 | grep -E "invisible wall|$DOOR|$WALL" | tail -5 | sed 's/^/host log: /'

through vm; FRIEND_THROUGH=$?
through host; HOST_THROUGH=$?
state host; state vm
if [ "$FRIEND_THROUGH" = 0 ] && [ "$HOST_THROUGH" != 0 ]; then verdict YES "the friend walked through the doorway, the host was stopped in it (the wall: $(dc host "refinfo $WALL" | grep -oE 'disabled=[a-z]+'))"; fi
if [ "$FRIEND_THROUGH" = 0 ] && [ "$HOST_THROUGH" = 0 ]; then verdict NO "both walked through the doorway"; fi
verdict VOID "the friend did not walk through either (their stage did not take the wall down)"
