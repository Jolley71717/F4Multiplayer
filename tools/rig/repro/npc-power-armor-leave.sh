#!/bin/bash
# npc-power-armor-leave.sh: the friend's session ends while the friend's game shows an NPC in a frame copy (npc-power-armor.sh's
# class), in rounds, since what it once crashed on was a race: the engine's exit, run as the friend left, loads the NPC's own
# anim graph on a background thread, and the exit was finished over that load (crash in the preload task, 2026-11-21).
# Each round, both games at Sanctuary and joined (REJOIN when not: rig.sh join by default):
#   1. Piper enabled, brought to the host's player (console moveto), made the host's companion (console SetCompanion), and put
#      in a fresh frame in the host's game (console placeatme + Actor.SwitchToPowerArmor): the friend's Piper in a frame copy.
#   2. The session ends, one way per round in turn:
#      a. the friend leaves at once (net leave), Piper in the copy;
#      b. the host's Piper is ordered out (console Actor.SwitchToPowerArmor None) and the friend leaves LEAVE_DELAY s after,
#         while the friend's Piper is getting out of the copy;
#      c. the host stops hosting (net stophost), Piper in the copy.
#   3. 10 s on: both games answer, no new crash log, the friend's Piper out of power armor and the copy gone. The host's Piper
#      is ordered out (when still in) for the next round.
# The friend's log lines on the exit (a wait on the engine's preload, a failed exit) are said for each round.
# REPRO: YES when a game crashes or stops answering, or a round leaves the friend's Piper in power armor or the copy in place.
# NO when every round ends clean. VOID when a round's scene could not be made.
# Run 2026-11-21 on the fix build: 6 rounds, no crash or hang, out of the copy every time (rounds c were read by hand before the
# rejoin check above was added: the friend's game had joined the host again and Piper was rightly in a fresh copy).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
PIPER=00002F1F; FRAME_BASE=0004A9C1
ROUNDS=${ROUNDS:-6}; LEAVE_DELAY=${LEAVE_DELAY:-0.4}
REJOIN=${REJOIN:-bash $STAGE/rig.sh join}
CH0=$(crash_list_host); CV0=$(crash_list_vm)
crashed() {
	local nh nv w
	nh=$(crash_new "$CH0" "$(crash_list_host)"); nv=$(crash_new "$CV0" "$(crash_list_vm)")
	CRASH="${nh:+host crash log: $nh; }${nv:+vm crash log: $nv; }"
	for w in host vm; do dc "$w" status | grep -q 'ingame=' || CRASH="$CRASH$w does not answer ($(dc "$w" status | cut -c1-80)); "; done
	[ -n "$CRASH" ]
}
alive_or_yes() { crashed || return 0; rkeep vm npc-power-armor-leave; rkeep host npc-power-armor-leave; verdict YES "$1: $CRASH"; }
npcpa() { dc "$1" "npcpa $PIPER"; }
in_pa() { [ "$(field "$(npcpa "$1")" inPA)" = true ]; }
out_pa() { [ "$(field "$(npcpa "$1")" inPA)" = false ]; }
joined() { dc vm 'net status' | grep -q 'status=joined'; }
mirrored() { [ "$(field "$(dc vm "mirror $PIPER")" mirror)" = true ] && [ "$(field "$(dc vm "mirror $PIPER")" loaded)" = true ]; }
owned_here() { [ "$(field "$(dc host "mirror $PIPER")" owned)" = true ]; }
friend_in_copy() { local p; p=$(npcpa vm); [ "$(field "$p" inPA)" = true ] && [ "$(field "$p" ourCopy)" = true ]; }
copy_gone() { local ri; [ -z "$1" ] || [ "$1" = 00000000 ] && return 0; ri=$(dc vm "refinfo $1"); case "$ri" in error*|"") return 0;; esac; [ "$(field "$ri" deleted)" = true ] || [ "$(field "$ri" disabled)" = true ]; }
frames_host() { dc host "refs 42 1500" | grep -oE "[0-9A-F]{8} base=$FRAME_BASE" | cut -c1-8; }
LEFTOVER=""; WAITS=0

