#!/bin/bash
# bos-fire-support.sh: the Brotherhood of Steel's first two quests, Fire Support (BoS100: Danse, Haylen and Rhys fight ghouls at
# College Square by the Cambridge Police Station; the player helps; Danse then talks to the player) and Call to Arms (BoS101:
# Danse briefs the player in the station, then leads to ArcJet Systems). GitHub issue 638 (0.23.1): "fire support, and the quest
# after have some major desync between players, friend who is joining me has to go into a singleplayer instance and load the game
# from before the ghouls attack paladin danse and do the quest from the start cause of desync, in the next quest he cant even
# talk to danse if i talk to him and start the journey to arkjet systems".
#
# A fresh pair of games is needed: both joined (rig.sh join CambridgePDExt after a restart of both games) with BoS100 (0005DDAB,
# stage 5) and its fight controller BoS100Fight (00068D7B, stage 10) as a new game has them; the quests run once, so a game that
# already went through them is VOID (the console cannot start them again). Danse is 0005DE4D (base 00027683).
#
# PHASE 1 (Fire Support). Console set-up (said): both players placed at Danse (player.moveto 0005DE4D, College Square), or with
# FRIEND_FAR=1 only the host (the friend stays at the station). Then nothing but real input: the players wait for the ghoul fight,
# the host walks to Danse (the pad) when BoS100 reaches stage 40 ("speak to Paladin Danse", FIGHT_WAIT s at most), aims, presses E
# and answers with the wheel's bottom reply (Down, Enter) until the menu closes. The friend does nothing (FRIEND_FAR=1: it walks to
# Danse once the host has finished, and presses E there). Read every 4 s: both games' BoS100, BoS100Fight and BoS101 stages, the
# friend's player scene, dialogue menu and talkingTo, and who runs Danse.
# PHASE 2 (Call to Arms): after BoS100's 255 the quest BoS101 starts at stage 10 ("speak to Paladin Danse"); Danse goes into the
# station (CambridgePD01, cell 0001FA25). Console set-up (said): both players to coc CambridgePD01. Real input: the host walks to
# Danse, presses E and answers until the menu closes, the friend stands in his sight; then the friend walks to Danse and presses E
# (HOLD_TALK s for the host's talk, FRIEND_E s for the friend's menu to open).
#
# REPRO: YES when, after the host's conversation, a quest the host moved on is behind in the friend's game for LAG s or more
# (BoS100, BoS100Fight, BoS101), or the friend's player was in a scene with no conversation of its own open for 2 looks or more,
# or the friend's game claimed Danse (owner = the friend's player) or opened a dialogue menu of its own, or (phase 2) the friend's
# E on Danse opened no dialogue menu within FRIEND_E s, or Danse stands 300 units or more apart in the two games 30 s after the
# host's talk. NO when none of these. VOID when the games are not fresh (BoS100 not at stage 5 / the fight not at 10), the fight
# never reaches stage 40 in FIGHT_WAIT s, a player is in a scene before the run, or the host's conversation never opens.
# HOLD_TALK=240 FIGHT_WAIT=360 LAG=40 FRIEND_FAR=0 FRIEND_E=10 bash tools/rig/repro/bos-fire-support.sh
cd "$(dirname "$0")" || exit 2
. ./lib.sh
REPRO_TAIL_SKIP="gfx: |Texture wait"
DPS=0006F5BB; BOS100=0005DDAB; FIGHT=00068D7B; BOS101=0006F5C1; DANSE=0005DE4D; PD_CELL=0001FA25; PD_INT=CambridgePD01
HOLD_TALK=${HOLD_TALK:-240}; FIGHT_WAIT=${FIGHT_WAIT:-360}; LAG=${LAG:-40}; FRIEND_FAR=${FRIEND_FAR:-0}; FRIEND_E=${FRIEND_E:-10}
run_cap 2400
need_session
rmark host a; rmark vm a
dc host "log level debug" >/dev/null; dc vm "log level debug" >/dev/null
log_back() { dc host "log level info" >/dev/null; dc vm "log level info" >/dev/null; }
on_end log_back
close_talks() {
	local g n
	for g in host vm; do
		menu_open "$g" DialogueMenu || continue
		for n in 1 2 3; do dc "$g" "dialogue reset" >/dev/null; sleep 3; menu_open "$g" DialogueMenu || break; done
	done
	return 0
}
on_end close_talks

