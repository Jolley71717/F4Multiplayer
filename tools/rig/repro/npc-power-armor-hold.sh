#!/bin/bash
# npc-power-armor-hold.sh: the friend's game shows the host's Piper in a frame copy (npc-power-armor.sh's class: any mirrored NPC in
# a suit), and the host's copy of her goes quiet or away while she is in it. A mirror in its copy whose owner's copy is silent is
# held there, still mirrored, for up to 10 s of silence (MirrorFrames::HoldWhileSilent: a death said then leaves the body in its
# suit, npc-power-armor-death.sh); each other way that ends, and an owner's copy that went through a door, must take her out with
# the engine's exit and leave nothing: no frame in the friend's save with a living NPC in it, no NPC in power armor without one.
#   1. Both games where they are (rig.sh join CELL), joined. Piper enabled, brought to the host's player (console moveto), made the
#      host's companion (console SetCompanion), and put in a fresh frame in the host's game (console placeatme +
#      Actor.SwitchToPowerArmor), all said: the friend's Piper in a frame copy.
#   2. MODE:
#      runout: the host's game stalls STALL s (14; the dev channel's stall: its main thread held, as a hang, so no NPC states
#              while the connection stands): the friend's Piper held at 3 s, the hold over at 10 s, taken out as a mirror.
#      leave:  the host's game stalls 8 s, and 5 s in (the hold begun) the friend leaves the session (net leave).
#      door:   the host walks to the door nearest to them (the pad) and opens it (E): the host's Piper follows through it; the
#              friend's stays (its owner's copy elsewhere, perhaps silent over the host's load first).
#      unload: the friend walks to the door nearest to them (the pad) and opens it (E): Piper's cell unloads in the friend's game
#              while she is in her copy there (her owner's copy still sending).
#      save:   the host's game stalls 12 s, and once the hold has begun the friend saves (console save) during it.
#   3. 10 s on: both games answer, no new crash log; the friend's log has her out of the copy ("out of frame copy <copy>") and
#      never "stays in frame copy <copy>" (left to the game alive), and that copy is gone.
#   4. The friend saves (console save, a throwaway save named below), leaves the session when still in it (net leave) and loads
#      the save alone (console load): Piper not in power armor, no frame near the friend's player.
# REPRO: YES when a game crashes or stops answering, the friend's Piper stays in the copy or the copy is left to the game, or
# the save loaded alone has her in power armor or a frame near. NO when she comes out and nothing is left. VOID when the scene
# could not be made (or MODE=leave's hold never began, MODE=runout's stall never silenced her).
# The throwaway save ($PA_SAVE) stays on the VM: the rig turn moves it aside after (never deleted).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
PIPER=00002F1F; FRAME_BASE=0004A9C1
MODE=${MODE:-runout}; STALL=${STALL:-14}
PA_SAVE=${PA_SAVE:-f4mp_repro_npc_power_armor_hold_$MODE}
need_session
CH0=$(crash_list_host); CV0=$(crash_list_vm)
crashed() {
	local nh nv w
	nh=$(crash_new "$CH0" "$(crash_list_host)"); nv=$(crash_new "$CV0" "$(crash_list_vm)")
	CRASH="${nh:+host crash log: $nh; }${nv:+vm crash log: $nv; }"
	for w in host vm; do dc "$w" status | grep -q 'ingame=' || CRASH="$CRASH$w does not answer ($(dc "$w" status | cut -c1-80)); "; done
	[ -n "$CRASH" ]
}
alive_or_yes() { crashed || return 0; rkeep vm npc-power-armor-hold; rkeep host npc-power-armor-hold; verdict YES "$MODE, $1: $CRASH"; }
npcpa() { dc "$1" "npcpa $PIPER"; }
in_pa() { [ "$(field "$(npcpa "$1")" inPA)" = true ]; }
joined() { dc vm 'net status' | grep -q 'status=joined'; }
mirrored() { [ "$(field "$(dc vm "mirror $PIPER")" mirror)" = true ] && [ "$(field "$(dc vm "mirror $PIPER")" loaded)" = true ]; }
owned_here() { [ "$(field "$(dc host "mirror $PIPER")" owned)" = true ]; }
friend_in_copy() { local p; p=$(npcpa vm); [ "$(field "$p" inPA)" = true ] && [ "$(field "$p" ourCopy)" = true ]; }
copy_gone() { local ri; ri=$(dc vm "refinfo $1"); case "$ri" in error*|"") return 0;; esac; [ "$(field "$ri" deleted)" = true ] || [ "$(field "$ri" disabled)" = true ]; }
frames_host() { dc host "refs 42 1500" | grep -oE "[0-9A-F]{8} base=$FRAME_BASE" | cut -c1-8; }
held_vm() { rsince vm act | grep -q "MirrorFrames: $PIPER is held in frame copy"; }
LEFTOVER=""