for round in $(seq 1 "$ROUNDS"); do
	way=$(( (round - 1) % 3 ))
	if ! joined; then
		dc host 'net status' | grep -qE 'status=(hosting|joined)' || dc host 'net host' >/dev/null
		say "round $round: joining again ($REJOIN)"
		$REJOIN >/dev/null 2>&1
		joined || verdict VOID "round $round: the friend's game did not join again"
	fi
	alive_or_yes "round $round start"
	rmark vm r$round; rmark host r$round
	dc host "console $PIPER.enable" >/dev/null; sleep 1
	dc host "console $PIPER.moveto player" >/dev/null; sleep 3
	dc host "console cqf Followers \"FollowersScript.SetCompanion\" $PIPER 1 1 1" >/dev/null; sleep 4
	wait_for 20 owned_here || verdict VOID "round $round: the host's game does not own Piper ($(dc host "mirror $PIPER" | cut -c1-160))"
	wait_for 40 mirrored || verdict VOID "round $round: the friend's game does not mirror Piper ($(dc vm "mirror $PIPER" | cut -c1-200))"
	before=$(frames_host)
	dc host "console $PIPER.placeatme $FRAME_BASE" >/dev/null; sleep 2
	FRAME=""
	for r in $(frames_host); do grep -qx "$r" <<<"$before" && continue; [ "$(field "$(dc host "refinfo $r")" disabled)" = true ] && continue; FRAME=$r; break; done
	[ -n "$FRAME" ] || verdict VOID "round $round: no frame was placed at Piper in the host's game"
	dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" $FRAME" >/dev/null
	wait_for 30 in_pa host || verdict VOID "round $round: the host's Piper did not get into frame $FRAME ($(npcpa host | cut -c1-160))"
	wait_for 40 friend_in_copy || { rkeep vm npc-power-armor-leave; verdict VOID "round $round: the friend's Piper did not get into a frame copy ($(npcpa vm | cut -c1-200))"; }
	P=$(npcpa vm); COPY=$(field "$P" frame)
	sleep 2
	case $way in
	0)	say "round $round: in (frame $FRAME, the friend's copy $COPY); the friend leaves at once (net leave)"
		dc vm 'net leave' >/dev/null ;;
	1)	say "round $round: in (frame $FRAME, the friend's copy $COPY); the host's Piper ordered out, the friend leaves $LEAVE_DELAY s after"
		dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" None" >/dev/null
		sleep "$LEAVE_DELAY"
		say "round $round: the friend's Piper as it leaves: $(npcpa vm | cut -c1-200)"
		dc vm 'net leave' >/dev/null ;;
	2)	say "round $round: in (frame $FRAME, the friend's copy $COPY); the host stops hosting (net stophost)"
		dc host 'net stophost' >/dev/null ;;
	esac
	sleep 10
	alive_or_yes "round $round end"
	P=$(npcpa vm)
	say "round $round: the friend's Piper: $(cut -c1-200 <<<"$P")"
	L=$(rsince vm r$round | grep -E "MirrorFrames: $PIPER" | cut -c1-220)
	grep -q 'waits on the engine' <<<"$L" && WAITS=$((WAITS + 1))
	say "round $round: the friend's log: $(grep -E 'out of frame copy|waits on the engine|did not get out' <<<"$L" | tail -3 | paste -sd'|' -)"
	# The host's game hosts again at once after net stophost, and the friend's game joins it again by itself: then Piper is
	# rightly back in a fresh copy, so that round asks for the exit from the old one.
	if [ $way = 2 ] && joined; then
		grep -q "out of frame copy $COPY" <<<"$L" || LEFTOVER="$LEFTOVER round $round: the friend's log has no exit from the copy $COPY;"
		say "round $round: the friend's game joined the host again by itself (the host hosts again at once)"
	else
		[ "$(field "$P" inPA)" = false ] || LEFTOVER="$LEFTOVER round $round: the friend's Piper is still in power armor ($(cut -c1-120 <<<"$P"));"
	fi
	wait_for 10 copy_gone "$COPY" || LEFTOVER="$LEFTOVER round $round: the frame copy $COPY stayed ($(dc vm "refinfo $COPY" | cut -c1-120));"
	if in_pa host; then dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" None" >/dev/null; fi
	wait_for 40 out_pa host || say "round $round: the host's Piper is not out of her frame 40 s on ($(npcpa host | cut -c1-120))"
	[ $way = 2 ] && dc host 'net host' >/dev/null
	joined && dc vm 'net leave' >/dev/null
	sleep 3
done
rkeep vm npc-power-armor-leave; rkeep host npc-power-armor-leave
say "rounds: $ROUNDS; rounds whose exit waited on the engine's preload: $WAITS"
[ -n "$LEFTOVER" ] && verdict YES "$LEFTOVER"
verdict NO "$ROUNDS rounds of the session ending with the friend's Piper in a frame copy (leave, leave while she gets out, host stops) ended clean: no crash, no hang, out of the copy, the copy gone"
