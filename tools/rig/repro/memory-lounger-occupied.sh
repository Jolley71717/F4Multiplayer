#!/bin/bash
# memory-lounger-occupied.sh [who]: a friend presses E on Kellogg's memory lounger (Dangerous Minds, the Memory Den) while the
# other player is on their way into the memory from it, their stand-in in that lounger in the friend's game. MQ202 155 blocks the
# lounger's own activation, but its alias (PlayerMemoryLounger, DefaultAliasOnActivate) still sets MQ202 160 on a press. 160's
# fragment takes the controls (MQ202EnableLayer: movement, fighting, the Pip-Boy, activation) and then seats the player itself
# (lounger.Activate(player, True)); with the lounger taken that seat fails, MQ202PlayerScript.OnSit never sets 170, and the player
# is left with no controls and no Pip-Boy, kept in any save made then (found by review; the stand-in never got into this lounger in
# the rig runs of 2026-11-16, so whether it is reached in play depends on that). <who> (vm, the default, or host) is the friend who
# presses; the other player (the sitter) goes into the memory by their own press on the lounger.
# Set-up, said so where it happens:
#   - both games are put in the Memory Den by console coc GoodneighborTheMemoryDen (placement; a fresh coc character each);
#   - the story is brought to the lounger by console setstage 000229E9 155 in the sitter's game (Amari's talk done: 155 blocks the
#     lounger's activation and shows its objective); the friend's game catches it up through the session;
#   - the lounger, disabled in a coc'd character's game (the den's earlier story enables it), is enabled by console where it is not;
#   - both players are put beside the lounger by the dev channel's setpos (placement: the den's stairs stop the pad's walk), the
#     friend in front of it, the sitter at its side; the sitter presses E (real); if that does not seat them, console setstage
#     000229E9 160 (said so);
#   - FORCE=1: the sitter's stand-in in <who>'s game is sent into the lounger by the dev channel's `seat <stand-in> <lounger> snap
#     ai` (and `activate ai`) when it is not in it by itself: the state the bug needs, which the furniture's own ways have not
#     reached on the rig. Without FORCE the stand-in is left to the mod's own seat tries. (2026-11-21, 0.21.0: neither the mod's
#     six tries nor the four dev ways ever got the stand-in into this lounger, "no furniture class".)
#   - SEATLESS=1: no sitter and no press: <who>'s lounger is disabled by console and <who>'s 160 set by console, so 160 takes the
#     controls with no seat to be had: the lockout itself, whatever keeps the seat from happening (the occupied lounger is one).
# Then <who> looks at the lounger (dev turnto), presses E on it (real) and is watched for WAIT s (default 30): MQ202's 160 and 170
# in its game, its player's furniture state (paframes' "our player": sitState, occupied), its input layers (`controls`), and a
# real Tab for the Pip-Boy.
# REPRO: YES when <who>'s 160 is done, its player is in no furniture and 170 is not done WAIT s after the press, its controls
# taken (a layer turns them off, or the Pip-Boy will not open); NO when the press left it its controls (160 not set, or given
# back), or seated it (it went into its own memory: a separate, filed matter); VOID when the scene was not reached (no sitter on
# the lounger, the friend's 155 not caught up, an earlier run's 160 done).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WHO=${1:-vm}; SITTER=$(other_game "$WHO"); WAIT=${WAIT:-30}; FORCE=${FORCE:-0}; SEATLESS=${SEATLESS:-0}
MQ202=000229E9; MQ203=000229EA; LOUNGER=${LOUNGER:-000D4C66}; DEN=GoodneighborTheMemoryDen
VK_TAB=0x09
[ "$WHO" = host ] || [ "$WHO" = vm ] || verdict VOID "usage: memory-lounger-occupied.sh [host|vm]"
need_session

stage_done() { dc "$1" "stages $2" | grep -oE 'done\([0-9]+\)=[0-9,none]+' | sed 's/^[^=]*=//' | tr ',' '\n' | grep -qx "$3"; }
use_of() { dc "$1" "paframes 100" | grep -oE 'our player: sitState=[0-9-]+ occupied=[0-9A-F]+' | sed 's/^our player: //'; }
seated() { local u; u=$(use_of "$1"); [ "$(field "$u" sitState)" = 3 ] || [ "$(field "$u" occupied)" = "$LOUNGER" ]; }
layers() { dc "$1" controls | grep -oE 'layers=[0-9]+[^|]*' | sed 's/ *$//'; }
standin_use() { dc "$WHO" "anim $PUPPET" | grep -oE 'sit=[0-9-]+ furniture=[0-9A-F]+'; }
standin_in() { [ "$(field "$(standin_use)" furniture)" = "$LOUNGER" ]; }
facts() { echo "MQ202 done=$(dc "$1" "stages $MQ202" | grep -oE 'done\([0-9]+\)=[0-9,none]+' | sed 's/^[^=]*=//'), MQ203 10 $(stage_done "$1" $MQ203 10 && echo done || echo not), $(use_of "$1"), $(layers "$1"), menus $(menus "$1")"; }