# 1. Piper with the host, in a frame there and in a copy here
rmark vm step; rmark host step
if [ "$MODE" = unload ]; then
	# The host steps back from the friend's nearest door before the set-up (real walk): their stand-in took the friend's E there
	# (rig, 2026-11-22), and the host walking once Piper is in her suit led her to get out of it in the host's game.
	D0=${DOOR:-$(doors_near vm | head -1)}
	[ -n "$D0" ] && { step_aside host "$D0" 400 || say "unload: the host's step aside ended short ($WALKED)"; say "unload: the host stepped back from door $D0"; }
fi
say "set-up ($MODE): Piper ($PIPER) enabled, moved to the host's player (console moveto), made the host's companion (console SetCompanion)"
dc host "console $PIPER.enable" >/dev/null; sleep 1
dc host "console $PIPER.moveto player" >/dev/null; sleep 3
dc host "console cqf Followers \"FollowersScript.SetCompanion\" $PIPER 1 1 1" >/dev/null; sleep 4
wait_for 20 owned_here || verdict VOID "the host's game does not own Piper ($(dc host "mirror $PIPER" | cut -c1-160))"
wait_for 40 mirrored || verdict VOID "the friend's game does not mirror Piper ($(dc vm "mirror $PIPER" | cut -c1-200))"
before=$(frames_host)
dc host "console $PIPER.placeatme $FRAME_BASE" >/dev/null; sleep 2
FRAME=""
for r in $(frames_host); do grep -qx "$r" <<<"$before" && continue; [ "$(field "$(dc host "refinfo $r")" disabled)" = true ] && continue; FRAME=$r; break; done
[ -n "$FRAME" ] || verdict VOID "no frame was placed at Piper in the host's game"
say "set-up: a frame ($FRAME) placed at the host's Piper (console placeatme), Piper ordered into it (console Actor.SwitchToPowerArmor)"
dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" $FRAME" >/dev/null
wait_for 30 in_pa host || verdict VOID "the host's Piper did not get into frame $FRAME ($(npcpa host | cut -c1-160))"
wait_for 40 friend_in_copy || { rkeep vm npc-power-armor-hold; verdict VOID "the friend's Piper did not get into a frame copy ($(npcpa vm | cut -c1-200))"; }
P=$(npcpa vm); COPY=$(field "$P" frame)
say "in: the friend's Piper in copy $COPY: $(cut -c1-160 <<<"$P")"
sleep 2
alive_or_yes "in"

# 2. the host's copy of her goes quiet or away
rmark vm act; rmark host act
case $MODE in
runout)
	say "runout: the host's game stalls $STALL s (dev stall: its main thread held, no NPC states)"
	( dc host "stall $STALL" >/dev/null 2>&1 & )
	sleep $((STALL + 3)) ;;
leave)
	say "leave: the host's game stalls 8 s (dev stall); the friend leaves (net leave) once the hold has begun"
	( dc host "stall 8" >/dev/null 2>&1 & )
	wait_for 7 held_vm || say "leave: no hold seen in the friend's log 7 s in ($(rsince vm act | grep -E "MirrorFrames: $PIPER" | tail -2 | cut -c1-200 | paste -sd'|' -))"
	held_vm || { sleep 4; verdict VOID "leave: the friend's Piper was never held (the stall did not silence her)"; }
	say "leave: held; the friend leaves now (net leave)"
	dc vm 'net leave' >/dev/null
	sleep 6 ;;
door)
	# rig.sh join's coc puts the player at an interior's way in; Piper follows the host through. The friend steps back from the
	# host's nearest door first (real walk): their stand-in stood at it and took the host's E (rig, 2026-11-22).
	D0=${DOOR:-$(doors_near host | head -1)}
	[ -n "$D0" ] && { step_aside vm "$D0" 400 || say "door: the friend's step aside ended short ($WALKED)"; say "door: the friend stepped back from door $D0"; }
	go_through host door $DOOR || verdict VOID "door: no door near the host's player took them elsewhere (refs 32 4000: $(dc host 'refs 32 4000' | cut -c1-200))"
	say "door: the host: $(dc host status | grep -oE "location=[0-9A-F]+ '[^']*'"); its Piper: $(dc host "mirror $PIPER" | cut -c1-100)"
	sleep 4 ;;
unload)
	say "unload: the friend goes through a door, leaving Piper (in her copy) behind with the host"
	# Still in her copy (the host stood still since the set-up): else this run would show nothing of the unload.
	friend_in_copy || verdict VOID "unload: the friend's Piper was out of her copy before the friend left ($(npcpa vm | cut -c1-160))"
	go_through vm unload $DOOR || verdict VOID "unload: no door near the friend's player took them elsewhere"
	say "unload: the friend: $(dc vm status | grep -oE "location=[0-9A-F]+ '[^']*'"); its Piper: $(dc vm "mirror $PIPER" | cut -c1-100)"
	sleep 4 ;;
