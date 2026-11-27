#!/bin/bash
# cabot-friend.sh: the host talks to Edward Deegan in Goodneighbor's Third Rail (where the Cabot House questline begins) with the
# friend standing near; the friend's game must not be pulled into the host's conversation (Nexus report on 0.22.1: "quest NPCs
# (Edward, then Jack Cabot, then every NPC after) engaged the friend in the same conversation the host had just finished, without
# forcing it; if the friend avoided them, the NPC was stuck between going where the quest needed and trying to talk to the friend,
# and the quest broke for both"). 0.23.0's fix for that class (ForeignScene.h, Dialogue.cpp StopForeignScene): a scene of a shared
# quest that holds our player, whose NPCs another game runs, with no conversation of ours open, is stopped after 1.5 s.
#
# Both games joined (rig.sh join) and both players in the Third Rail (the script console-places them there: `coc
# GoodneighborTheThirdRail`, said). Edward Deegan (00047EC1, base 00047EC0) is the quest NPC of MS09Intro (00047E90, the story
# manager's "a man at a bar offers you work" quest) and the giver of The Secret of Cabot House (MS09, 00022A00). The story
# manager would start MS09Intro as the player enters a bar; the rig does what it does (dev `radiantstart 00047E90 - <Third
# Rail's location>` in the host's game, said): Edward is then in the bar, run by the host's game. Both games' MS09 and MS09Intro are
# reset first (console resetquest, said) so the run starts from a game that never met him; a game whose Edward is not in the
# Third Rail after that is VOID.
# Then, with real input: both players are put beside Edward by the console (player.moveto, said) and walk to where they stand
# (the pad: the host to within 130 units of him, the friend to within 250: nearby, in his sight), the host aims at him and
# presses E, and answers through the whole conversation with the keys (the dialogue wheel's bottom reply, Down then Enter, each
# time it takes input: "I'm listening", then "Sign me up"), until the menu closes. The friend's player does nothing.
#
# What is read: from the host's E to HOLD s after its conversation ended, every 3 s, the friend's player scene and dialogue menu
# (dev `dialogue`, `menus`), its `talkingTo`; the friend's log for "Dialogue: stopping scene" (the 0.23.0 fix), "started by a friend's
# stage" and any conversation of the friend's own with Edward; at the end both games' MS09Intro and MS09 stages and who runs Edward.
# REPRO: YES when the friend's player is in a scene with no conversation of its own open for two looks or more, or its game opened
# a conversation with Edward (the host's, again), or the friend's game claimed Edward (owner = the friend's player), or a quest the
# host has started (a displayed objective, so one the session shares) is at an earlier stage in the friend's game once the host's
# conversation is over; NO when none of these (the friend's game stopped any scene it was put in: the log says "Dialogue: stopping
# scene ..."; or it was never put in one). VOID when Edward is not in the Third Rail, the host's conversation never opened or did
# not end in HOLD_TALK s, or a player is in a scene before the run.
# Lines said are kept in $REPRO_OUT/run.log. HOLD=30 HOLD_TALK=120 bash tools/rig/repro/cabot-friend.sh
cd "$(dirname "$0")" || exit 2
. ./lib.sh
EDWARD=00047EC1; INTRO=00047E90; MS09=00022A00; CELL=GoodneighborTheThirdRail; THIRD_RAIL_CELL=0002263E; THIRD_RAIL=0002267F
HOLD=${HOLD:-30}; HOLD_TALK=${HOLD_TALK:-120}
run_cap 900
need_session
rmark host a; rmark vm a
dc host "log level debug" >/dev/null; dc vm "log level debug" >/dev/null
log_back() { dc host "log level info" >/dev/null; dc vm "log level info" >/dev/null; }
on_end log_back
# whoever's dialogue menu is still up at the end is closed (the dev channel's forced ending)
close_talks() {
	local g n
	for g in host vm; do
		menu_open "$g" DialogueMenu || continue
		for n in 1 2 3; do dc "$g" "dialogue reset" >/dev/null; sleep 3; menu_open "$g" DialogueMenu || break; done
		menu_open "$g" DialogueMenu && { say "end step: $g's dialogue menu is still up after 'dialogue reset': its cell is loaded again (console coc)"; dc "$g" "console coc $CELL" >/dev/null; sleep 3; wait_ingame "$g" 60; }
	done
	return 0
}
on_end close_talks

