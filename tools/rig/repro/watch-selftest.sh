#!/bin/bash
# watch-selftest.sh: lib.sh's scene gate, idle watch, time cap and end steps, against fake plugin logs. Touches no game and no VM
# (dc is replaced by a stub that only writes down what it was asked; REPRO_LOG_HOST / REPRO_LOG_VM point the log reads at files
# here). Each case is a small script that sources lib.sh, run on its own; prints PASS/FAIL a case, exits with the failures.
#   bash tools/rig/repro/watch-selftest.sh
LIB=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh
T=$(mktemp -d); export T
trap 'rm -rf "${T:?}"' EXIT
mkdir -p "$T/stage" "$T/checks"
export F4MP_STAGE=$T/stage F4MP_CHECKS=$T/checks VM_SSH=selftest-no-vm REPRO_POLL=1
export REPRO_LOG_HOST=$T/host.log REPRO_LOG_VM=$T/vm.log
FAILS=0
ok() { echo "PASS $1"; }
no() { echo "FAIL $1: $2"; FAILS=$((FAILS + 1)); }
stamp() { printf '[%(%H:%M:%S)T.%03d] [1] [I] %s\n' -1 $((RANDOM % 1000)) "$1"; }
fresh_logs() {
	: > "$REPRO_LOG_HOST"; : > "$REPRO_LOG_VM"
	for i in 1 2 3; do stamp "Session: host line $i" >> "$REPRO_LOG_HOST"; stamp "Session: vm line $i" >> "$REPRO_LOG_VM"; done
	stamp "DevCommands: gfx: root.Menu_mc noise" >> "$REPRO_LOG_HOST"
}
# run <name> <body> [env...]: the body as a script after lib.sh; RC, OUT (its output), DT (its seconds), CALLS (the stub's dc calls)
run() {
	local name=$1 body=$2 t0; shift 2
	cat > "$T/$name.sh" <<EOF
#!/bin/bash
. "$LIB"
dc() { echo "dc \$*" >> "$T/$name.calls"; }
on_end eval 'echo end >> "$T/$name.ends"'
$body
EOF
	: > "$T/$name.calls"; : > "$T/$name.ends"
	t0=$SECONDS
	env REPRO_OUT="$T/out-$name" "$@" bash "$T/$name.sh" > "$T/$name.out" 2>&1; RC=$?
	DT=$((SECONDS - t0)); OUT=$(cat "$T/$name.out"); CALLS=$(cat "$T/$name.calls"); ENDS=$(grep -c end "$T/$name.ends")
}
# writer <file> <line> <n>: that line added to the file once a second, n times (a game logging as it plays)
writer() { local i; for i in $(seq 1 "$3"); do sleep 1; stamp "$2" >> "$1"; done; }

# 1. a scene that comes alive: the run goes on to its own verdict
fresh_logs
run scene-ok 't1=$SECONDS; live() { [ $((SECONDS - t1)) -ge 1 ]; }
scene_live "the raider is in combat with the stand-in" 6 live
verdict NO "the run went on"'
[ "$RC" = 0 ] && grep -q 'scene live: the raider is in combat with the stand-in' <<<"$OUT" && grep -q 'REPRO: NO - the run went on' <<<"$OUT" && [ "$ENDS" = 1 ] \
	&& ok "scene_live: a scene that comes alive goes on (rc $RC, end steps $ENDS)" || no "scene_live alive" "rc=$RC ends=$ENDS: $OUT"

# 2. a scene that never does: VOID at once, the tails of both logs (no gfx dump), the end step run, nothing after it
fresh_logs
run scene-void 'scene_live "the friend took a hit" 2 false
verdict NO "must not be reached"'
[ "$RC" = 2 ] && grep -q 'REPRO: VOID - scene not live: the friend took a hit (not within 2 s)' <<<"$OUT" && ! grep -q 'must not be reached' <<<"$OUT" \
	&& grep -q 'host| .*Session: host line 3' <<<"$OUT" && grep -q 'vm| .*Session: vm line 3' <<<"$OUT" && ! grep -q 'gfx: ' <<<"$OUT" \
	&& [ -s "$T/out-scene-void/void-host.log" ] && [ "$ENDS" = 1 ] && [ "$DT" -lt 30 ] \
	&& ok "scene_live: a dead scene is VOID in ${DT} s with both logs' tails, the end step run once" || no "scene_live dead" "rc=$RC ends=$ENDS dt=$DT: $OUT"

# 3. idle_watch: a game that keeps logging progress (the host) is never stalled
fresh_logs
writer "$REPRO_LOG_HOST" "CopyWatch: hit on our copy FF000801 (player 2's) by a shot" 9 & W=$!
run idle-busy 'idle_watch "CopyWatch: hit on|took .* damage" 5
sleep 8
verdict NO "the fight went on"'
wait "$W"
[ "$RC" = 0 ] && grep -q 'REPRO: NO - the fight went on' <<<"$OUT" && [ "$ENDS" = 1 ] \
	&& ok "idle_watch: steady progress lines in the host's log keep the run going" || no "idle_watch busy (host)" "rc=$RC: $OUT"

# 4. the same, the progress in the VM's log only
fresh_logs
writer "$REPRO_LOG_VM" "NpcSync: player 1 took 12.0 damage" 9 & W=$!
run idle-busy-vm 'idle_watch "CopyWatch: hit on|took .* damage" 5
sleep 8
verdict NO "the fight went on"'
wait "$W"
[ "$RC" = 0 ] && grep -q 'REPRO: NO' <<<"$OUT" && ok "idle_watch: progress in the VM's log counts too" || no "idle_watch busy (vm)" "rc=$RC: $OUT"

