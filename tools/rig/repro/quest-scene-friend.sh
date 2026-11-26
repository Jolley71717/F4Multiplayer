#!/bin/bash
# quest-scene-friend.sh: a quest NPC's scene the host just played starts again in the friend's game, with the friend as its player
# (Nexus report 2026-11-25: "a quest NPC starts the same conversation the host just finished, this time with the friend"; left
# waiting, the NPC never goes where the quest needs it). A shared quest's stage comes to the friend's game and runs its fragment
# there, which starts the scene; the NPC is the host's (its copy runs no AI in the friend's game), so the scene cannot go on:
# the engine holds the friend in it (no weapons, Pip-Boy, workshop) until the host's game ends its own, and a friend who walks
# up to the NPC answers a conversation the host has had.
#
# Both games joined in Sanctuary, Codsworth the host's NPC (rig.sh join SanctuaryExt). The host's console sets
# DialogueCodsworthPostWar 50 (set-up, said): the stage his talk sets, which starts MQ102_CodsworthSearch01 (Codsworth and the
# player). Nothing else is done: both players stand where the join left them. The friend's game's player scene is read for HOLD s.
# A game whose session file already has the stage done sets nothing (the run is VOID): restore the backup and join again.
#
# REPRO: YES when the friend's player is still in the scene (HOLD s after the host's own scene began, or after the host's scene
# ended) or the friend's game claimed Codsworth; NO when the friend's game stopped it within 5 s and the host's scene played on.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
HOLD=${HOLD:-30}; QUEST=0001E64E; SCENE=0019E137
need_session
rmark host a; rmark vm a
[ "$(field "$(dc vm dialogue)" playerScene)" = 00000000 ] || verdict VOID "the friend's player is in a scene already"
say "set-up (console): host sets stage 50 of $QUEST"
dc host "console setstage $QUEST 50" >/dev/null
host_in_scene() { [ "$(field "$(dc host dialogue)" playerScene)" = "$SCENE" ]; }
scene_live "the host's scene began" 20 host_in_scene
FRIEND_IN=0; CLEARED=""
for i in $(seq 1 $((HOLD / 3))); do
	sleep 3
	h=$(field "$(dc host dialogue)" playerScene); v=$(field "$(dc vm dialogue)" playerScene)
	say "host in scene: $h; friend in scene: $v"
	[ "$v" = "$SCENE" ] && FRIEND_IN=$((FRIEND_IN + 1)) || { [ -z "$CLEARED" ] && CLEARED=$((i * 3)); }
done
rsince vm a | grep -E "started by a friend's stage|stopping scene" | cut -c1-200 | tee -a "$REPRO_OUT/run.log"
owner=$(field "$(dc vm "refinfo 0001CA7D")" owner)
if [ "$FRIEND_IN" -ge 2 ] || [ "$owner" = 2 ]; then verdict YES "the friend's player stayed in the host's scene for $((FRIEND_IN * 3)) s of $HOLD (Codsworth's owner: player $owner)"; fi
verdict NO "the friend's game let go of the scene within ${CLEARED:-0} s (Codsworth's owner: player $owner)"
