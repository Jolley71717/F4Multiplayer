#!/bin/bash
# npc-power-armor-play.sh: the host's companion in power armor through a real fight, as the friend's game shows it (the class of
# npc-power-armor.sh, played rather than set: the scripted repros put the suit on and take it off by console while both players
# stand). Both players walk (the pad), the companion follows the host on its own AI, the host shoots the raiders (real clicks,
# aimed with turnto), the companion fights and takes hits (it may die in the suit), and then both players walk out through a door
# and back in.
#   set-up (console, said): both games in Corvega's plant (rig.sh join CorvegaAssemblyPlant01, the host first, so the host's game
#      runs its raiders), Piper enabled, brought to the host (moveto) and made the host's companion (SetCompanion), put in a fresh
#      frame (placeatme + Actor.SwitchToPowerArmor); a 10mm pistol and rounds for the host (additem, equipitem).
#   play: FIGHT_S seconds (150) of rounds: the host walks toward the nearest living raider, stops 500 u short, turns to it and
#      fires 3 shots; the friend walks to the host. Then both walk to the door nearest to where they came in, the host opens it
#      (E), the friend follows (E), both come back in the same way.
#   watched every 3 s: the host's Piper (in power armor, dead, where) and the friend's (in power armor, in the copy, mirrored,
#      where). The friend's is counted wrong when the host's is in the suit and the friend's not (or the other way) for 2 samples
#      in a row while both games have her loaded and mirrored, and frozen when the host's moved 150 u or more over 10 s while the
#      friend's moved less than 30 (mirrored, the host's loaded).
#   end: the friend saves (a throwaway save, named below), leaves (net leave) and loads it alone: no frame near with a living NPC
#      in it, Piper (alive) not in power armor.
# REPRO: YES when a game crashes or stops answering, the friend's Piper was wrong or frozen, or the save loaded alone has a living
# NPC in a frame or Piper in power armor. NO otherwise. VOID when the scene could not be made. Every sample is in samples.txt.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
. "$RIG/crashlib.sh"
PIPER=00002F1F; FRAME_BASE=0004A9C1; PISTOL=00004822; ROUNDS=0001F276
FIGHT_S=${FIGHT_S:-150}
PA_SAVE=${PA_SAVE:-f4mp_repro_npc_power_armor_play}
SAMPLES=$REPRO_OUT/samples.txt
need_session
CH0=$(crash_list_host); CV0=$(crash_list_vm)
crashed() {
	local nh nv w
	nh=$(crash_new "$CH0" "$(crash_list_host)"); nv=$(crash_new "$CV0" "$(crash_list_vm)")
	CRASH="${nh:+host crash log: $nh; }${nv:+vm crash log: $nv; }"
	for w in host vm; do dc "$w" status | grep -q 'ingame=' || CRASH="$CRASH$w does not answer ($(dc "$w" status | cut -c1-80)); "; done
	[ -n "$CRASH" ]
}
alive_or_yes() { crashed || return 0; rkeep vm npc-power-armor-play; rkeep host npc-power-armor-play; verdict YES "$1: $CRASH"; }
npcpa() { dc "$1" "npcpa $PIPER"; }
in_pa() { [ "$(field "$(npcpa "$1")" inPA)" = true ]; }
owned_here() { [ "$(field "$(dc host "mirror $PIPER")" owned)" = true ]; }
mirrored() { [ "$(field "$(dc vm "mirror $PIPER")" mirror)" = true ] && [ "$(field "$(dc vm "mirror $PIPER")" loaded)" = true ]; }
friend_in_copy() { local p; p=$(npcpa vm); [ "$(field "$p" inPA)" = true ] && [ "$(field "$p" ourCopy)" = true ]; }
frames_host() { dc host "refs 42 1500" | grep -oE "[0-9A-F]{8} base=$FRAME_BASE" | cut -c1-8; }
xy_of() { local r; r=$(dc "$1" "refinfo $2"); echo "$(field "$r" x) $(field "$r" y)"; }
nearest_door() {
	local p; p=$(pos_of "$1")
	dc "$1" "refs 32 4000" | tr ';' '\n' | awk -v p="$p" '{ split(p, q, " "); n = split($0, w, " "); for (i = 1; i <= n; i++) if (w[i] == "at") { d = (w[i+1]-q[1])^2 + (w[i+2]-q[2])^2; id = $1; gsub(/[^0-9A-F]/, "", id); if (best == "" || d < bd) { best = id; bd = d } } } END { print best }'
}
WRONG=0; WRONG_RUN=0; FROZEN=0; SAMPLED=0; INPA_BOTH=0; LEFTOVER=""
HIST=()  # "t hostxy friendxy" of the last samples
sample() {
	local label=$1 h v hm vm_ hxy vxy hin vin t=$SECONDS
	h=$(npcpa host); v=$(npcpa vm); hm=$(dc host "mirror $PIPER"); vm_=$(dc vm "mirror $PIPER")
	hxy=$(xy_of host $PIPER); vxy=$(xy_of vm $PIPER)
	hin=$(field "$h" inPA); vin=$(field "$v" inPA)
	echo "$(date +%T) [$1] host: inPA=$hin state=$(field "$h" state) at $hxy loaded=$(field "$hm" loaded) | friend: inPA=$vin ourCopy=$(field "$v" ourCopy) mirror=$(field "$vm_" mirror) loaded=$(field "$vm_" loaded) at $vxy" >> "$SAMPLES"
	SAMPLED=$((SAMPLED + 1))
	local both=false
	[ "$(field "$hm" loaded)" = true ] && [ "$(field "$vm_" mirror)" = true ] && [ "$(field "$vm_" loaded)" = true ] && both=true
	if $both && [ -n "$hin" ] && [ -n "$vin" ] && [ "$hin" != "$vin" ]; then
		WRONG_RUN=$((WRONG_RUN + 1)); [ $WRONG_RUN -eq 2 ] && { WRONG=$((WRONG + 1)); say "watch [$1]: the friend's Piper inPA=$vin while the host's is inPA=$hin (2 samples running)"; }
	else
		WRONG_RUN=0
	fi
	$both && [ "$hin" = true ] && [ "$vin" = true ] && INPA_BOTH=$((INPA_BOTH + 1))
	HIST+=("$t $hxy $vxy")
	# frozen: over the samples 9-12 s ago to now, the host's moved 150+ and the friend's under 30 (both loaded and mirrored now)
	if $both; then
		local old e
		for e in "${HIST[@]}"; do set -- $e; [ $((t - $1)) -ge 9 ] && [ $((t - $1)) -le 12 ] && old="$e"; done
		if [ -n "$old" ]; then
			set -- $old
			local dh dv
			dh=$(dist_xy "$2 $3" "$hxy"); dv=$(dist_xy "$4 $5" "$vxy")
			if [ -n "$dh" ] && [ -n "$dv" ] && [ "$dh" -ge 150 ] && [ "$dv" -lt 30 ]; then
				FROZEN=$((FROZEN + 1)); say "watch [$label]: the friend's Piper stood still (moved $dv u in ~10 s) while the host's moved $dh u"
			fi
		fi
	fi
}