scene_of() { field "$(dc "$1" dialogue)" playerScene; }
stage_of() { field "$(dc "$1" "stages $2")" stage; }
danse_at() { local r; r=$(dc "$1" "refinfo $DANSE"); echo "$(field "$r" x) $(field "$r" y)"; }
danse_cell() { field "$(dc "$1" "refinfo $DANSE")" cell; }
danse_owner() { field "$(dc "$1" "refinfo $DANSE")" owner; }

say "before: BoS100 host $(stage_of host $BOS100) vm $(stage_of vm $BOS100); fight host $(stage_of host $FIGHT) vm $(stage_of vm $FIGHT); BoS101 host $(stage_of host $BOS101) vm $(stage_of vm $BOS101)"
for g in host vm; do
	[ "$(stage_of $g $BOS100)" = 5 ] && [ "$(stage_of $g $FIGHT)" = 10 ] || verdict VOID "$g's game is not a fresh one (BoS100 $(stage_of $g $BOS100), BoS100Fight $(stage_of $g $FIGHT)): restart both games and join again"
	[ "$(scene_of $g)" = 00000000 ] || verdict VOID "$g's player is in a scene before the run"
	menu_open $g DialogueMenu && verdict VOID "$g's dialogue menu is open before the run"
done

LOOKS=0; FSCENE=0; FMENU=0; FSCENES=""; LAGGED=""; LAGSINCE=""; CLAIM=""
# behind <quest>...: the quests whose friend stage is below the host's
behind() { local q out=""; for q in "$@"; do [ "$(stage_of vm "$q")" -lt "$(stage_of host "$q")" ] 2>/dev/null && out="$out $q(host $(stage_of host "$q"), friend $(stage_of vm "$q"))"; done; echo "$out"; }
# look: one sample of the friend and of the quests; sets LAGGED when a quest stayed behind LAG s
look() {
	local s m t b o
	s=$(scene_of vm); menu_open vm DialogueMenu && m=1 || m=0; t=$(field "$(dc vm 'net status')" talkingTo)
	LOOKS=$((LOOKS + 1)); [ "$m" = 1 ] && [ -z "$NOCOUNT" ] && FMENU=$((FMENU + 1))
	if [ "$s" != 00000000 ] && [ "$m" = 0 ]; then FSCENE=$((FSCENE + 1)); FSCENES="$FSCENES $s"; fi
	o=$(danse_owner host); [ "$o" = 2 ] && CLAIM="Danse owner $o (host's view)"
	b=$(behind $BOS100 $FIGHT $BOS101 $DPS)
	if [ -n "$b" ]; then [ -n "$LAGSINCE" ] || LAGSINCE=$SECONDS; [ $((SECONDS - LAGSINCE)) -ge "$LAG" ] && LAGGED="$b"; else LAGSINCE=""; fi
	say "look $LOOKS: host scene $(scene_of host) menu $(menu_open host DialogueMenu && echo open || echo closed) | friend scene $s menu $([ "$m" = 1 ] && echo open || echo closed) talkingTo $t | BoS100 $(stage_of host $BOS100)/$(stage_of vm $BOS100) fight $(stage_of host $FIGHT)/$(stage_of vm $FIGHT) BoS101 $(stage_of host $BOS101)/$(stage_of vm $BOS101) station-dialogue $(stage_of host $DPS)/$(stage_of vm $DPS) (host/friend) | Danse host cell $(danse_cell host) at $(danse_at host) owner $o, friend cell $(danse_cell vm) at $(danse_at vm)${b:+ BEHIND:$b}"
}
# talk <secs>: the host's wheel, the bottom reply each time it takes input, until the menu closes; 0 when it did
talk() {
	local secs=$1 t0=$SECONDS opts n=0 idle=0
	while menu_open host DialogueMenu; do
		[ $((SECONDS - t0)) -ge "$secs" ] && { say "the host's conversation was still open after $secs s ($n replies)"; return 1; }
		look
		if [ "$(field "$(dc host dialogue)" allowInput)" = true ] || [ "$idle" -ge 2 ]; then
			opts=$(rgfx_text host DialogueMenu root.Menu_mc.FadeHolder_mc 4 textField_tf | awk '{ $1 = ""; print substr($0, 2) }' | sort -u | paste -sd'|' -)
			[ -n "$(tr -d '| ' <<<"$opts")" ] || { sleep 1; continue; }
			n=$((n + 1)); say "reply $n: the wheel offers: $opts; the host takes the bottom one (Down, Enter)"
			rkey host "$VK_DOWN" 100; sleep 0.6; rkey host "$VK_ENTER" 100; sleep 4; idle=0
		else idle=$((idle + 1)); sleep 3; fi   # allowInput stayed false with the wheel up (2026-11-27, Danse's "ready to move out?"): after two looks the wheel's own text decides
	done
	say "the host's conversation ended after $((SECONDS - t0)) s ($n replies)"
}
# press_e_on_danse <who>: that player aims at Danse (the aim point moved until the crosshair names him) and presses E
press_e() {
	local a
	for a in aim "aim -40" "aim -60" "aim -20" "aim 20" "aim 0 25" "aim 0 -25"; do
		dc "$1" "turnto $DANSE 300 $a" >/dev/null; sleep 0.9; ROLLOVER=$(rrollover "$1")
		grep -qi DANSE <<<"$ROLLOVER" && break
	done
	say "$1's crosshair names '$ROLLOVER'; it presses E"
	rkey "$1" "$VK_E" 100
}