# ---- the Memory Den, the story at the lounger -----------------------------------------------------------------------------------
for w in host vm; do
	if [ "$(field "$(dc $w status)" cell)" != 00022884 ]; then
		say "set-up: $w to the Memory Den (console coc $DEN)"; dc $w focus >/dev/null; dc $w "console coc $DEN" >/dev/null
	fi
done
sleep 8; wait_ingame host 60; wait_ingame vm 60
for w in host vm; do stage_done $w $MQ202 160 && verdict VOID "$w's MQ202 160 is done already (an earlier run): a fresh game (rig.sh join $DEN)"; done
if ! stage_done "$SITTER" $MQ202 155; then
	say "set-up: $SITTER's story at the lounger (console setstage $MQ202 155)"; dc "$SITTER" "console setstage $MQ202 155" >/dev/null
fi
wait_for 30 stage_done "$WHO" $MQ202 155 || verdict VOID "$WHO's game did not catch up MQ202 155 in 30 s ($(dc "$WHO" "stages $MQ202"))"
say "before: $SITTER: $(facts "$SITTER")"
say "before: $WHO: $(facts "$WHO")"
PUPPET=$(dc "$WHO" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
[ -n "$PUPPET" ] || verdict VOID "$WHO's game has no stand-in for the $SITTER's player (no puppet= in its net status)"
# The lounger is enabled by the Memory Den's earlier story (a coc'd character never had it): enabled by console where it is not.
for w in host vm; do
	if [ "$(field "$(dc $w "refinfo $LOUNGER")" disabled)" = true ]; then
		say "set-up: $w's lounger $LOUNGER is disabled (a coc'd character never had the den's earlier story): console $LOUNGER.enable"
		dc $w "console $LOUNGER.enable" >/dev/null; sleep 1
	fi
done
rmark host; rmark vm

if [ "$SEATLESS" = 1 ]; then
	# ---- SEATLESS=1: 160 with no seat to be had, in <who>'s own game -------------------------------------------------------------
	# The lounger is disabled by console in <who>'s game and its 160 is set by console (both set-up, said so): the fragment takes the
	# controls and its own seat cannot happen, the lockout the occupied lounger would give, reached without the other player.
	say "set-up (SEATLESS): $WHO's lounger disabled by console ($LOUNGER.disable), then console setstage $MQ202 160 in $WHO's game"
	dc "$WHO" "console $LOUNGER.disable" >/dev/null; sleep 1
	rmark "$WHO" press
	dc "$WHO" "console setstage $MQ202 160" >/dev/null; TP=$SECONDS
	ROLLOVER="(none: console 160)"; STANDIN_IN="n/a"
else
# ---- the friend beside the lounger, the sitter onto it --------------------------------------------------------------------------
# Placement (console-like, said so): the den's stairs stop the pad's walk, so both are put beside the lounger by the dev channel's
# setpos, the friend in front of it, the sitter at its side. The sitter presses E (real); 160's own seat takes them from there.
L=$(dc "$WHO" "refinfo $LOUNGER"); LX=$(field "$L" x); LY=$(field "$L" y); LZ=$(field "$L" z); LR=$(field "$L" rz)
spot() { awk -v x="$LX" -v y="$LY" -v r="$LR" -v d="$1" -v a="$2" 'BEGIN { t = (r + a) * 3.14159265 / 180; printf "%d %d", x + d * sin(t), y + d * cos(t) }'; }
say "set-up: the lounger at ($LX, $LY, $LZ) heading $LR; $WHO placed at $(spot 100 0) and $SITTER at $(spot 110 90) (dev setpos)"
dc "$WHO" "setpos 00000014 $(spot 100 0) $LZ" >/dev/null; dc "$SITTER" "setpos 00000014 $(spot 110 90) $LZ" >/dev/null; sleep 2
rpress_e "$SITTER" "$LOUNGER"; say "$SITTER presses E on the lounger (real; the crosshair named '$ROLLOVER')"
if ! wait_for 8 seated "$SITTER"; then
	say "$SITTER not seated by the E ($(use_of "$SITTER")): set-up: console setstage $MQ202 160 in $SITTER's game"
	dc "$SITTER" "console setstage $MQ202 160" >/dev/null
	wait_for 10 seated "$SITTER" || verdict VOID "$SITTER never sat in the lounger ($(facts "$SITTER"))"
fi
TS=$SECONDS; say "$SITTER on the lounger: $(use_of "$SITTER")"
# The window is short (the lounger's scene, then MQ203 10 moves the sitter into the memory, about a minute): the stand-in's own
# tries get a few seconds (each look is a dev call of a few seconds itself), then FORCE.
for i in 1 2; do standin_in && break; sleep 1; done
say "$WHO: the $SITTER's stand-in $PUPPET in the lounger: $(standin_in && echo yes || echo no)"
if ! standin_in && [ "$FORCE" = 1 ]; then
	for way in "snap ai" "activate ai" "snap" "activate"; do
		say "set-up: the stand-in sent into the lounger by the dev channel (seat $PUPPET $LOUNGER $way): $(dc "$WHO" "seat $PUPPET $LOUNGER $way" | cut -c1-120)"
		wait_for 2 standin_in && break
	done
fi
STANDIN_IN=$(standin_in && echo yes || echo no)
say "$WHO: the stand-in at the press: $(standin_use) (in the lounger: $STANDIN_IN), +$((SECONDS - TS)) s after the sitter sat"

# ---- the friend's press ---------------------------------------------------------------------------------------------------------
dc "$WHO" "turnto $LOUNGER 300 aim" >/dev/null; sleep 0.6
rmark "$WHO" press
rkey "$WHO" "$VK_E" 100; TP=$SECONDS
ROLLOVER=$(rrollover "$WHO")
say "$WHO presses E on the lounger (real, the look at its middle; the crosshair named '$ROLLOVER' just after; the stand-in in it: $STANDIN_IN)"
stage_done "$SITTER" $MQ203 10 && say "$SITTER is on the way into the memory already (MQ203 10 done)"
fi
LAST=""; SEATED_AT=""
while [ $((SECONDS - TP)) -lt "$WAIT" ]; do
	now="$(use_of "$WHO"); 160 $(stage_done "$WHO" $MQ202 160 && echo done || echo not); 170 $(stage_done "$WHO" $MQ202 170 && echo done || echo not)"
	[ "$now" != "$LAST" ] && { say "$WHO at +$((SECONDS - TP)) s: $now; $(layers "$WHO")"; LAST=$now; }
	seated "$WHO" && SEATED_AT=${SEATED_AT:-$((SECONDS - TP))}
	sleep 1
done
END="$(facts "$WHO")"
say "$WHO at +$((SECONDS - TP)) s: $END"
say "$WHO: the stand-in now: $(standin_use); the guard (dev lounger, newer builds): $(dc "$WHO" lounger | cut -c1-200)"
# the Pip-Boy: a real Tab, then Tab again to shut it if it opened
rkey "$WHO" $VK_TAB; sleep 2.5
if menu_open "$WHO" PipboyMenu; then PIPBOY=opens; rkey "$WHO" $VK_TAB; sleep 1.5; else PIPBOY="does not open"; fi
say "$WHO: the Pip-Boy on a real Tab: $PIPBOY ($(menus "$WHO"))"
say "$WHO's log since the press: $(rsince "$WHO" press | grep -E 'QuestSync|RemotePlayers|Puppets|MemoryLounger|LoungerWatch|seat' | cut -c16-220 | tail -14 | paste -sd'|' -)"
rkeep host memory-lounger-occupied; rkeep vm memory-lounger-occupied

D160=$(stage_done "$WHO" $MQ202 160 && echo 1 || echo 0); D170=$(stage_done "$WHO" $MQ202 170 && echo 1 || echo 0)
if [ "$D160" = 1 ] && [ "$D170" = 0 ] && ! seated "$WHO" && { [ "$PIPBOY" != opens ] || [ -n "$(layers "$WHO" | grep -oE "'[^']*'")" ]; }; then
	verdict YES "$WHO pressed the lounger (the $SITTER's stand-in in it: $STANDIN_IN; the crosshair named '$ROLLOVER'): MQ202 160 done, 170 not, not seated, Pip-Boy $PIPBOY, $WAIT s on ($END)"
fi
if [ "$D160" = 1 ] && { [ "$D170" = 1 ] || [ -n "$SEATED_AT" ]; }; then
	verdict NO "$WHO pressed the lounger (the stand-in in it: $STANDIN_IN; the crosshair named '$ROLLOVER') and was seated (at +${SEATED_AT:-?} s; 170 $([ "$D170" = 1 ] && echo done || echo not)): into its own memory, the controls the memory's ($END)"
fi
verdict NO "$WHO pressed the lounger (the stand-in in it: $STANDIN_IN; the crosshair named '$ROLLOVER') and keeps its controls: 160 $([ "$D160" = 1 ] && echo done || echo not), Pip-Boy $PIPBOY ($END)"