# set-up (console)
rmark vm step; rmark host step
say "set-up (console): Piper ($PIPER) enabled, moved to the host's player (moveto), made the host's companion (SetCompanion); a 10mm pistol and rounds for the host (additem, equipitem)"
dc host "console $PIPER.enable" >/dev/null; sleep 1
dc host "console $PIPER.moveto player" >/dev/null; sleep 3
dc host "console cqf Followers \"FollowersScript.SetCompanion\" $PIPER 1 1 1" >/dev/null; sleep 4
dc host "console player.additem $PISTOL 1" >/dev/null; dc host "console player.additem $ROUNDS 400" >/dev/null; dc host "console player.equipitem $PISTOL" >/dev/null; dc host "draw on" >/dev/null
wait_for 20 owned_here || verdict VOID "the host's game does not own Piper ($(dc host "mirror $PIPER" | cut -c1-160))"
wait_for 40 mirrored || verdict VOID "the friend's game does not mirror Piper ($(dc vm "mirror $PIPER" | cut -c1-200))"
before=$(frames_host)
dc host "console $PIPER.placeatme $FRAME_BASE" >/dev/null; sleep 2
FRAME=""
for r in $(frames_host); do grep -qx "$r" <<<"$before" && continue; [ "$(field "$(dc host "refinfo $r")" disabled)" = true ] && continue; FRAME=$r; break; done
[ -n "$FRAME" ] || verdict VOID "no frame was placed at Piper in the host's game"
say "set-up (console): a frame ($FRAME) placed at the host's Piper, Piper ordered into it (Actor.SwitchToPowerArmor)"
dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" $FRAME" >/dev/null
wait_for 30 in_pa host || verdict VOID "the host's Piper did not get into frame $FRAME ($(npcpa host | cut -c1-160))"
wait_for 40 friend_in_copy || { rkeep vm npc-power-armor-play; verdict VOID "the friend's Piper did not get into a frame copy ($(npcpa vm | cut -c1-200))"; }
COPY=$(field "$(npcpa vm)" frame)
DOOR=$(nearest_door host); VDOOR=$(nearest_door vm)
say "set-up: the friend's Piper in copy $COPY; the way out: door $DOOR (host), $VDOOR (friend)"
sample setup
alive_or_yes "set-up"