# ---- phase 1: Fire Support -----------------------------------------------------------------------------------------------------
say "PHASE 1. set-up (console): the host placed at Danse (player.moveto $DANSE, College Square)$([ "$FRIEND_FAR" = 1 ] || echo ", and the friend too")"
dc host "console player.moveto $DANSE" >/dev/null
[ "$FRIEND_FAR" = 1 ] || dc vm "console player.moveto $DANSE" >/dev/null
sleep 12; wait_ingame host 60; wait_ingame vm 60
say "the fight runs; the players wait for BoS100 stage 40 (at most $FIGHT_WAIT s)"
T0=$SECONDS
while [ "$(stage_of host $BOS100)" -lt 40 ] 2>/dev/null; do
	[ $((SECONDS - T0)) -ge "$FIGHT_WAIT" ] && verdict VOID "BoS100 did not reach stage 40 in $FIGHT_WAIT s (host $(stage_of host $BOS100))"
	look; sleep 4
done
say "BoS100 is at stage 40 in the host's game after $((SECONDS - T0)) s"
for i in 1 2 3; do look; sleep 4; done
for try in 1 2 3 4; do
	[ "$(stage_of host $BOS100)" -ge 255 ] 2>/dev/null && break
	[ $try -gt 1 ] && say "the host talks to Danse again (try $try; BoS100 is at $(stage_of host $BOS100))"
	rwalk host $DANSE 130 || say "the host's walk to Danse ended short ($WALKED)"
	press_e host
	wait_for 15 menu_open host DialogueMenu || { say "no menu"; continue; }
	talk "$HOLD_TALK" || verdict VOID "the host's conversation with Danse did not end"
	p1_done() { [ "$(stage_of host $BOS100)" -ge 255 ] 2>/dev/null; }; wait_for 60 p1_done || say "BoS100 is still at $(stage_of host $BOS100) 60 s after the talk"
done
if [ "$FRIEND_FAR" = 1 ]; then
	say "the friend walks to Danse (the pad)"; rwalk vm $DANSE 200 || say "the friend's walk ended short ($WALKED)"
fi
say "after the host's talk: watched 40 s"
for i in $(seq 1 10); do look; sleep 4; done
P1_BEHIND=$(behind $BOS100 $FIGHT $BOS101 $DPS)
say "phase 1 over: BoS100 host $(stage_of host $BOS100) vm $(stage_of vm $BOS100); BoS101 host $(stage_of host $BOS101) vm $(stage_of vm $BOS101); behind:${P1_BEHIND:- none}"
[ "$(stage_of host $BOS101)" -ge 10 ] 2>/dev/null || verdict VOID "the host's talk did not start Call to Arms (BoS101 stage $(stage_of host $BOS101))"

# ---- phase 2: Call to Arms ----------------------------------------------------------------------------------------------------
say "PHASE 2. set-up (console): both players to the station (coc $PD_INT)"
close_talks
P2_FIRST=${P2_FIRST:-host}; P2_SECOND=$(other_game $P2_FIRST)
say "$P2_FIRST enters first, $P2_SECOND ${P2_GAP:-25} s later (P2_FIRST, P2_GAP): the first game to load the station runs Danse, Haylen and Rhys"
dc $P2_FIRST "console coc $PD_INT" >/dev/null; sleep ${P2_GAP:-25}; dc $P2_SECOND "console coc $PD_INT" >/dev/null
sleep 12; wait_ingame host 60; wait_ingame vm 60; sleep 4
for g in host vm; do [ "$(cell_of $g)" = "$PD_CELL" ] || say "$g is in cell $(cell_of $g), not the station"; done
say "Danse: host cell $(danse_cell host) at $(danse_at host) 3d $(field "$(dc host "refinfo $DANSE")" 3d); friend cell $(danse_cell vm) at $(danse_at vm) 3d $(field "$(dc vm "refinfo $DANSE")" 3d)"
look
rwalk vm $DANSE 300 || say "the friend's walk to Danse ended short ($WALKED)"
rwalk host $DANSE 130 || say "the host's walk to Danse ended short ($WALKED)"
say "distances to Danse: host $(dist_xy "$(pos_of host)" "$(danse_at host)") friend $(dist_xy "$(pos_of vm)" "$(danse_at vm)")"
rmark vm c; rmark host c
for try in 1 2 3; do
	menu_open host DialogueMenu && break
	[ $try -gt 1 ] && { say "the host tries again (try $try)"; rwalk host $DANSE 130 || say "the host's walk to Danse ended short ($WALKED)"; }
	press_e host; wait_for 12 menu_open host DialogueMenu && break; say "the host's menu did not open"