save)
	say "save: the host's game stalls 12 s (dev stall); the friend saves (console save $PA_SAVE) once the hold has begun"
	( dc host "stall 12" >/dev/null 2>&1 & )
	wait_for 8 held_vm || say "save: no hold seen in the friend's log 8 s in"
	held_vm || { sleep 6; verdict VOID "save: the friend's Piper was never held (the stall did not silence her)"; }
	say "save: held; the friend saves now, during the hold"
	dc vm "console save $PA_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	sleep 4 ;;
*) verdict VOID "MODE is runout, leave, door, unload or save, not '$MODE'" ;;
esac
alive_or_yes "$MODE"
out_seen() { rsince vm act | grep -qE "MirrorFrames: $PIPER (out of|stays in) frame copy $COPY"; }
wait_for 15 out_seen  # a released owner holds her up to 10 s for a death first
L=$(rsince vm act | grep -E "MirrorFrames: $PIPER|MirrorFrames: [0-9A-F]{8}'s hold|NpcSync: $PIPER" | cut -c1-260)
say "$MODE: the friend's log: $(grep -E 'held in frame|hold in its frame copy ends|out of frame copy|stays in frame copy|did not get out|waits on' <<<"$L" | tail -5 | paste -sd'|' -)"
grep -q "stays in frame copy $COPY" <<<"$L" && LEFTOVER="$LEFTOVER the copy $COPY was left to the game with Piper in it ($(grep "stays in frame copy $COPY" <<<"$L" | tail -1));"
grep -q "out of frame copy $COPY" <<<"$L" || LEFTOVER="$LEFTOVER the friend's log has no exit from the copy $COPY;"
# Its cell not loaded at the exit (unload): the engine let go of the copy at the exit's demote; it must be forgotten, its record dropped.
grep -q "frame copy $COPY was let go of by the engine" <<<"$L" && say "$MODE: the copy $COPY was unloaded by the engine at the exit: forgotten, its record dropped"
[ "$MODE" = door ] || held_vm || say "$MODE: (no hold seen: her owner's copy was never silent 3 s)"
wait_for 10 copy_gone "$COPY" || LEFTOVER="$LEFTOVER the frame copy $COPY stayed ($(dc vm "refinfo $COPY" | cut -c1-120));"
say "$MODE: the friend's Piper now: $(npcpa vm | cut -c1-160)"

# 4. a save, the leave, the save loaded alone
if [ "$MODE" = save ]; then
	say "save: (saved during the hold) the friend leaves (net leave) and loads it alone (console load)"
else
	say "save: the friend saves (console save $PA_SAVE), leaves (net leave, when still joined) and loads it alone (console load)"
	dc vm "console save $PA_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
	alive_or_yes "save"
fi
joined && { dc vm "net leave" >/dev/null; sleep 6; }
alive_or_yes "leave"
rmark vm load
dc vm "console load $PA_SAVE" >/dev/null; sleep 20; wait_ingame vm 60 || { alive_or_yes "load"; verdict VOID "the friend's game did not come back from the load"; }
sleep 10
alive_or_yes "load"
P=$(npcpa vm); F=$(dc vm 'paframes 1500')
say "load: the friend's Piper: $(cut -c1-160 <<<"$P")"
say "load: frames near the friend's player: $(cut -c1-300 <<<"$F")"
[ "$(field "$P" inPA)" = true ] && LEFTOVER="$LEFTOVER the save loaded alone has Piper in power armor (frame $(field "$P" frame));"
grep -qE "[0-9A-F]{8} base=" <<<"$F" && LEFTOVER="$LEFTOVER the save loaded alone has a frame near the friend's player;"
copy_gone "$COPY" || LEFTOVER="$LEFTOVER the save loaded alone has the copy $COPY ($(dc vm "refinfo $COPY" | cut -c1-120));"
say "load: the friend's log: $(rsince vm load | grep -iE 'stray|power armor|frame' | tail -4 | cut -c1-200 | paste -sd'|' -)"
alive_or_yes "the end"
# The host's Piper out of her frame for whatever comes next
in_pa host && dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" None" >/dev/null
rkeep vm npc-power-armor-hold; rkeep host npc-power-armor-hold
[ -n "$LEFTOVER" ] && verdict YES "$MODE:$LEFTOVER"
verdict NO "$MODE: the friend's Piper came out of the copy with the engine's exit, the copy went, and the save loaded alone has no frame and no power armor"