# play: the fight (real walks, real clicks)
rmark vm play; rmark host play
say "play: $FIGHT_S s of fighting: the host walks at the nearest living raider and shoots it, the friend walks after the host, Piper follows on her own AI"
END=$((SECONDS + FIGHT_S)); SHOTS=0; ROUND=0
while [ $SECONDS -lt $END ]; do
	ROUND=$((ROUND + 1))
	HP=$(pos_of host)
	TARGET=$(dc host 'actors 3000' | tr ';' '\n' | grep "dead=false" | grep -i "raider" | while read -r line; do r=$(grep -oE "[0-9A-F]{8}" <<<"$line" | head -1); [ -n "$r" ] || continue; echo "$(dist_xy "$HP" "$(xy_of host $r)") $r"; done | sort -n | awk 'NF == 2 { print $2; exit }')
	if [ -z "$TARGET" ]; then say "play: no living raider left near the host (round $ROUND)"; break; fi
	( rwalk vm "$(cut -d' ' -f1 <<<"$HP")" "$(cut -d' ' -f2 <<<"$HP")" 300 >/dev/null 2>&1 & )
	rwalk host "$TARGET" 500 0.9 >/dev/null 2>&1
	sample "round $ROUND walk"
	dc host "turnto $TARGET 300 aim" >/dev/null; sleep 0.6
	for s in 1 2 3; do dc host focus >/dev/null; dc host "click 90" >/dev/null; SHOTS=$((SHOTS + 1)); sleep 0.8; done
	sample "round $ROUND shots at $TARGET ($(field "$(dc host "refinfo $TARGET")" dead))"
	sleep 2; sample "round $ROUND after"
	alive_or_yes "play round $ROUND"
done
say "play: $ROUND rounds, $SHOTS shots; the host's Piper now: $(npcpa host | cut -c1-120)"