done
if menu_open host DialogueMenu; then talk "$HOLD_TALK" || say "the host's talk with Danse did not end"; else HOSTNO=1; say "the host's dialogue menu never opened in the station (scene $(scene_of host))"; fi
for i in $(seq 1 8); do look; sleep 4; done
say "Danse after the talk: host cell $(danse_cell host) at $(danse_at host); friend cell $(danse_cell vm) at $(danse_at vm)"
# the friend now walks up to Danse and presses E
rmark vm e; NOCOUNT=1
rwalk vm $DANSE 130 || say "the friend's walk to Danse ended short ($WALKED)"
say "the friend is $(dist_xy "$(pos_of vm)" "$(danse_at vm)") from Danse (its view); presses E"
press_e vm
FOPEN=no; wait_for "$FRIEND_E" menu_open vm DialogueMenu && FOPEN=yes
say "the friend's dialogue menu opened: $FOPEN; talkingTo $(field "$(dc vm 'net status')" talkingTo); scene $(scene_of vm); hud: $(rhud vm)"
# the same press by the host, for comparison: at BoS101 stage 20 ("Follow Paladin Danse") the game itself may give him nothing to say to anyone
close_talks; HOSTE=no
if [ "$FOPEN" = no ]; then
	rmark host h; rwalk host $DANSE 130 || say "the host's walk to Danse ended short ($WALKED)"
	press_e host; wait_for "$FRIEND_E" menu_open host DialogueMenu && HOSTE=yes
	say "the host's own E on Danse (BoS101 stage $(stage_of host $BOS101)): dialogue menu opened: $HOSTE"; close_talks
fi
rsince vm e | grep -E "Dialogue:|NpcSync: (talking to|not talking to|our dialogue)|Conversations" | cut -c1-230 | tee -a "$REPRO_OUT/run.log" | head -12
sleep 6; look
DD=$(dist_xy "$(danse_at host)" "$(danse_at vm)")
rkeep vm after; rkeep host after
say "Danse's two positions $DD units apart; BoS101 host $(stage_of host $BOS101) vm $(stage_of vm $BOS101)"
rsince vm a | grep -E "Dialogue: stopping scene|started by a friend's stage|NpcSync: (talking to|not talking to|our dialogue menu)|the player is in scene" | cut -c1-230 | tee -a "$REPRO_OUT/run.log" | head -10

why=""
[ -n "$LAGGED" ] && why="$why; a quest the host moved on stayed behind in the friend's game for $LAG s or more:$LAGGED"
[ -n "$HOSTNO" ] && why="$why; the host's E on Danse (crosshair '$ROLLOVER') opened no dialogue menu in the station"
[ -n "$P1_BEHIND" ] && why="$why; after the host's talk in phase 1 these were behind:$P1_BEHIND"
[ "$FSCENE" -ge 2 ] && why="$why; the friend's player was in a scene with no conversation of its own for $FSCENE of $LOOKS looks (scenes$FSCENES)"
[ -n "$CLAIM" ] && why="$why; the friend's game claimed Danse ($CLAIM)"
[ "$FMENU" -ge 1 ] && why="$why; the friend's game opened a dialogue menu of its own ($FMENU of $LOOKS looks) though its player pressed nothing"
[ "$FOPEN" = no ] && [ "$HOSTE" = yes ] && why="$why; the friend's E on Danse opened no dialogue menu in $FRIEND_E s, the host's did"
[ -n "$DD" ] && [ "$DD" -ge 300 ] 2>/dev/null && why="$why; Danse stands $DD units apart in the two games"
[ -n "$why" ] && verdict YES "${why#; }"
verdict NO "the friend's game kept up on BoS100, the fight and BoS101, was in no scene, opened nothing of its own, and its E on Danse opened his dialogue ($LOOKS looks)"