# 5. a stall: lines that do not match come and go, none that does; the script's long sleep is cut short, its end step runs, a walk it
# left going is stopped, and no watcher outlives it
fresh_logs
writer "$REPRO_LOG_HOST" "WorldSync: something else" 12 & W=$!
run idle-stall '_REPRO_WALKING=" vm"
idle_watch "CopyWatch: hit on|took .* damage" 5
cat "$_RW"/*.pid > "$T/idle-stall.pids"
sleep 120
verdict NO "must not be reached"'
kill "$W" 2>/dev/null; wait "$W" 2>/dev/null
alive=""; for p in $(cat "$T/idle-stall.pids" 2>/dev/null); do kill -0 "$p" 2>/dev/null && alive="$alive $p"; done
[ "$RC" = 2 ] && grep -q "REPRO: VOID - stalled: no line matching 'CopyWatch: hit on|took .\* damage'" <<<"$OUT" && ! grep -q 'must not be reached' <<<"$OUT" \
	&& [ "$DT" -lt 60 ] && [ "$ENDS" = 1 ] && grep -q 'dc vm walkto stop' <<<"$CALLS" && grep -q 'dc vm pad unplug' <<<"$CALLS" \
	&& grep -q 'host| .*WorldSync: something else' <<<"$OUT" && [ -z "$alive" ] && [ -s "$T/idle-stall.pids" ] \
	&& ok "idle_watch: a stall is VOID in ${DT} s (the sleep cut short), end step and walk stop run, no watcher left" \
	|| no "idle_watch stall" "rc=$RC dt=$DT ends=$ENDS alive='$alive' calls='$CALLS': $OUT"

# 6. idle_watch off: a quiet stretch meant to be quiet is not a stall
fresh_logs
run idle-off 'idle_watch "CopyWatch: hit on" 2
idle_watch off
sleep 5
verdict NO "quiet on purpose"'
[ "$RC" = 0 ] && grep -q 'REPRO: NO - quiet on purpose' <<<"$OUT" && ok "idle_watch off: no stall after it" || no "idle_watch off" "rc=$RC: $OUT"

# 7. the time cap (REPRO_CAP), and a script's own (run_cap)
fresh_logs
run cap-env 'run_cap 100
sleep 120
verdict NO "must not be reached"' REPRO_CAP=3
[ "$RC" = 2 ] && grep -q 'REPRO: VOID - time cap: the run took [0-9]* s, past its cap of 3 s' <<<"$OUT" && [ "$DT" -lt 60 ] && [ "$ENDS" = 1 ] \
	&& ok "time cap: an exported REPRO_CAP=3 (over the script's run_cap 100) ends a 120 s sleep as VOID in ${DT} s" || no "time cap (REPRO_CAP)" "rc=$RC dt=$DT: $OUT"
fresh_logs
run cap-own 'run_cap 2
sleep 120
verdict NO "must not be reached"'
[ "$RC" = 2 ] && grep -q 'REPRO: VOID - time cap: .* past its cap of 2 s' <<<"$OUT" && [ "$DT" -lt 60 ] \
	&& ok "time cap: run_cap 2 ends the run in ${DT} s" || no "time cap (run_cap)" "rc=$RC dt=$DT: $OUT"
fresh_logs
run cap-default 'verdict NO "short"'
grep -q 'REPRO: NO - short' <<<"$OUT" && [ "$(cat "$T/out-cap-default/.watch/cap")" = 3600 ] && ok "time cap: every run has one (3600 s by default)" \
	|| no "time cap default" "cap=$(cat "$T/out-cap-default/.watch/cap" 2>/dev/null): $OUT"

# 8. stopped from outside (TERM): VOID, the end step run
fresh_logs
cat > "$T/term.sh" <<EOF
#!/bin/bash
. "$LIB"
dc() { :; }
on_end eval 'echo end >> "$T/term.ends"'
: > "$T/term.ready"
for i in \$(seq 1 120); do sleep 1; done
verdict NO "must not be reached"
EOF
: > "$T/term.ends"
REPRO_OUT=$T/out-term bash "$T/term.sh" > "$T/term.out" 2>&1 & P=$!
# a TERM waits for the command the script runs to end, as bash does (Ctrl-C reaches both): so this script sleeps a second at a time
for i in $(seq 1 120); do [ -e "$T/term.ready" ] && break; sleep 0.5; done
kill -TERM "$P"; wait "$P"; RC=$?; OUT=$(cat "$T/term.out")
[ "$RC" = 2 ] && grep -q 'REPRO: VOID - the run was stopped (SIGTERM)' <<<"$OUT" && [ "$(grep -c end "$T/term.ends")" = 1 ] \
	&& ok "a TERM from outside: VOID, the end step run" || no "TERM" "rc=$RC: $OUT"

# 9. end steps: once, the last given first, also when the script ends with no verdict; a verdict in a subshell runs none
fresh_logs
run ends-order 'on_end eval "echo first >> $T/ends-order.ends"
on_end eval "echo second >> $T/ends-order.ends"
x=$(verdict YES "in a subshell")
verdict NO "main"'
[ "$RC" = 0 ] && [ "$(paste -sd, "$T/ends-order.ends")" = "second,first,end" ] && ok "on_end: once each, the last given first; a subshell's verdict runs none" \
	|| no "on_end order" "rc=$RC ends=$(paste -sd, "$T/ends-order.ends"): $OUT"
fresh_logs
run ends-noverdict 'echo body done'
[ "$RC" = 0 ] && [ "$ENDS" = 1 ] && ok "on_end: a script that ends with no verdict runs its end steps too" || no "on_end with no verdict" "rc=$RC ends=$ENDS"

echo
[ "$FAILS" = 0 ] && echo "watch-selftest: all passed" || echo "watch-selftest: $FAILS failed"
exit "$FAILS"
