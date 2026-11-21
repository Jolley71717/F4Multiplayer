#!/bin/bash
# npc-power-armor.sh: a companion in power armor in the host's game shows out of it in the friend's game (player report: Piper in
# a suit, seen out of one by the other player). The friend's copy of the NPC is to follow the owner into a frame and out again, and
# no frame or power armor state of it is to reach the friend's save, or stay after the session or a load.
#   1. Both games at Sanctuary (console coc SanctuaryExt when not, said), joined. Piper enabled, brought to the host's player
#      (console moveto) and made the host's companion (console FollowersScript.SetCompanion), all said. The friend's game mirrors her.
#   2. A frame (base 0004A9C1, the rig's usual) placed beside Piper in the host's game (console placeatme) and Piper ordered into it
#      (console Actor.SwitchToPowerArmor <frame>): the host's Piper in power armor. The friend's Piper is looked at for 30 s.
#   3. The host's Piper ordered out (console Actor.SwitchToPowerArmor None; she walks out on her own AI). The friend's Piper is
#      looked at for 30 s: out of the suit, the frame copy the friend's game showed her in gone.
#   4. The host's Piper ordered in again. The friend saves (console save, a throwaway save named below) while she is in.
#   5. The friend leaves the session (net leave): their Piper out of the suit, the copy gone (a session end leaves nothing).
#   6. The friend loads the save alone (console load): Piper not in power armor, and no frame copy of the session's under the id it
#      had (the frames near the friend's player are listed too).
# What the friend's game holds of Piper is read with the dev channel's `npcpa` (her power armor state, the frame she is in, whether
# it is a copy the session placed); a build without it (0.21.0) is read from the plugin log ("is in power armor in its owner's game
# ... and out of one here") and `equipped`.
# REPRO: YES when the friend's Piper does not follow the host's into the suit (step 2), or does not come out (3), or any of steps
# 4-6 leaves a frame or power armor state in the friend's game or save. NO when she follows in and out and nothing is left.
# The throwaway save ($PA_SAVE) stays on the VM: the rig turn moves it aside after (never deleted).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
PIPER=00002F1F; FRAME_BASE=0004A9C1
PA_SAVE=${PA_SAVE:-f4mp_repro_npc_power_armor}
need_session
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) say "set-up: $w is not at Sanctuary ($loc): console coc SanctuaryExt"; dc $w focus >/dev/null; dc $w "console coc SanctuaryExt" >/dev/null; sleep 5; wait_ingame $w 60;; esac
done

# npcpa <who>: the dev channel's view of Piper's power armor in that game (empty on a build without it)
npcpa() { local r; r=$(dc "$1" "npcpa $PIPER"); grep -q '^error\|unknown' <<<"$r" && return 0; echo "$r"; }
HAS_NPCPA=$( [ -n "$(npcpa host)" ] && echo 1 )
in_pa() { [ "$(field "$(npcpa "$1")" inPA)" = true ]; }
out_pa() { [ "$(field "$(npcpa "$1")" inPA)" = false ]; }
mirrored() { [ "$(field "$(dc vm "mirror $PIPER")" mirror)" = true ] && [ "$(field "$(dc vm "mirror $PIPER")" loaded)" = true ]; }
owned_here() { [ "$(field "$(dc host "mirror $PIPER")" owned)" = true ]; }
# says <who> <regex>: the lines that game's log got since the mark matching it
says() { rsince "$1" "${3:-step}" | grep -E "$2"; }

# 1. Piper with the host
say "set-up: Piper ($PIPER) enabled, moved to the host's player (console moveto) and made the host's companion (console SetCompanion)"
dc host "console $PIPER.enable" >/dev/null; sleep 1
dc host "console $PIPER.moveto player" >/dev/null; sleep 3
dc host "console cqf Followers \"FollowersScript.SetCompanion\" $PIPER 1 1 1" >/dev/null; sleep 4
wait_for 20 owned_here || verdict VOID "the host's game does not own Piper ($(dc host "mirror $PIPER" | cut -c1-160))"
wait_for 40 mirrored || verdict VOID "the friend's game does not mirror Piper ($(dc vm "mirror $PIPER" | cut -c1-200))"
say "Piper: host $(dc host "mirror $PIPER" | cut -c1-80); friend $(dc vm "mirror $PIPER" | cut -c1-80)"
[ -n "$HAS_NPCPA" ] || say "this build has no npcpa: the friend's side is read from its log and equipped"
FOLLOW=""; LEFTOVER=""

