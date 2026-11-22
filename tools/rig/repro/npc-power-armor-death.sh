#!/bin/bash
# npc-power-armor-death.sh: an NPC that dies in power armor in its owner's game, as the friend's game shows it in a frame copy (the
# class of npc-power-armor.sh: a settler or a raider in a suit, killed). The friend's copy of the NPC stops being mirrored as it
# dies, and is taken out of the frame copy the friend's game placed for it, with the engine's own exit run on a dead actor; the
# vanilla game never runs that exit on a corpse, so this is looked at for a crash, a hang, and anything left in the friend's
# game or save.
#   1. Both games at PLACE (default Sanctuary; console coc CELL when not, said), joined. The NPC: NPC_REF, a placed NPC of the
#      world, enabled and brought to the host's player (console enable, moveto); else a base from safenpc (or NPC_BASE) placed
#      with the dev spawn (a runtime reference, which the session does not mirror: VOID then). The friend's game mirrors it.
#      Corvega's own raiders are mirrored and can die (PLACE=Corvega CELL=CorvegaAssemblyPlant01 NPC_REF=<one the host's game
#      runs>, console stopcombat and setav aggression 0 first). Run 2026-11-21: YES on 25ae884e3 (the friend's raider was taken
#      out alive as its owner's copy went silent, 44 ms before its death came), and the body in its suit on both games on
#      48aa2a083 (the frame in the friend's save loaded alone is the body's own; this check was then made to say so).
#   2. A frame (base 0004A9C1) placed at it in the host's game (console placeatme) and the NPC ordered into it (console
#      Actor.SwitchToPowerArmor). The friend's copy is looked for in a frame copy (its log: "MirrorFrames: <id> is in frame copy").
#   3. The host's NPC killed in the suit (console kill). The friend's copy is looked at for 30 s: dead, in or out of power armor
#      as the host's NPC is (out: the frame copy gone), and both games answering with no new crash log.
#   4. The friend saves (console save, a throwaway save named below), leaves the session (net leave) and loads it alone
#      (console load): no frame near its player, nothing in power armor, no crash.
# REPRO: YES when a game crashes or stops answering, or the friend's dead copy is not in or out as the host's is, or the copy stays, or the
# save loaded alone holds either. NO when it comes out, the copy goes, and nothing is left. VOID when the scene could not be made.
# The throwaway save ($PA_SAVE) stays on the VM: the rig turn moves it aside after (never deleted).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
FRAME_BASE=0004A9C1
PA_SAVE=${PA_SAVE:-f4mp_repro_npc_power_armor_death}
need_session
CH0=$(crash_list_host); CV0=$(crash_list_vm)
crashed() {
	local nh nv w
	nh=$(crash_new "$CH0" "$(crash_list_host)"); nv=$(crash_new "$CV0" "$(crash_list_vm)")
	CRASH="${nh:+host crash log: $nh; }${nv:+vm crash log: $nv; }"
	for w in host vm; do dc "$w" status | grep -q 'ingame=' || CRASH="$CRASH$w does not answer ($(dc "$w" status | cut -c1-80)); "; done
	[ -n "$CRASH" ]
}
alive_or_yes() { crashed || return 0; rkeep vm npc-power-armor-death; rkeep host npc-power-armor-death; verdict YES "$1: $CRASH"; }
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"${PLACE:-Sanctuary}"*) ;; *) say "set-up: $w is not at ${PLACE:-Sanctuary} ($loc): console coc ${CELL:-SanctuaryExt}"; dc $w focus >/dev/null; dc $w "console coc ${CELL:-SanctuaryExt}" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done
npcpa() { dc "$1" "npcpa $2"; }

# 1. an NPC with the host, mirrored in the friend's game
rmark vm step; rmark host step
owned_here() { [ "$(field "$(dc host "mirror $NPC")" owned)" = true ]; }
mirrored() { [ "$(field "$(dc vm "mirror $NPC")" mirror)" = true ] && [ "$(field "$(dc vm "mirror $NPC")" loaded)" = true ]; }
if [ -n "$NPC_REF" ]; then
	# A placed NPC of the world (a runtime one from the dev spawn is not mirrored): enabled and brought to the host's player.
	NPC=$NPC_REF
	say "set-up: $NPC enabled and moved to the host's player (console enable, moveto)"
	dc host "console $NPC.enable" >/dev/null; sleep 1
	dc host "console $NPC.moveto player" >/dev/null; sleep 4
else
	BASE=${NPC_BASE:-$(dc host safenpc | grep -oE '[0-9A-F]{8}' | head -1)}
	[ -n "$BASE" ] || verdict VOID "no NPC base to place (safenpc found none)"
	NPC=$(dc host "spawn $BASE 250" | grep -oE 'ref=[0-9A-F]{8}' | head -1 | cut -d= -f2)
	[ -n "$NPC" ] || verdict VOID "the NPC base $BASE could not be placed in the host's game"
	say "set-up: NPC base $BASE placed in front of the host's player (dev spawn): $NPC"
fi
wait_for 20 owned_here || verdict VOID "the host's game does not own $NPC ($(dc host "mirror $NPC" | cut -c1-160)); NPC_REF=<a placed NPC> instead"
wait_for 40 mirrored || verdict VOID "the friend's game does not mirror $NPC ($(dc vm "mirror $NPC" | cut -c1-160))"
say "set-up: host: $(dc host "mirror $NPC" | cut -c1-120)"

