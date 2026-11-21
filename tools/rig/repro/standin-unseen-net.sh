#!/bin/bash
# standin-unseen-net.sh [watchers]: what is done for a friend who is near us and not shown (src/game/StandInDraw.h). A player's
# report (0.20.1) had a friend on the compass, acting on the world, and not drawn until the game was restarted; how a stand-in gets
# so is not known, so this run makes one unseen on purpose and watches the steps: dev `standindraw once` culls the 3D of
# the watcher's stand-in of the other player once (said so: the cull is ours, by the dev channel; nothing else is touched), and
# the watcher is expected to say it, ask the engine to build the 3D again and say "shown again"; then `standindraw hold` culls
# every stand-in it has every frame, and it is expected to place a fresh one, then a plain one (the default settler, without their
# face and body data), then leave it and say so; `standindraw off` then places normal stand-ins again, which must be drawn.
# Each of the watchers (default "host vm") in turn, with the other player standing within 2000 units (as a join leaves them).
# REPRO: YES when a culled stand-in stays unseen (no rebuild, or still culled after the steps, or the stand-in after `off` is not
# drawn); NO when every step came and the friend is shown again; VOID on a build without `standindraw` (0.21.0 and before: there
# a culled stand-in has nothing looking at it at all), or when the two could not be put near each other.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
WATCHERS=${1:-host vm}
need_session
standin_of() { dc "$1" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2; }
dist_players() {
	local h v; h=$(dc host status); v=$(dc vm status)
	[ "$(field "$h" cell)" = "$(field "$v" cell)" ] || [ "$(field "$h" worldspace)" = "$(field "$v" worldspace)" ] || { echo ""; return; }
	dist_xy "$(pos_of host)" "$(pos_of vm)"
}
# has <who> <tag> <pattern>: the watcher's log since the mark has a line matching
has() { rsince "$1" "$2" | grep -qE "$3"; }
# drawn <who>: its stand-in of the other is there, with a 3D that is not culled
drawn() { local p st; p=$(standin_of "$1"); [ -n "$p" ] || return 1; st=$(dc "$1" "standin $p"); case "$st" in *" 3d='"*) ;; *) return 1;; esac; [ "$(field "$st" culled)" = false ]; }

for w in host vm; do case "$(dc "$w" standindraw)" in standindraw:*) ;; *) verdict VOID "the $w's build has no dev standindraw (0.21.0 or before): nothing there looks at a stand-in that is not drawn";; esac; done
d=$(dist_players)
[ -n "$d" ] && [ "$d" -le 2000 ] || verdict VOID "the two players are not near each other (${d:-other spaces})"
say "standin-unseen-net: watchers '$WATCHERS'; the players are $d u apart"

BAD=""
for w in $WATCHERS; do
	wait_for 20 drawn "$w" || verdict VOID "the $w has no drawn stand-in of the other player to start from: $(dc "$w" "standin $(standin_of "$w")")"
	p0=$(standin_of "$w"); say "[$w] stand-in $p0 drawn: $(dc "$w" "standin $p0" | cut -c1-200)"
	say "[$w] before: $(dc "$w" standindraw)"

	# 1) culled once: said, built again, shown again
	rmark "$w" once; dc "$w" 'standindraw once' >/dev/null
	wait_for 20 has "$w" once 'is near us and not shown: its 3D is culled; asked the engine to build its 3D again' || BAD="$BAD [$w] culled once: no rebuild was asked within 20 s;"
	wait_for 25 has "$w" once 'is shown again, after' || BAD="$BAD [$w] culled once: not shown again within 25 s of the rebuild;"
	rsince "$w" once | grep -E 'near us and|shown again|spawned' | sed "s/^/  [$w] /" | tee -a "$REPRO_OUT/run.log"
	drawn "$w" && say "[$w] culled once: drawn again ($(standin_of "$w"))" || BAD="$BAD [$w] culled once: the stand-in is not drawn after the steps;"

	# 2) culled every frame: a fresh one, a plain one, then left
	rmark "$w" hold; dc "$w" 'standindraw hold' >/dev/null
	wait_for 30 has "$w" hold 'still not shown: .*; placed again \(' || BAD="$BAD [$w] culled every frame: no fresh stand-in within 30 s;"
	wait_for 30 has "$w" hold 'placed again plain' || BAD="$BAD [$w] culled every frame: no plain stand-in within 30 s more;"
	wait_for 15 has "$w" hold 'spawned .*\(plain' || BAD="$BAD [$w] culled every frame: the plain stand-in was not placed;"
	wait_for 30 has "$w" hold 'left as it is' || BAD="$BAD [$w] culled every frame: never left alone (said) within 30 s more;"
	rsince "$w" hold | grep -E 'near us and|shown again|spawned' | sed "s/^/  [$w] /" | tee -a "$REPRO_OUT/run.log"
	n=$(rsince "$w" hold | grep -cE 'spawned .* for player'); say "[$w] culled every frame: $n stand-in(s) placed in all (2 expected: a fresh one, a plain one)"
	[ "$n" -le 3 ] || BAD="$BAD [$w] culled every frame: $n stand-ins placed, more than the steps allow;"

	# 3) off: normal stand-ins again, drawn
	rmark "$w" off; dc "$w" 'standindraw off' >/dev/null
	wait_for 25 drawn "$w" || BAD="$BAD [$w] after off: the stand-in is not drawn;"
	rsince "$w" off | grep -qE 'spawned .*\(plain' && BAD="$BAD [$w] after off: the new stand-in is still a plain one;"
	say "[$w] after off: $(standin_of "$w") $(dc "$w" "standin $(standin_of "$w")" | cut -c1-160)"
	say "[$w] after: $(dc "$w" standindraw)"
	rkeep "$w" unseen-net
done
[ -n "$BAD" ] && verdict YES "a stand-in culled on purpose was not brought back as the steps say:$BAD"
verdict NO "each watcher ($WATCHERS) said a culled stand-in, built its 3D again and showed it; culled every frame, it placed a fresh one, then a plain one, then left it and said so; normal ones stand again after"