# piper_in_host: the host's Piper into a fresh frame (console). PA_FRAME: the frame placed
piper_in_host() {
	local before r
	before=$(dc host "refs 42 600" | grep -oE "[0-9A-F]{8} base=$FRAME_BASE" | cut -c1-8)
	dc host "console $PIPER.placeatme $FRAME_BASE" >/dev/null; sleep 2
	PA_FRAME=""
	for r in $(dc host "refs 42 600" | grep -oE "[0-9A-F]{8} base=$FRAME_BASE" | cut -c1-8); do
		grep -qx "$r" <<<"$before" && continue
		[ "$(field "$(dc host "refinfo $r")" disabled)" = true ] && continue
		PA_FRAME=$r; break
	done
	[ -n "$PA_FRAME" ] || verdict VOID "no frame was placed at Piper in the host's game"
	say "$1: a frame ($PA_FRAME) placed at the host's Piper (console placeatme), Piper ordered into it (console Actor.SwitchToPowerArmor)"
	dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" $PA_FRAME" >/dev/null
	if [ -n "$HAS_NPCPA" ]; then
		wait_for 30 in_pa host || verdict VOID "$1: the host's Piper did not get into the frame ($(npcpa host))"
		say "$1: the host's Piper is in: $(npcpa host)"
	else
		sleep 12; say "$1: the host's Piper: $(dc host "equipped $PIPER" | cut -c1-240)"
	fi
}
# friend_follows_in <step>: 0 when the friend's Piper is in a frame within 30 s
friend_follows_in() {
	if [ -n "$HAS_NPCPA" ]; then
		if wait_for 30 in_pa vm; then say "$1: the friend's Piper is in a frame too: $(npcpa vm)"; return 0; fi
		say "$1: the friend's Piper is NOT in a frame 30 s on: $(npcpa vm)"; return 1
	fi
	sleep 20
	local l; l=$(says vm "$PIPER is in power armor in its owner's game" step | tail -1)
	say "$1: the friend's Piper: $(dc vm "equipped $PIPER" | cut -c1-200)"
	if [ -n "$l" ]; then say "$1: friend's log: ${l:0:260}"; return 1; fi
	say "$1: the friend's log says nothing of a frame it does not show (no npcpa to read the state)"; return 1
}
# copy_gone <id>: the frame copy is no longer in the friend's game (no reference, or disabled or deleted)
copy_gone() {
	[ -z "$1" ] && return 0
	local ri; ri=$(dc vm "refinfo $1")
	case "$ri" in error*|"") return 0;; esac
	[ "$(field "$ri" deleted)" = true ] || [ "$(field "$ri" disabled)" = true ]
}
# 2. in
rmark vm step; rmark host step
piper_in_host "in"
if friend_follows_in "in"; then FOLLOW="in"; COPY=$(field "$(npcpa vm)" frame); else
	rkeep vm npc-power-armor; rkeep host npc-power-armor
	verdict YES "the host's Piper is in power armor and the friend's is not"
fi

# 3. out
rmark vm step
say "out: the host's Piper ordered out (console Actor.SwitchToPowerArmor None)"
dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" None" >/dev/null
wait_for 40 out_pa host || verdict VOID "out: the host's Piper did not leave the frame ($(npcpa host))"
say "out: the host's Piper is out: $(npcpa host)"
if wait_for 30 out_pa vm; then say "out: the friend's Piper is out too: $(npcpa vm)"; FOLLOW="$FOLLOW out"; else LEFTOVER="$LEFTOVER the friend's Piper stayed in the frame after the host's left it ($(npcpa vm));"; fi
wait_for 10 copy_gone "$COPY" || LEFTOVER="$LEFTOVER the frame copy $COPY stayed in the friend's game after she left it ($(dc vm "refinfo $COPY" | cut -c1-120));"
say "out: the friend's log: $(says vm 'power armor|frame copy' | tail -4 | cut -c1-200 | paste -sd'|' -)"

# 4. in again, the friend saves
rmark vm step
piper_in_host "again"
friend_follows_in "again" || LEFTOVER="$LEFTOVER the friend's Piper did not follow her into the second frame;"
COPY2=$(field "$(npcpa vm)" frame)
rmark vm save
say "save: the friend saves with Piper in the suit (console save $PA_SAVE)"
dc vm "console save $PA_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
say "save: the friend's log: $(rsince vm save | grep -E 'NpcSync|power armor|SaveGuard' | grep -iE 'frame|power armor|stray|kept out' | tail -5 | cut -c1-200 | paste -sd'|' -)"
say "save: the friend's Piper right after: $(npcpa vm)"

# 5. the session ends for the friend
rmark vm leave
say "leave: the friend leaves the session (net leave)"
dc vm "net leave" >/dev/null; sleep 6
out_pa vm || LEFTOVER="$LEFTOVER after the friend left the session their Piper was still in the frame ($(npcpa vm));"
wait_for 10 copy_gone "$COPY2" || LEFTOVER="$LEFTOVER after the friend left the session the frame copy $COPY2 was still there ($(dc vm "refinfo $COPY2" | cut -c1-120));"
say "leave: the friend's Piper: $(npcpa vm); copy $COPY2: $(dc vm "refinfo $COPY2" | cut -c1-100)"

# 6. the save loaded alone
rmark vm load
say "load: the friend loads $PA_SAVE alone (console load)"
dc vm "console load $PA_SAVE" >/dev/null; sleep 20; wait_ingame vm 60 || verdict VOID "the friend's game did not come back from the load"
sleep 10
P=$(npcpa vm)
say "load: the friend's Piper: ${P:-$(dc vm "equipped $PIPER" | cut -c1-200)}; at $(dc vm "refinfo $PIPER" | cut -c1-140)"
[ -z "$HAS_NPCPA" ] || out_pa vm || LEFTOVER="$LEFTOVER the friend's save, loaded alone, has Piper in power armor ($P);"
for c in $COPY $COPY2; do copy_gone "$c" || LEFTOVER="$LEFTOVER the friend's save, loaded alone, holds the frame copy $c ($(dc vm "refinfo $c" | cut -c1-120));"; done
say "load: frames near the friend's player: $(dc vm 'paframes 1500' | cut -c1-300)"
say "load: the friend's log: $(rsince vm load | grep -iE 'stray|power armor|frame' | tail -5 | cut -c1-200 | paste -sd'|' -)"
rkeep vm npc-power-armor; rkeep host npc-power-armor
[ -n "$LEFTOVER" ] && verdict YES "$LEFTOVER"
verdict NO "the friend's Piper followed the host's in and out ($FOLLOW, and in again), and the save, the leave and the load left no frame or power armor state"
