#!/bin/bash
# one-conversation.sh: both players can talk to the same NPC at once, each in a conversation of their own (a bug found by reading
# the code: NpcPlace::StaysWithUs gives our copy of an NPC its own AI whenever our player talks to it, whoever runs it elsewhere,
# so while one player is in a conversation with Codsworth the other presses E on their copy and gets a second, separate
# conversation; each game drops the other's lines of him, and what a line's fragments do can happen in both games).
#
# Both games joined in Sanctuary (rig.sh join SanctuaryExt). Codsworth is held where he stands (console SetRestrained, said) and
# both players walk to him with the pad (real movement). FIRST (host or vm) presses E on him, and stays in the menu. SECOND then
# presses E on their copy (RACE=1: both in the same moment, after both took aim). The conversations are read from both games'
# `net status` (talkingTo=) and menus (DialogueMenu open), and from the plugin logs ("talking to", "not talking to").
#
# REPRO: YES when both games are in a conversation with him at the end of WAIT s; NO when only one is (the first, or one of the
# two in a race), the other's menu closed and its player told. VOID when the first never got a conversation.
# FIRST=host|vm RACE=0|1 WAIT=8 bash tools/rig/repro/one-conversation.sh
cd "$(dirname "$0")" || exit 2
. ./lib.sh
CODS=0001CA7D
FIRST=${FIRST:-host}; SECOND=$(other_game "$FIRST"); RACE=${RACE:-0}; WAIT=${WAIT:-8}
need_session
rmark host a; rmark vm a
dc host "log level debug" >/dev/null; dc vm "log level debug" >/dev/null
log_back() { dc host "log level info" >/dev/null; dc vm "log level info" >/dev/null; }
on_end log_back
free_cods() { dc host "console $CODS.setrestrained 0" >/dev/null; }
on_end free_cods
# whoever's dialogue menu is still up at the end is closed
close_talks() {
	local g
	for g in host vm; do
		menu_open "$g" DialogueMenu || continue
		rkey "$g" "$VK_TAB" 100; sleep 1.5
		# Codsworth's own talk has no goodbye: the plugin's forced ending (dev `dialogue reset`) closes it, else the game's own reload of the cell does
		for n in 1 2 3; do menu_open "$g" DialogueMenu || break; dc "$g" "dialogue reset" >/dev/null; sleep 3; done   # the first also stops the scene, which can open the menu once more
		menu_open "$g" DialogueMenu && { say "end step: $g's dialogue menu is still up after 'dialogue reset': the cell is loaded again (console coc)"; dc "$g" "console coc SanctuaryExt" >/dev/null; sleep 3; wait_ingame "$g" 60; }
	done
	return 0
}
on_end close_talks

# press_on <who>: aim at Codsworth until the crosshair names him (up to 4 tries), then press E; VOID when it never does
aim_check() {
	local i
	for i in 1 2 3 4; do
		dc "$1" "turnto $CODS 300 aim" >/dev/null; sleep 1; ROLLOVER=$(rrollover "$1")
		[ "$ROLLOVER" = CODSWORTH ] && break
		say "$1's crosshair named '$ROLLOVER', not Codsworth (try $i)"
	done
	[ "$ROLLOVER" = CODSWORTH ] || verdict VOID "$1's crosshair never named Codsworth"
}
press_on() { aim_check "$1" && rkey "$1" "$VK_E" 100; }
talking_to() { field "$(dc "$1" 'net status')" talkingTo; }
in_menu() { menu_open "$1" DialogueMenu; }

