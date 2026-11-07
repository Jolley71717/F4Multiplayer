#!/bin/bash
# clocklib.sh: lining the two machines' traces up by their clocks. Sourced by trace.sh and steps.sh; no game, no ssh.
#
# Each game stamps its trace rows with its machine's clock, so the VM's clock minus the host's (the offset, measured over
# ssh before and after a trace: trace.sh offset) is taken out of the VM's rows. Two things moved that offset during a trace
# and failed the power armor step as if the mod had (0.12.0, 0.13.1 and 0.13.2 soaks; VM's System log, Kernel-General 1):
#  - the VM's time service had two sources, the host's clock (Hyper-V's time provider) and an internet time server, about
#    2 s apart while the host's own clock drifted between its syncs; the forced resync before each trace let the second set
#    the VM's clock ~1.9 s ahead, and the first set it back 15 s later, mid-trace. The VM now takes only the host's clock
#    (its NtpClient provider off: rig upkeep, not in this repository), and a resync is made only when the offset is large.
#  - any step at all, on either machine (the host's clock is set too when its time service syncs). Since a build with
#    `trace stop` answering wallStep=, a trace's rows are stamped with the clock as it read at `trace start`, run on by the
#    steady clock, so a step during the trace moves no row; the offset that lines the rows up is the one at the start.
#
# clock_lineup <before> <after> <host wallStep> <vm wallStep>: the VM's clock minus the host's to take out of its rows, from
# the readings before and after a trace and each game's wallStep (empty from an older build). Prints "ok <ms> <how>", or
# "rig <why>" when the readings can't line the traces up (unread, or a clock moved outside the trace): the rig's clocks, a
# rig fault, never a failure of the mod.
#  - stamps run on the steady clock (both wallSteps given): the offset is the one before; the one after must be it plus
#    what the two clocks stepped during the trace (vm - host), within CLOCK_HOLD_MS. Anything else stepped a clock between
#    a reading and the trace's start or end, and which offset held during the trace is not known.
#  - an older build (a wallStep missing): the clocks are the stamps, so the two readings must agree within CLOCK_HOLD_MS
#    (a step during the trace moved the rows), and their mean is taken.
# Either way the offset must be at most CLOCK_MAX_MS (a time service that badly off is broken: fix it first).
CLOCK_HOLD_MS=${CLOCK_HOLD_MS:-50}
CLOCK_MAX_MS=${CLOCK_MAX_MS:-500}
_clk_int() { [[ $1 =~ ^-?[0-9]+$ ]]; }
_clk_abs() { local v=$1; echo $((v < 0 ? -v : v)); }
# wall_step <`trace stop` answer>: its wallStep= (ms the machine's clock stepped during the trace), empty from an older build
wall_step() { sed -n 's/.*wallStep=\(-\?[0-9]*\).*/\1/p' <<<"$1" | head -1; }
clock_lineup() {
	local before=$1 after=$2 hs=$3 vs=$4 off how want
	if ! _clk_int "$before" || ! _clk_int "$after"; then echo "rig the VM's clock offset was not read (before '$before', after '$after')"; return 1; fi
	if _clk_int "$hs" && _clk_int "$vs"; then
		want=$((before + vs - hs))
		if [ "$(_clk_abs $((after - want)))" -gt "$CLOCK_HOLD_MS" ]; then
			echo "rig a clock was set between a reading and the trace (offset before $before, after $after; during the trace the host's clock stepped $hs ms, the VM's $vs ms, so after should be $want): which offset held is not known"; return 1
		fi
		off=$before; how="stamps on the steady clock; the clocks stepped host $hs ms, vm $vs ms during the trace"
	else
		if [ "$(_clk_abs $((after - before)))" -gt "$CLOCK_HOLD_MS" ]; then
			echo "rig the VM's clock offset moved $((after - before)) ms during the trace (before $before, after $after; a build without wallStep stamps rows with the clock itself, so a step moved them)"; return 1
		fi
		off=$(( (before + after) / 2 )); how="the clocks are the stamps (an older build); they held within $CLOCK_HOLD_MS ms"
	fi
	if [ "$(_clk_abs "$off")" -gt "$CLOCK_MAX_MS" ]; then echo "rig the VM's clock is $off ms off the host's, over $CLOCK_MAX_MS ms: fix the VM's time service"; return 1; fi
	echo "ok $off $how"
}

# `bash clocklib.sh selftest`
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "$1" = selftest ]; then
	bad=0
	t() { case "$2" in $3) echo "ok   $1";; *) echo "FAIL $1: got [$2] want [$3]"; bad=1;; esac; }
	t "wallStep read" "$(wall_step '412 rows -> C:\x\F4Multiplayer_trace.csv wallStep=-1875')" "-1875"
	t "wallStep absent (older build)" "$(wall_step '412 rows -> C:\x\F4Multiplayer_trace.csv')" ""
	t "steady stamps, no step" "$(clock_lineup -13 -12 0 1)" "ok -13 *"
	# 2026-11-07 05:28: the VM's clock set +1876 and back -1875 within the trace: before and after agree, the steps cancel
	t "steady stamps, a step and its undoing inside the trace" "$(clock_lineup -13 -13 0 1)" "ok -13 *"
	# a step that stayed: the VM set 1.9 s ahead during the trace, still there after it
	t "steady stamps, a step kept" "$(clock_lineup -13 1863 0 1876)" "ok -13 *"
	t "steady stamps, the host's clock stepped" "$(clock_lineup -13 -2013 2000 0)" "ok -13 *"
	t "steady stamps, a step outside the trace" "$(clock_lineup 1862 -13 0 0)" "rig *not known"
	t "older build, readings agree" "$(clock_lineup -14 -12 '' '')" "ok -13 *"
	t "older build, a step during the trace" "$(clock_lineup 1862 -13 '' '')" "rig *moved*"
	t "older build, one wallStep missing" "$(clock_lineup -13 1863 0 '')" "rig *moved*"
	t "unread before" "$(clock_lineup '' -13 0 0)" "rig *not read*"
	t "unread after" "$(clock_lineup -13 '' 0 0)" "rig *not read*"
	t "too far off" "$(clock_lineup 900 905 0 5)" "rig *over 500*"
	clock_lineup 1862 -13 0 0 >/dev/null && { echo "FAIL a rig answer returns non-zero"; bad=1; } || echo "ok   a rig answer returns non-zero"
	[ $bad -eq 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"; exit $bad
fi