scene_of() { field "$(dc "$1" dialogue)" playerScene; }
stage_of() { field "$(dc "$1" "stages $2")" stage; }
in_cell() { [ "$(cell_of "$1")" = "$THIRD_RAIL_CELL" ]; }
# started_quest <who> <quest>: 0 when that game shows an objective of the quest (what the session counts as the player having
# started it: its stages are shared from then on)
started_quest() { dc "$1" "objectives $2" | tr ';' '\n' | grep "state=" | grep -qvE "state=0 "; }

say "set-up (console): both players to the Third Rail (coc $CELL); both games' MS09 and MS09Intro reset; the host starts MS09Intro in the Third Rail as the story manager would (radiantstart)"
for g in host vm; do in_cell "$g" || { dc "$g" "console coc $CELL" >/dev/null; sleep 12; wait_ingame "$g" 60; }; done
sleep 4
for g in host vm; do in_cell "$g" || verdict VOID "$g is not in the Third Rail (cell $(cell_of "$g")) after the console's coc"; done
for q in $MS09 $INTRO; do dc host "console resetquest $q" >/dev/null; dc vm "console resetquest $q" >/dev/null; done
sleep 2
say "radiantstart: $(dc host "radiantstart $INTRO - $THIRD_RAIL" | cut -c1-120)"
sleep 4
ri=$(dc host "refinfo $EDWARD")
[ "$(field "$ri" cell)" = "$THIRD_RAIL_CELL" ] && [ "$(field "$ri" 3d)" = true ] || verdict VOID "Edward is not in the Third Rail in the host's game ($(cut -c1-120 <<<"$ri"))"
for g in host vm; do
	[ "$(scene_of "$g")" = 00000000 ] || verdict VOID "$g's player is in a scene before the run"
	menu_open "$g" DialogueMenu && verdict VOID "$g's dialogue menu is open before the run"
done
say "Edward: host sees $(cut -c1-100 <<<"$ri"); owner before: host sees player $(field "$ri" owner), vm sees player $(field "$(dc vm "refinfo $EDWARD")" owner)"
say "quests before: MS09Intro host $(stage_of host $INTRO) vm $(stage_of vm $INTRO); MS09 host $(stage_of host $MS09) vm $(stage_of vm $MS09)"

# both players beside Edward by the console (placement, said), then each walks to where they stand: the friend behind the host
say "set-up (console): both players placed at Edward (player.moveto), then walked (the pad) to where they stand"
for g in host vm; do dc "$g" "console player.moveto $EDWARD" >/dev/null; done
sleep 4
step_aside host $EDWARD 300 || say "host's step aside ended short ($WALKED)"
step_aside vm $EDWARD 450 || say "vm's step aside ended short ($WALKED)"
rwalk vm "$EDWARD" 250 || say "vm's walk ended short ($WALKED)"
rwalk host "$EDWARD" 130 || say "host's walk ended short ($WALKED)"
say "distances to Edward: host $(dist_xy "$(pos_of host)" "$(field "$(dc host "refinfo $EDWARD")" x) $(field "$(dc host "refinfo $EDWARD")" y)"), vm $(dist_xy "$(pos_of vm)" "$(field "$(dc vm "refinfo $EDWARD")" x) $(field "$(dc vm "refinfo $EDWARD")" y)")"

# the friend's samples: how many looks it was in a scene with no conversation of its own open, with its own conversation open
FRIEND_SCENE=0; FRIEND_MENU=0; LOOKS=0; FRIEND_SCENES=""
look() {
	local s m t; s=$(scene_of vm); menu_open vm DialogueMenu && m=1 || m=0; t=$(field "$(dc vm 'net status')" talkingTo)
	LOOKS=$((LOOKS + 1))
	[ "$m" = 1 ] && FRIEND_MENU=$((FRIEND_MENU + 1))
	if [ "$s" != 00000000 ] && [ "$m" = 0 ]; then FRIEND_SCENE=$((FRIEND_SCENE + 1)); FRIEND_SCENES="$FRIEND_SCENES $s"; fi
	say "look $LOOKS: host scene $(scene_of host), menu $(menu_open host DialogueMenu && echo open || echo closed); friend scene $s, menu $([ "$m" = 1 ] && echo open || echo closed), talkingTo $t"
}