# 2. into a frame in the host's game; the friend's copy into a frame copy
frames_host() { dc host "refs 42 1500" | grep -oE "[0-9A-F]{8} base=$FRAME_BASE" | cut -c1-8; }
BEFORE=$(frames_host)
dc host "console $NPC.placeatme $FRAME_BASE" >/dev/null; sleep 2
FRAME=""
for r in $(frames_host); do grep -qx "$r" <<<"$BEFORE" || { FRAME=$r; break; }; done
[ -n "$FRAME" ] || verdict VOID "no frame was placed at the NPC in the host's game"
say "in: a frame ($FRAME) placed at the host's NPC (console placeatme), the NPC ordered into it (console Actor.SwitchToPowerArmor)"
dc host "console $NPC.cf \"Actor.SwitchToPowerArmor\" $FRAME" >/dev/null
in_host() { [ "$(field "$(npcpa host "$NPC")" inPA)" = true ]; }
wait_for 30 in_host || verdict VOID "the host's NPC did not get into the frame ($(npcpa host "$NPC"))"
say "in: the host's NPC: $(npcpa host "$NPC")"
friend_in() { MIRROR=$(rsince vm step | grep -oE 'MirrorFrames: [0-9A-F]{8} is in frame copy' | tail -1 | awk '{print $2}'); [ -n "$MIRROR" ]; }
wait_for 60 friend_in || { rkeep vm npc-power-armor-death; verdict VOID "the friend's copy of the NPC was not put in a frame copy ($(rsince vm step | grep -E 'MirrorFrames|NpcSync' | tail -3 | cut -c1-200 | paste -sd'|' -))"; }
P=$(npcpa vm "$MIRROR"); COPY=$(field "$P" frame)
say "in: the friend's copy $MIRROR: $P"
alive_or_yes "in"

# 3. killed in the suit
rmark vm death
say "death: the host's NPC killed in the suit (console kill)"
dc host "console $NPC.kill" >/dev/null
dead_vm() { [ "$(field "$(dc vm "refinfo $MIRROR")" dead)" = true ] || dc vm "actors 3000" | grep -q "$MIRROR.*dead=true"; }
wait_for 30 dead_vm || say "death: the friend's copy is not seen dead yet ($(dc vm "refinfo $MIRROR" | cut -c1-160))"
sleep 8
alive_or_yes "death"
LEFTOVER=""
P=$(npcpa vm "$MIRROR"); H=$(npcpa host "$NPC")
say "death: the friend's copy: $P; host's: $(cut -c1-160 <<<"$H")"
# The friend's copy shows what the host's game shows: a corpse left in its suit (as the vanilla game leaves one) stays in the
# frame copy; one the host's game took out comes out.
[ "$(field "$P" inPA)" = "$(field "$H" inPA)" ] || LEFTOVER="$LEFTOVER the friend's dead copy (inPA=$(field "$P" inPA)) does not match the host's (inPA=$(field "$H" inPA));"
copy_gone() { local ri; ri=$(dc vm "refinfo $1"); case "$ri" in error*|"") return 0;; esac; [ "$(field "$ri" deleted)" = true ] || [ "$(field "$ri" disabled)" = true ]; }
[ "$(field "$P" inPA)" = true ] || [ -z "$COPY" ] || [ "$COPY" = 00000000 ] || wait_for 10 copy_gone "$COPY" || LEFTOVER="$LEFTOVER the frame copy $COPY stayed after the copy came out ($(dc vm "refinfo $COPY" | cut -c1-120));"
say "death: the friend's log: $(rsince vm death | grep -E 'MirrorFrames|power armor' | tail -4 | cut -c1-200 | paste -sd'|' -)"

# 4. a save, the leave, the save loaded alone
say "save: the friend saves (console save $PA_SAVE), leaves (net leave) and loads it alone (console load)"
dc vm "console save $PA_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
alive_or_yes "save"
dc vm "net leave" >/dev/null; sleep 6
alive_or_yes "leave"
rmark vm load
dc vm "console load $PA_SAVE" >/dev/null; sleep 20; wait_ingame vm 60 || { alive_or_yes "load"; verdict VOID "the friend's game did not come back from the load"; }
sleep 10
alive_or_yes "load"
F=$(dc vm 'paframes 1500')
say "load: frames near the friend's player: $(cut -c1-300 <<<"$F")"
# A body left in its suit keeps its frame (the copy was left to the game with it): that one is the save's own, worn by the body.
if [ "$(field "$P" inPA)" = true ]; then
	say "load: the body was left in its suit, its frame copy ($(field "$P" frame)) left to the game with it: a frame here is that one"
else
	grep -qE "[0-9A-F]{8} base=" <<<"$F" && LEFTOVER="$LEFTOVER the save loaded alone has a frame near the friend's player;"
fi
say "load: the friend's log: $(rsince vm load | grep -iE 'stray|power armor|frame' | tail -4 | cut -c1-200 | paste -sd'|' -)"
alive_or_yes "the end"
rkeep vm npc-power-armor-death; rkeep host npc-power-armor-death
[ -n "$LEFTOVER" ] && verdict YES "$LEFTOVER"
verdict NO "the friend's dead copy showed what the host's game shows, no game crashed, and the save loaded alone holds no frame"