# out through the door and back (real walks, E)
for leg in out back; do
	D=$DOOR; FD=$VDOOR
	if [ $leg = back ]; then D=$(nearest_door host); FD=$(nearest_door vm); fi
	[ -n "$D" ] || { say "door $leg: no door near the host"; break; }
	say "door $leg: the host walks to door $D and opens it (E); the friend walks to door $FD and opens it (E)"
	rwalk host "$D" 150 >/dev/null 2>&1; rpress_e host "$D"; HR=$ROLLOVER
	sample "door $leg host through"
	rwalk vm "$FD" 150 >/dev/null 2>&1; rpress_e vm "$FD"; VR=$ROLLOVER
	say "door $leg: E on '$HR' (host), '$VR' (friend)"
	sleep 8; wait_ingame host 60; wait_ingame vm 60
	for i in 1 2 3 4 5; do sample "door $leg $i"; sleep 3; done
	say "door $leg: host at $(dc host status | grep -oE "location=[0-9A-F]+ '[^']*'"), friend at $(dc vm status | grep -oE "location=[0-9A-F]+ '[^']*'")"
	alive_or_yes "door $leg"
done
L=$(rsince vm play | grep -E "MirrorFrames: $PIPER|MirrorFrames: [0-9A-F]{8}'s hold" | cut -c1-240)
say "play: the friend's MirrorFrames lines for Piper: $(grep -cE . <<<"$L") ($(grep -oE 'is in frame copy|out of frame copy|is held in frame copy|hold in its frame copy ends|stays in frame copy' <<<"$L" | sort | uniq -c | paste -sd',' -))"
grep -E 'stays in frame copy' <<<"$L" | grep -v 'dead or dying' | head -2 | while read -r l; do say "play: $l"; done
grep -E 'stays in frame copy' <<<"$L" | grep -qv 'dead or dying' && LEFTOVER="$LEFTOVER a copy was left to the game with Piper alive in it;"
HOSTDEAD=$(field "$(dc host "refinfo $PIPER")" dead)

# the friend's save, loaded alone
say "save: the friend saves (console save $PA_SAVE), leaves (net leave) and loads it alone (console load)"
dc vm "console save $PA_SAVE" >/dev/null; sleep 8; wait_ingame vm 30
alive_or_yes "save"
dc vm 'net status' | grep -q 'status=joined' && { dc vm 'net leave' >/dev/null; sleep 6; }
alive_or_yes "leave"
rmark vm load
dc vm "console load $PA_SAVE" >/dev/null; sleep 20; wait_ingame vm 60 || { alive_or_yes "load"; verdict VOID "the friend's game did not come back from the load"; }
sleep 10
alive_or_yes "load"
P=$(npcpa vm); F=$(dc vm 'paframes 3000'); PD=$(field "$(dc vm "refinfo $PIPER")" dead)
say "load: the friend's Piper: $(cut -c1-160 <<<"$P") dead=$PD"
say "load: frames near the friend's player: $(cut -c1-400 <<<"$F")"
if [ "$(field "$P" inPA)" = true ] && [ "$PD" != true ]; then
	LEFTOVER="$LEFTOVER the save loaded alone has Piper alive in power armor (frame $(field "$P" frame));"
fi
# a frame someone wears near the friend: only a body may wear one (its copy left to the game with it)
grep -qE "worn=true|holds=true" <<<"$F" && [ "$PD" != true ] && LEFTOVER="$LEFTOVER the save loaded alone has a worn frame near the friend with no body in it;"
alive_or_yes "the end"
in_pa host && dc host "console $PIPER.cf \"Actor.SwitchToPowerArmor\" None" >/dev/null
rkeep vm npc-power-armor-play; rkeep host npc-power-armor-play
say "watch: $SAMPLED samples, $INPA_BOTH with both in the suit, wrong $WRONG, frozen $FROZEN; the host's Piper dead=$HOSTDEAD"
[ $WRONG -gt 0 ] && LEFTOVER="$LEFTOVER the friend's Piper was out of step with the host's $WRONG times;"
[ $FROZEN -gt 0 ] && LEFTOVER="$LEFTOVER the friend's Piper stood still while the host's moved $FROZEN times;"
[ -n "$LEFTOVER" ] && verdict YES "$LEFTOVER"
verdict NO "through $ROUND rounds of real fighting and the door out and back, the friend's Piper was in the suit as the host's ($INPA_BOTH samples), never frozen, and the save loaded alone has no living NPC in a frame"