say "the host aims at Edward and presses E"
dc host "turnto $EDWARD 300 aim" >/dev/null; sleep 1
say "the host's crosshair names '$(rrollover host)'"
rkey host "$VK_E" 100
host_talks() { menu_open host DialogueMenu; }
scene_live "the host's conversation with Edward opened" 15 host_talks
# the wheel takes no input until its replies show; the bottom reply (Down, Enter) carries Edward's talk on
allow() { [ "$(field "$(dc host dialogue)" allowInput)" = true ]; }
T0=$SECONDS; PICKS=0
while menu_open host DialogueMenu; do
	[ $((SECONDS - T0)) -ge "$HOLD_TALK" ] && verdict VOID "the host's conversation was still open after $HOLD_TALK s (the replies: $PICKS chosen)"
	look
	if wait_for 8 allow; then
		opts=$(rgfx_text host DialogueMenu root.Menu_mc.FadeHolder_mc 4 textField_tf | awk '{ $1 = ""; print substr($0, 2) }' | sort -u | paste -sd'|' -)
		[ -n "$(tr -d '| ' <<<"$opts")" ] || { sleep 2; continue; }
		PICKS=$((PICKS + 1)); say "reply $PICKS: the wheel offers: $opts; the host takes the bottom one (Down, Enter)"
		rkey host "$VK_DOWN" 100; sleep 0.6; rkey host "$VK_ENTER" 100; sleep 6
	else sleep 2; fi
done
say "the host's conversation ended after $((SECONDS - T0)) s ($PICKS replies); the friend is watched for $HOLD s more"
rsince host a | grep "Conversations:.*says" | grep -v "Sound.Voice" | sed 's/.*\] \[D\] //' | cut -c1-160 | tee -a "$REPRO_OUT/run.log" | head -20
for i in $(seq 1 $((HOLD / 3))); do sleep 3; look; done

# ---- what happened ----------------------------------------------------------------------------------------------------------------
sleep 2
rkeep vm after; rkeep host after
STOPS=$(rsince vm a | grep -c "Dialogue: stopping scene")
rsince vm a | grep -E "Dialogue: stopping scene|started by a friend's stage|NpcSync: (talking to|our dialogue menu)|the player is in scene" | cut -c1-230 | tee -a "$REPRO_OUT/run.log" | head -10
owner=$(field "$(dc host "refinfo $EDWARD")" owner)
say "after: MS09Intro host $(stage_of host $INTRO) vm $(stage_of vm $INTRO); MS09 host $(stage_of host $MS09) vm $(stage_of vm $MS09); Edward's owner (host sees) player $owner; the friend's game stopped $STOPS scene(s)"
[ "$(stage_of host $INTRO)" -ge 100 ] 2>/dev/null || verdict VOID "the host's conversation did not move MS09Intro on (stage $(stage_of host $INTRO)): it did not go to its end"
# a quest the host has started (an objective shown) must be as far in the friend's game: the session shares it
BEHIND=""
for q in $MS09 $INTRO; do
	if started_quest host $q && [ "$(stage_of vm $q)" -lt "$(stage_of host $q)" ] 2>/dev/null; then BEHIND="$BEHIND $q(host $(stage_of host $q), friend $(stage_of vm $q))"; fi
done
say "quests the host has started that are behind in the friend's game:${BEHIND:- none} (a quest with no objective shown yet is not shared: MS09 shows its first at the Cabot House, not here)"
if [ "$FRIEND_SCENE" -ge 2 ]; then verdict YES "the friend's player was in a scene with no conversation of its own open for $FRIEND_SCENE of $LOOKS looks (scenes$FRIEND_SCENES); the game stopped $STOPS"; fi
if [ "$owner" = 2 ]; then verdict YES "the friend's game claimed Edward (owner: player $owner)"; fi
if [ -n "$BEHIND" ]; then verdict YES "a quest the host started is behind in the friend's game:$BEHIND"; fi
if [ "$FRIEND_MENU" -ge 1 ]; then verdict YES "the friend's game opened a dialogue menu of its own ($FRIEND_MENU of $LOOKS looks) though its player pressed nothing"; fi
verdict NO "the friend was in no scene (or the game stopped it: $STOPS stop(s)) and opened no conversation ($LOOKS looks), Edward stayed the host's, and the host's quests agree"