say "set-up (console): Codsworth held where he stands (SetRestrained 1), in the host's game, whose NPC he is"
dc host "console $CODS.setrestrained 1" >/dev/null; sleep 2
ri=$(dc host "refinfo $CODS"); say "Codsworth: $(cut -c1-160 <<<"$ri")"
[ "$(field "$ri" dead)" = false ] || verdict VOID "Codsworth is not alive in the host's game"
for g in host vm; do in_menu "$g" && verdict VOID "$g's dialogue menu is open before the run"; done
# the host's player (FIRST's, the one who stands near him first) walks to him; the other walks to the far side of him, so the first's
# stand-in is not in the way of the second's aim (the second's crosshair named the first's stand-in on the first run)
say "$FIRST walks to Codsworth (the pad)"
rwalk "$FIRST" "$CODS" 200 || say "$FIRST's walk ended short ($WALKED)"
dc "$FIRST" "pad unplug" >/dev/null
fp=$(pos_of "$FIRST"); cp=$(field "$(dc "$FIRST" "refinfo $CODS")" x)" "$(field "$(dc "$FIRST" "refinfo $CODS")" y)
far=$(awk -v a="$fp" -v b="$cp" 'BEGIN{split(a,p," "); split(b,q," "); dx=q[1]-p[1]; dy=q[2]-p[2]; l=sqrt(dx*dx+dy*dy); if (l<1) {dx=1; dy=0; l=1} printf "%d %d", q[1]+dx/l*160, q[2]+dy/l*160}')
say "$SECOND walks to $far, the far side of Codsworth (the pad)"
# shellcheck disable=SC2086
rwalk "$SECOND" $far 80 || say "$SECOND's walk ended short ($WALKED)"
dc "$SECOND" "pad unplug" >/dev/null
say "owner of Codsworth before: host sees player $(field "$(dc host "refinfo $CODS")" owner), vm sees player $(field "$(dc vm "refinfo $CODS")" owner)"

aim() { dc "$1" "turnto $CODS 300 aim" >/dev/null; }
focus() { dc "$1" focus >/dev/null; }
if [ "$RACE" = 1 ]; then
	say "RACE: both players aim at Codsworth, then press E in the same moment"
	aim_check host; aim_check vm; focus host; focus vm; sleep 0.5
	( dc host "key $VK_E 100" >/dev/null & dc vm "key $VK_E 100" >/dev/null & wait )
	for i in 1 2; do say "host HUD: $(rhud host)"; say "vm HUD: $(rhud vm)"; done
else
	say "$FIRST aims at Codsworth and presses E (first)"
	press_on "$FIRST"; say "$FIRST's crosshair named '$ROLLOVER'"
	talk_live() { in_menu "$FIRST" && [ "$(talking_to "$FIRST")" = "$CODS" ]; }
	scene_live "$FIRST is in a conversation with Codsworth (menu open, talkingTo=$CODS)" 15 talk_live
	say "$SECOND aims at their copy of Codsworth and presses E (second)"
	rmark host b; rmark vm b
	press_on "$SECOND"; say "$SECOND's crosshair named '$ROLLOVER'"
	for i in 1 2 3; do say "$SECOND's HUD after the press: $(rhud "$SECOND")"; done
fi
sleep "$WAIT"
HM=$(in_menu host && echo yes || echo no); VMM=$(in_menu vm && echo yes || echo no)
HT=$(talking_to host); VT=$(talking_to vm)
say "after ${WAIT} s: host menu=$HM talkingTo=$HT; vm menu=$VMM talkingTo=$VT"
say "host's menus: $(menus host | cut -c1-100)"; say "vm's menus: $(menus vm | cut -c1-100)"
for g in host vm; do
	say "$g's log lines on talking:"; rsince "$g" a | grep -E "NpcSync: (talking to|not talking to|our dialogue menu|the conversation with)|Dialogue:" | cut -c1-230 | tee -a "$REPRO_OUT/run.log"
done
rkeep host after; rkeep vm after
if [ "$HM" = yes ] && [ "$VMM" = yes ] && [ "$HT" = "$CODS" ] && [ "$VT" = "$CODS" ]; then verdict YES "both games are in a conversation with Codsworth at once (talkingTo host $HT, vm $VT)"; fi
if [ "$HM" = no ] && [ "$VMM" = no ]; then verdict VOID "neither game has a conversation open (the first never started, or both ended)"; fi
verdict NO "one conversation: host menu=$HM talkingTo=$HT, vm menu=$VMM talkingTo=$VT"
