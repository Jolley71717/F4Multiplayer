#!/bin/bash
# friendbot.sh: the friend bot (dev `friendbot`, src/game/FriendBotPlan.h) drives the VM's player after the host's, on the
# engine's own AI (Game.SetPlayerAIDriven and a package made at run time): it follows, goes through a load door after the host,
# fights what fights the two of them, and waits when told. Nothing of the VM's player is moved by the rig: no setpos, no
# walkto, no keys. The host is walked by the pad (tools/rig/walkto.sh) and goes through a door with E, as in the other repros.
# Set-up, said so where it happens: both games to $CELL by console coc (an interior with a load door); a raider placed by dev
# spawnenemies in the host's game for the fight (made at runtime, shared by the session as an ambush's).
#   1. follow: the VM's bot on; the host walks 600 units away; the friend must come within 400 units of the host in 30 s.
#   2. door: the host goes through the nearest load door; the friend's cell must become the host's in 90 s.
#   3. fight: a raider 600 units from the host; the friend's log must say "FriendBot: fight" and its pad fire in 40 s.
#   4. wait: friendbot wait; the host walks 600 units off; the friend must stay within 150 units of where it stood.
#   5. off: friendbot off; the friend's status must say aiDriven=false.
# REPRO, as for a bug ("the bot cannot play the second player"): YES when a step failed (its step and the friend's FriendBot lines
# say how), NO when every step worked, VOID when the scene could not be made (no session, no load door, no raider).
# Not run yet: written with the bot, before any rig test.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
CELL=${CELL:-DiamondCityOrigin}
RAIDER=${RAIDER:-001AE38D}
run_cap 1500
need_session
on_end dc vm "friendbot off"

bot() { dc vm "friendbot $*"; }
dist_host_vm() { dist_xy "$(pos_of host)" "$(pos_of vm)"; }
fail() { rlog vm 300 | grep 'FriendBot:' | tail -12 | sed 's/^/vm log: /'; say "vm: $(bot status)"; verdict YES "$1"; }

say "set-up: both games to $CELL by console coc"
enter_both "$CELL" 16
bot status | grep -q 'friendbot=' || verdict VOID "the VM's plugin has no friendbot command (an older build)"

say "1. follow: the friend bot on in the VM; the host walks off (the pad)"
say "vm: $(bot on)"
sleep 3
H0=$(pos_of host)
read -r hx hy <<<"$H0"
rwalk host "$(( ${hx%.*} + 600 ))" "${hy%.*}" 80 || say "follow: the host's walk ended short ($WALKED)"
follow_close() { d=$(dist_host_vm); is_int "$d" && [ "$d" -lt 400 ]; }
wait_for 30 follow_close || fail "follow: the friend did not come within 400 units of the host in 30 s (now $(dist_host_vm))"
say "follow: the friend is $(dist_host_vm) units from the host"

say "2. door: the host goes through the nearest load door (walk and E)"
go_through host door || verdict VOID "door: no door near the host's player took them elsewhere"
HOST_CELL=$(cell_of host)
same_cell() { [ "$(cell_of vm)" = "$HOST_CELL" ]; }
wait_for 90 same_cell || fail "door: the friend was not in the host's cell $HOST_CELL 90 s after the host went through $WENT (the friend is in $(cell_of vm))"
say "door: the friend came through to $HOST_CELL"
wait_for 30 follow_close || say "door: the friend is $(dist_host_vm) units from the host 30 s after the door"

say "3. fight: a raider 600 units from the host (dev spawnenemies, set-up)"
rmark vm fight
R=$(dc host "spawnenemies $RAIDER 1 600" | grep -oE 'ids=[^ ]*' | cut -d= -f2 | cut -d, -f1)
[ -n "$R" ] || verdict VOID "fight: the host's game placed no raider (dev spawnenemies $RAIDER 1 600)"
fought() { rsince vm fight | grep -q 'FriendBot: fight' && [ "$(field "$(bot status)" fire)" -gt 0 ] 2>/dev/null; }
wait_for 40 fought || fail "fight: the friend's bot did not fight the raider $R in 40 s"
say "fight: $(rsince vm fight | grep 'FriendBot: fight' | head -1 | cut -c1-160); $(bot status | grep -oE 'aim=[0-9]+ fire=[0-9]+')"
dc host "console $R.kill" >/dev/null
sleep 5

say "4. wait: friendbot wait; the host walks off (the pad)"
say "vm: $(bot wait | cut -c1-120)"
sleep 2
V0=$(pos_of vm)
read -r hx hy <<<"$(pos_of host)"
rwalk host "$(( ${hx%.*} - 600 ))" "${hy%.*}" 80 || say "wait: the host's walk ended short ($WALKED)"
sleep 5
moved=$(dist_xy "$V0" "$(pos_of vm)")
is_int "$moved" && [ "$moved" -le 150 ] || fail "wait: the friend moved $moved units while told to wait"
say "wait: the friend stayed ($moved units)"
say "vm: $(bot follow | cut -c1-120)"
wait_for 30 follow_close || fail "wait: after follow the friend did not come back within 400 units in 30 s"

say "5. off"
bot off >/dev/null
sleep 2
grep -q 'aiDriven=false' <<<"$(bot status)" || fail "off: the friend's player is still AI-driven"
verdict NO "the friend bot followed, came through the door, fought, waited and let go"
