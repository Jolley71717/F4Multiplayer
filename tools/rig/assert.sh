#!/bin/bash
# assert.sh: small helpers that turn a rig run into named checks with a verdict. Source it:
#
#     . tools/rig/assert.sh
#     a_init smoke                         # makes a run folder under $CHECKS, starts the clock
#     section "movement"                   # a heading; the summary also lists the time each section took
#     pass "name" "reason"; fail "name" "reason"; warn "name" "reason"      # record one check by hand (fail: a check that read
#                                          # a game's dev command that got no answer is recorded RIG; see fail below)
#     fail_mod "name" "reason"             # a verdict on the mod that reads no dev command (a crash, a log error): always a FAIL
#     setup_fault "name" "reason"          # a step's own set-up, of one game's own state, could not be made: RIG, the step not run
#     rig_setup expect_eq "label" ...      # the same for an expect_*: its failure is a set-up fault
#     expect_eq "label" "$got" "$want"     # string equal
#     expect_le "label" "$got" 10 u        # number at most (an empty or non-number "got" is a FAIL, never a pass)
#     expect_ge "label" "$got" 1 hp        # number at least
#     expect_between "label" "$got" 0 100  # number in a range
#     expect_match "label" "$got" 'regex'  # extended regex
#     expect_zero "label" "$got"           # shorthand for expect_eq "$got" 0
#     warn_gt "label" "$got" 5 u           # number above a soft limit: WARN (printed in the summary), never a FAIL
#     rig_fault clock "name" "reason"      # a check the rig could not make (RIG, not FAIL; see rig_fault below)
#     healed "name" "how"                  # a rig state put right before or during a step (HEAL: no failure, listed)
#     rig_unhealed state "name" "reason"   # a rig state that stayed wrong: checks not made (RIGFAULT: never a PASS, counts toward the breaker)
#     a_attempt_mark; a_attempt_rig; a_attempt_void [first]  # smoke.sh's re-run: a rig fault since the mark? lines from it on set aside (VOID)
#     a_summary                            # one screen: counts, every FAIL, RIGFAULT, RIG, HEAL, VOID and WARN, times; exits non-zero on a
#                                          # FAIL, a RIGFAULT or a check never made
#     trap a_interrupted INT TERM          # after a_init: a run stopped by a signal records a FAIL ("run interrupted") and ends in VERDICT FAIL
#
# Reading the games (both are driven through the dev channel, see env.sh):
#     field "<text>" key                   # the value after "key=" up to a space, ';', '|' or ','  (empty if absent)
#     part "5/4" 2                         # the n-th part of "a/b/c"
#     ns host                              # `net status` of that game, cached in NS_host / NS_vm for nsf
#     nsf host key                         # key from the cached `net status`
#     dcf vm "refinfo 00000014" hp         # run a command and take one field of the reply
#     expect_same "label" "refinfo X" hp 2     # same field from both games, within 2 of each other
#
# The plugin log (the delta since a mark, so history never fails a run):
#     log_mark host|vm                     # remember where each game's log is now (survives a log rotation)
#     log_check host|vm "label"            # since the mark: any [E] line or assertion text FAILs, [W] lines are counted per prefix
#     log_privacy host|vm "label"          # since the mark: private patterns, Steam ids, other IPs. Counts only, never the text
#
# `bash assert.sh selftest` checks all of this without touching a game.
#
# Every check is appended to $A_OUT/results.tsv as it happens, so a check made in a subshell is not lost.
# the self-test needs no game, so no VM: env.sh insists on VM_SSH, give it a placeholder only for `assert.sh selftest`
[ "${BASH_SOURCE[0]}" = "$0" ] && [ "$1" = selftest ] && : "${VM_SSH:=selftest-no-vm}"
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

A_SECTION=""
declare -A NS MARK_SIZE MARK_FIRST SEC_T

a_init() {  # a_init <name>: $A_OUT = $CHECKS/<name>-<stamp>/, results.tsv inside
	A_NAME=$1; A_T0=$SECONDS; A_START=$(date +%T)
	A_OUT=${A_OUT:-$CHECKS/$1-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$A_OUT"
	A_RESULTS=$A_OUT/results.tsv; : > "$A_RESULTS"; A_SECLOG=$A_OUT/sections.tsv; : > "$A_SECLOG"
	A_TIMES=$A_OUT/times.tsv; : > "$A_TIMES"; _now; A_EPOCH0=$NOW
	_reach_count > "$A_OUT/reach.mark"
	export F4MP_DC_STATE=$A_OUT/dc-noanswer; rm -f "$F4MP_DC_STATE.host" "$F4MP_DC_STATE.vm" "$F4MP_DC_STATE.seq" "$F4MP_DC_STATE.at"   # dcfast.sh: a game whose last command got no answer, and the count of dev calls
	echo "$1 run folder: $A_OUT"
}
# The VM's dev channel made reachable again by dcfast.sh (env.sh vm_reach: a tunnel or token made again while the VM's game
# ran) is a HEAL of this run: each one in $STAGE/vm-reach.log since a_init is recorded before the next line (the mark is a
# file, so a heal seen in a subshell is not recorded twice).
_reach_count() { if [ -s "$STAGE/vm-reach.log" ]; then wc -l < "$STAGE/vm-reach.log"; else echo 0; fi; }
_reach_flush() {
	local m n t what
	[ -n "$A_OUT" ] && [ -f "$A_OUT/reach.mark" ] || return 0
	m=$(cat "$A_OUT/reach.mark"); n=$(_reach_count); [ "$n" -gt "$m" ] 2>/dev/null || return 0
	echo "$n" > "$A_OUT/reach.mark"
	sed -n "$((m + 1)),${n}p" "$STAGE/vm-reach.log" | while IFS=$'\t' read -r _ t what; do
		printf 'HEAL\t%s\t%s\t%s\n' "$A_SECTION" "the VM's dev channel reached again ($t)" "$what" >> "$A_RESULTS"
		printf 'HEAL %s: %s\n' "the VM's dev channel reached again ($t)" "$what"
	done
}

section() {  # section <title>
	[ -z "$A_RIG_STOPPED" ] && [ -n "$A_RESULTS" ] && [ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && _rig_stop "$(_rig_last_signal)"
	[ -n "$A_SECTION" ] && printf '%s\t%s\n' "$A_SECTION" "$((SECONDS - A_SEC_T0))" >> "$A_SECLOG"
	A_SECTION="$1${A_ROUND:+ (round $A_ROUND)}${A_AGAIN:+ (again, after a rig fault)}"; A_SEC_T0=$SECONDS; echo; echo "=== $A_SECTION ($(date +%T))"   # A_ROUND: smoke.sh --soak's round
}

# _now: NOW = the clock, in seconds (no fork: record calls it for every line). A_NOW: the self-test's clock.
_now() { if [ -n "${A_NOW:-}" ]; then NOW=$A_NOW; else printf -v NOW '%(%s)T' -1; fi; }
record() {  # record <PASS|FAIL|WARN> <name> <reason>
	_reach_flush; _dc_batch
	printf '%s\t%s\t%s\t%s\n' "$1" "$A_SECTION" "$2" "$3" >> "$A_RESULTS"
	[ -n "$DC_AT" ] && printf '%s u\n' "$DC_AT" > "$F4MP_DC_STATE.at"   # this line used its own calls: the next has none until it makes one
	# when it was made, whether it is the mod's own verdict (mod: fail_mod, mod_check) and the games its readings were of: hang_check, a_attempt_void
	[ -n "${A_TIMES:-}" ] && { _now; local mk=-; [ -n "${A_MOD_CHECK:-}" ] && mk=mod; printf '%s\t%s\t%s\t%s\t%s\n' "$NOW" "$1" "$2" "$mk" "$DC_READ" >> "$A_TIMES"; }
	printf '%-4s %s%s\n' "$1" "$2" "${3:+: $3}"
}
pass() { record PASS "$1" "$2"; }
# fail <name> <reason>: a check that failed. One kind of failure says nothing of the mod and is recorded RIG (rig_fault
# below: the check was not made, a run with one never made is VERDICT: RIG, and smoke.sh runs a loop step again):
#   channel  a dev command this check read got no answer in time: the game was hung or stalled, and what the check read
#            was the empty reply ("got '', want '1'"; 2026-11-08 to 11-14: about 20 such FAIL lines). dcfast.sh counts
#            every dev call of the run ($F4MP_DC_STATE.seq) and keeps, per game, the count at its last command that got
#            no answer ($F4MP_DC_STATE.<host|vm>, removed by that game's next answer). A check's readings are the dev
#            calls made since the line recorded before it (_dc_batch; for this rule, lines recorded with no call between
#            them share their readings, as the fields of one reply do). So only a check that read the unanswered command is taken
#            as not made. A check made of later readings (of the other game, or after the game answered again) is a
#            FAIL, and so is one whose unanswered command is older than DC_NA_AGE s (35: the 20 s a command waits and a
#            step's slack).
# The rule must never hide a fault of the mod, so:
#   - a verdict that reads no dev command is recorded with fail_mod (or under mod_check) and is always a FAIL: a new crash
#     log, an error or an assert in the plugin log, private text in it, a hang in the mod's code. Its line is marked the
#     mod's (times.tsv), and neither a hang taken as the rig's (hang_check) nor smoke.sh's re-run (a_attempt_void) sets
#     it aside;
#   - a game that crashed is a crash, not a channel fault: crash_verdict turns that game's channel lines back into FAILs
#     (_channel_stand), and so does a hang that can't be said to be the rig's (hang_check).
# A set-up is never told by a check's name. A step says so itself, with setup_fault (or rig_setup before an expect_*), and
# only for one game's own state after a console command or the harness's own input: an ini value, a console setstage or
# placement read back in the game it was typed in, an item given, a walk, an aim. Its failure is the step not run (signal
# setup), and the run is never a PASS for it. What the mod carried to the other game (a stand-in, a copy, a synced stage,
# a rejoin, a counter of the mod's) is a plain fail, named "precondition:": a FAIL.
DC_NA_AGE=${DC_NA_AGE:-35}
# _dc_batch: DC_BASE: the count of dev calls at the last recorded line that calls were made after (the calls past it are
# this line's readings, for the channel rule); DC_READ: the games of the calls this line made itself ("host", "vm",
# "host vm"; "-": none), for a hang (hang_check). A line with no call of its own reads no game, though it shares the line
# before's readings: a check of a log or a file recorded after a check of the VM's game is never set aside by the VM's
# hang. dcfast.sh writes a letter a call (h, v, b for both); any other is a game not known, and counts for neither.
# $F4MP_DC_STATE.at: "<base> <count>", with " u" once a line was recorded with those calls (record writes DC_AT then).
_dc_batch() {
	local x="" b=0 l=0 u="" r h=""; DC_BASE=0; DC_READ=-; DC_AT=""
	[ -n "${F4MP_DC_STATE:-}" ] && [ -s "$F4MP_DC_STATE.seq" ] || return 0
	read -r -N 100000000 x < "$F4MP_DC_STATE.seq"   # in blocks (dcfast.sh dc_count)
	[ -s "$F4MP_DC_STATE.at" ] && read -r b l u < "$F4MP_DC_STATE.at"
	case "$b$l" in ""|*[!0-9]*) b=0; l=0; u="" ;; esac
	if [ "${#x}" -gt "$l" ]; then b=$l; l=${#x}; u=""; printf '%s %s\n' "$b" "$l" > "$F4MP_DC_STATE.at"; fi
	DC_BASE=$b; [ -n "$u" ] && return 0   # no call of its own
	DC_AT="$b $l"; r=${x:b:l-b}
	case $r in *[hb]*) h=host ;; esac
	case $r in *[vb]*) h="$h${h:+ }vm" ;; esac
	DC_READ=${h:--}
}
_dc_noanswer() {  # DC_NA: "<who>'s game <what>" for each game whose unanswered dev command is among this check's readings; 1 when none
	local g e q s n; DC_NA=""
	[ -n "${F4MP_DC_STATE:-}" ] || return 1
	_dc_batch
	for g in host vm; do
		[ -s "$F4MP_DC_STATE.$g" ] || continue
		IFS=$'\t' read -r e q s < "$F4MP_DC_STATE.$g"
		case $e in ""|*[!0-9]*) continue ;; esac; case $q in ""|*[!0-9]*) continue ;; esac; printf -v n "%(%s)T" -1
		[ "$q" -gt "$DC_BASE" ] && [ $((n - e)) -le "$DC_NA_AGE" ] && DC_NA="$DC_NA${DC_NA:+; }$g's game $s"
	done
	[ -n "$DC_NA" ]
}
fail() {
	if [ -n "${A_MOD_CHECK:-}" ]; then record FAIL "$1" "$2"
	elif _dc_noanswer; then rig_fault channel "$1" "$DC_NA, so this check was not made${2:+ (it read: $2)}"
	elif [ -n "${A_RIG_SETUP:-}" ]; then setup_fault "$1" "$2"
	else record FAIL "$1" "$2"; fi
}
fail_mod() { local A_MOD_CHECK=1; record FAIL "$1" "$2"; }   # fail_mod <name> <reason>: a verdict on the mod that reads no dev command: never a rig fault, never set aside
mod_check() { local A_MOD_CHECK=1; "$@"; }      # mod_check expect_... : the same for an expect_*
# setup_fault <name> <reason>: a step's own set-up could not be made (see above): the step was not run. rig_setup expect_...:
# an expect_* whose failure is one.
setup_fault() { rig_fault setup "$1" "this set-up could not be made, so what the step tests was not tested${2:+: $2}"; }
rig_setup() { local A_RIG_SETUP=1; "$@"; }
# _channel_stand <who> <why> [from to]: that game's rig fault (channel) lines, also those a re-run set aside, are FAILs again
# (of the checks made in that time, when given; a check's time as in _hang_fails); prints their names, one a line
_channel_stand() {
	[ -s "${A_RESULTS:-}" ] || return 0
	local tmp two=""; if [ -n "${3:-}" ]; then [ -s "${A_TIMES:-}" ] || return 0; two=1; fi
	tmp=$(mktemp)
	awk -F'\t' -v OFS='\t' -v g="$1's game " -v why="$2" -v a="${3:-}" -v b="${4:-}" -v look="$HANG_LOOK" -v two="$two" -v names="$tmp.names" '
		two != "" && FNR == NR { k = $3 SUBSEP (++n[$3]); S[k] = (FNR > 1 && prev > $1 - look) ? prev : $1 - look; E[k] = $1; prev = $1; next }
		{ k = $3 SUBSEP (++m[$3]); r = "" }
		$1 == "RIG" && index($4, "rig fault (channel): ") == 1 { r = substr($4, 22) }
		$1 == "VOID" && index($4, "[was RIG] rig fault (channel): ") == 1 { r = substr($4, 32) }
		r != "" && index(r, g) && (two == "" || ((k in E) && S[k] <= b && E[k] >= a)) { print $3 > names; $1 = "FAIL"; $4 = why " (it was first taken as a channel fault: " r ")" }
		{ print }' ${two:+"$A_TIMES"} "$A_RESULTS" > "$tmp" && cat "$tmp" > "$A_RESULTS"
	cat "$tmp.names" 2>/dev/null; rm -f "$tmp" "$tmp.names"
}
# rig_fault_stands <signal> <name> <why>: the rig fault lines of that signal and check name, also those a re-run set aside,
# are FAILs again: what a whole run showed says they were not the rig's (hits.sh: a shooter none of whose shots reached a copy)
rig_fault_stands() {
	[ -s "${A_RESULTS:-}" ] || return 0
	local tmp; tmp=$(mktemp)
	awk -F'\t' -v OFS='\t' -v p="rig fault ($1): " -v nm="$2" -v why="$3" -v sig="$1" '
		{ r = "" }
		$3 == nm && $1 == "RIG" && index($4, p) == 1 { r = substr($4, length(p) + 1) }
		$3 == nm && $1 == "VOID" && index($4, "[was RIG] " p) == 1 { r = substr($4, length(p) + 11) }
		r != "" { $1 = "FAIL"; $4 = why " (it was first taken as a " sig " fault: " r ")" }
		{ print }' "$A_RESULTS" > "$tmp" && cat "$tmp" > "$A_RESULTS"; rm -f "$tmp"
}
warn() { record WARN "$1" "$2"; }

# --- the circuit breaker: a broken rig ends the run ------------------------------------------------------------
# A check that finds the RIG broken, not the mod, says so with rig_broken: a game's input not reaching it (a click that
# fires nothing, keys that move nobody), a game not running or not connected, ssh to the VM down. Such a failure is a FAIL
# like any other, and RIG_BREAK_AT of them (2) end the run at once as FAIL "run stopped: rig broken (<signal>)": the rest
# would measure the rig (2026-11-01: a VM whose mouse never came up made 18 failures that proved nothing). A failure of the
# mod is a plain fail and never trips it. They are counted in results.tsv, so one made in a subshell counts too; section()
# looks again, for a trip made where an exit can't end the run. A rig-broken failure that smoke.sh's re-run set aside (VOID,
# "[was FAIL] rig broken (...)") still counts: two rig-broken events stop the run, set aside or not.
RIG_BREAK_AT=${RIG_BREAK_AT:-2}
_rig_last_signal() { awk -F'\t' '($1=="FAIL" && index($4, "rig broken (") == 1) || ($1=="VOID" && index($4, "[was FAIL] rig broken (") == 1) || ($1=="RIGFAULT" && index($4, "rig fault (") == 1) {s=substr($4, index($4, "(") + 1); s=substr(s, 1, index(s, ")") - 1)} END{print s}' "$A_RESULTS" 2>/dev/null; }
_rig_broken_count() { awk -F'\t' '($1=="FAIL" && index($4, "rig broken (") == 1) || ($1=="VOID" && index($4, "[was FAIL] rig broken (") == 1) || $1=="RIGFAULT" {n++} END{print n+0}' "$A_RESULTS" 2>/dev/null; }
_rig_stop() {  # _rig_stop <signal> [why]: the run ends here, its verdict FAIL. why: a stop for a reason that isn't the rig's
	A_RIG_STOPPED=1
	if [ -n "$2" ]; then
		record FAIL "run stopped: $1" "$2. The checks after this point were not made"; echo "STOP: $1: $2"
	else
		record FAIL "run stopped: rig broken ($1)" "$(_rig_broken_count) rig-broken failure(s), the last: $1. The checks after this point were not made; fix the rig and run again"
		echo "STOP: rig broken ($1)"
	fi
	declare -F crash_verdict >/dev/null && [ -n "$A_NAME" ] && crash_verdict "$A_NAME"   # a game that went down may have crashed: that is the mod's, and is said
	_hang_at_stop "$1"                                                                    # ... or hung: said too, and whose it is
	a_summary
}
rig_broken() {  # rig_broken <signal: input|game|ssh> <check name> <reason>
	record FAIL "$2" "rig broken ($1): $3"
	[ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && [ -z "$A_RIG_STOPPED" ] && _rig_stop "$1"
	return 0
}
# A rig fault is a check the rig could not make this time, with the rest of the rig fine: the two machines' clocks could
# not line a trace up (the VM's clock offset could not be read over ssh, which was every one on 2026-11-07 under a loaded
# host, or a time service set a clock between a reading and the trace); or a step whose own set-up could not be made, so the
# step was not run (signal setup, said by the step with setup_fault: hits.sh's raider with no clear spot, a line of fire
# blocked at every spot, clicks that came late on hitching games, a quest the host's ini does not share). It is recorded
# RIG, not FAIL: no verdict on the mod
# either way, and it never trips the circuit breaker. The summary lists them; a soak whose every RIG check was made and
# passed in another round still passes, and one with a check the rig never made is VERDICT: RIG (a non-zero exit).
rig_fault() {  # rig_fault <signal: clock|walk|input|state|setup|channel|hang> <check name> <reason>
	record RIG "$2" "rig fault ($1): $3"
}

# --- the rig's faults healed, and a step made again ------------------------------------------------------------
# A soak FAIL must mean the mod's: a rig problem is found, healed with real actions and the check made again, never left
# to fail product checks (2026-11-07: the VM left in power armor by one climb-out that missed made 9 of the 0.14.0-rc1
# soak's 20 failures). Three more kinds of line, beside PASS, FAIL, WARN and RIG:
#   HEAL      the rig was found in a wrong state (a menu open, a player in power armor or down, a walk stuck) and put right
#             (healed <what> <how>). Listed in the summary; no verdict either way.
#   VOID      a check made in an attempt at a step after it met a rig fault (a RIG line of a walk, a press or a state, or a
#             FAIL that says "rig broken"): the step is healed and run again, and what the attempt recorded from that line on
#             is set aside (its kind kept in the reason, "[was FAIL] ..."), so a failure that followed from the rig's fault
#             never stands as the mod's. A heal or a re-run never erases evidence of the mod's failure: the lines before the
#             rig line stand (the rig was still sound when they were made); a clock fault sets nothing aside (it is the one
#             check a stepped clock spoils, and its RIG line already says "not made"); a check that fails in both attempts
#             stays a FAIL, rig line or not; and a PASS verdict names the failures it set aside, to be read.
#   RIGFAULT  a rig fault that could not be healed: a wrong state that stayed, or a step that met a rig fault in both of its
#             attempts. Its checks were not made, so a run with one is never a PASS (VERDICT: RIG). RIG_BREAK_AT of them (2),
#             with the rig-broken failures, end the run as the breaker always has.
healed() {  # healed <what> <how>
	record HEAL "$1" "$2"
}
rig_unhealed() {  # rig_unhealed <signal> <what> <reason>
	record RIGFAULT "$2" "rig fault ($1), not healed: $3"
	[ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && [ -z "$A_RIG_STOPPED" ] && _rig_stop "$1"
	return 0
}
# An attempt at a step: a_attempt_mark before it, then a_attempt_rig says whether it met a rig fault (prints the signals), and
# a_attempt_void sets its lines aside. Lines are counted in results.tsv, so a check made in a subshell is seen too.
a_attempt_mark() { A_ATTEMPT_LINE=$(wc -l < "$A_RESULTS" 2>/dev/null || echo 0); }
a_attempt_rig() {
	awk -F'\t' -v s="$A_ATTEMPT_LINE" 'NR > s && ($1 == "RIG" || ($1 == "FAIL" && index($4, "rig broken (") == 1)) { printf "%s%s: %s", (n++ ? "; " : ""), $3, substr($4, 1, 160) } END { exit !n }' "$A_RESULTS"
}
# a_attempt_void [first]: the attempt's lines from its first rig line on (a RIG line other than a clock's, or a rig-broken
# FAIL) set aside as VOID; the lines before it, clock RIG lines, HEAL and RIGFAULT lines stand. first: the results line the
# step's first attempt began at, given for its second: a FAIL (not a rig-broken one) of a check whose failure the first
# attempt had set aside is the check failing in both attempts, and stands ("in both attempts: ...").
# A line marked the mod's own verdict (fail_mod, mod_check: times.tsv) is never set aside: a crash, a log error or assert,
# private text or a hang in the mod's code stays a FAIL whatever the rig did.
a_attempt_void() {
	local t ht=""; t=$(mktemp); [ -s "${A_TIMES:-}" ] && ht=1
	awk -F'\t' -v OFS='\t' -v s="$A_ATTEMPT_LINE" -v first="${1:-}" -v ht="$ht" '
		function rigline() { return ($1 == "RIG" && index($4, "rig fault (clock)") != 1) || ($1 == "FAIL" && index($4, "rig broken (") == 1) }
		BEGIN { f = ht ? -1 : 0 }
		FNR == 1 { f++ }
		f == 0 { k = $3 SUBSEP (++tn[$3]); if ($4 == "mod") mod[k] = 1; next }
		f == 1 { if (first != "" && FNR > first && FNR <= s && $1 == "VOID" && index($4, "[was FAIL] ") == 1 && index($4, "[was FAIL] rig broken (") != 1) was[$3] = 1; next }
		{ k = $3 SUBSEP (++m[$3]) }
		FNR > s && !on && rigline() { on = 1 }
		on && !(k in mod) && $1 != "VOID" && $1 != "HEAL" && $1 != "RIGFAULT" && !($1 == "RIG" && index($4, "rig fault (clock)") == 1) {
			if ($1 == "FAIL" && !rigline() && ($3 in was)) $4 = "in both attempts: " $4
			else { $4 = "[was " $1 "] " $4; $1 = "VOID" }
		}
		{ print }' ${ht:+"$A_TIMES"} "$A_RESULTS" "$A_RESULTS" > "$t" && cat "$t" > "$A_RESULTS"
	rm -f "$t"
}
# rig_ssh_up <label>: the VM answers ssh
rig_ssh_up() { if [ "$(vmssh 'echo up' | tr -d '[:space:]')" = up ]; then pass "$1" "ssh to the VM answers"; return 0; fi; rig_broken ssh "$1" "ssh to the VM does not answer"; return 1; }
# rig_up <label>: both games run, are in the game and connected; a game that is not is a broken rig (signal game, or ssh
# when the VM's game does not answer and ssh does not either)
rig_up() {
	local a w s down=""; a=$(alive)
	for w in host vm; do
		case "$a" in *"$w=ingame=true"*) ;; *) down="$down $w";; esac
	done
	if [ -n "$down" ]; then
		case "$down" in *vm*) [ "$(vmssh 'echo up' | tr -d '[:space:]')" = up ] || { rig_broken ssh "$1" "$a: the VM's game does not answer and ssh to the VM is down"; return 1; };; esac
		rig_broken game "$1" "$a: not running or not in the game:$down"; return 1
	fi
	for w in host vm; do
		s=$(field "$(dc $w 'net status')" status)
		[ "$s" = joined ] || { rig_broken game "$1" "$w is not connected to the session (status='$s')"; return 1; }
	done
	pass "$1" "$a, both joined"; return 0
}
# rig_input_ready <who> [seconds]: before an input probe, that game is brought to the front and its player left free to act:
# nothing open but the HUD (dev `menus`; the power armor HUD counts as the HUD: it stays listed after a climb-out, and every
# probe after the first power armor step waited 30 s for it, 2026-11-07). What a player would close is closed: a message box (Enter), the Pip-Boy, a terminal or
# a container (Tab), the pause menu (Esc), the console (~). Anything else is waited out, up to <seconds> (default 30): a loading
# screen, a fade, or the engine's level-up prompt (PromptMenu, its input layer up for about 20 s), which a game shows once,
# half a minute after its first load (2026-11-01: a hits run started right after a deploy and join met it). INPUT_READY says
# what was closed or waited for (empty when nothing was); returns 1 if something is still open then, and the probe goes on (its
# own result decides). A build without `menus` counts as ready.
rig_input_ready() {
	local w=$1 limit=${2:-30} t0=$SECONDS m open did="" waited=""
	INPUT_READY=""
	dc "$w" focus >/dev/null
	while :; do
		m=$(dc "$w" menus | grep -oE 'open=[^ ]*'); [ -n "$m" ] || break
		open=$(tr ',' '\n' <<<"${m#open=}" | cut -d: -f1 | grep -vxE 'HUDMenu|CursorMenu|PowerArmorHUDMenu|none' | tr '\n' ' '); open=${open% }
		[ -z "$open" ] && break
		if [ $((SECONDS - t0)) -ge "$limit" ]; then INPUT_READY="${did:+closed$did; }${waited:+waited for$waited; }still open after $limit s: $open"; return 1; fi
		dc "$w" focus >/dev/null
		case " $open " in
		*" MessageBoxMenu "*) dc "$w" "key 0x0D 60" >/dev/null; did="$did MessageBoxMenu" ;;
		*" PipboyMenu "*|*" TerminalMenu "*|*" TerminalHolotapeMenu "*|*" ContainerMenu "*) dc "$w" "key 0x09 60" >/dev/null; did="$did $(grep -oE 'PipboyMenu|TerminalHolotapeMenu|TerminalMenu|ContainerMenu' <<<"$open" | head -1)" ;;
		*" PauseMenu "*) dc "$w" "key 0x1B 60" >/dev/null; did="$did PauseMenu" ;;
		*" Console "*) dc "$w" "key 0xC0 60" >/dev/null; did="$did Console" ;;
		*) case "$waited " in *" $open "*) ;; *) waited="$waited $open";; esac ;;
		esac
		sleep 1.5
	done
	[ -n "$did$waited" ] && INPUT_READY="${did:+closed$did}${did:+${waited:+; }}${waited:+waited for$waited ($((SECONDS - t0)) s)}"
	# Last, the window in front, on a screen, and kept there a moment: the mod reads no input while it is not (InputGate: polling
	# off), and a click that brings it back is kept from the game. `focus` answers whether the game was in front as it came in, so
	# the second of a pair says whether the first brought it there. Two ways the front goes, both seen 2026-11-01:
	#  - the window parked off every screen: a game minimised during its first load into the world (started while another window
	#    had the front) keeps the minimised spot (-32000, -32000) once restored. It is "in front" and takes keys, but a real click
	#    lands on the window under the cursor (a folder window on the host), which takes the front: a click "fires nothing", for
	#    the game's whole life. The dev channel now counts such a window as not in front, and `focus` moves it back to the primary
	#    screen; `click` puts the cursor over the game's window first.
	#  - the host's desktop is shared: another window can take the front for seconds.
	local f k
	INPUT_FRONT=yes
	for k in 1 2 3 4 5 6; do dc "$w" focus >/dev/null; sleep 1; f=$(dc "$w" focus); case "$f" in front=no*) ;; *) break;; esac; done
	case "$f" in front=no*) INPUT_FRONT=no; INPUT_READY="${INPUT_READY:+$INPUT_READY; }not in front after $k tries: ${f%% input:*}";;
	front=*) ;;
	*) INPUT_FRONT=unknown; INPUT_READY="${INPUT_READY:+$INPUT_READY; }the dev channel gave no answer to focus";; esac
	[ "$INPUT_FRONT" = yes ] && [ "$k" -gt 1 ] && INPUT_READY="${INPUT_READY:+$INPUT_READY; }brought to the front (try $k)"
	return 0
}
# What a probe that failed twice says. With the window in front and on a screen, a click or key that still does nothing is real
# dead input (a game whose player input died at its first load, about one fresh game in three, which only a restart mends and
# rig.sh join restarts; or a real regression); one that would not come to the front is the desktop's or a build without the fix.
RIG_INPUT_DEAD="its window was in front and on a screen, and its input still never reached the player"
RIG_INPUT_NOFRONT="its window would not stay in front and on a screen (another window kept taking it, or it is parked off every screen and the DLL can't move it): no input goes to a game that is not in front"
rig_input_why() { case "$INPUT_FRONT" in no) echo "$RIG_INPUT_NOFRONT" ;; unknown) echo "the dev channel gave no answer to focus, so whether its window was in front is not known" ;; *) echo "$RIG_INPUT_DEAD" ;; esac; }
# rig_click_fires <who> <label>: one real click fires that game's gun (`net status` shots counts its player's attacks); the game
# made ready first (rig_input_ready) and the cursor put where a click reaches it (click_spot), and once more before a second
# click if the first fired nothing. A failure says which kind it was (click_dead_why).
# mouse_why <who>: what that game's raw input looked like (the `mousedev` answer), for a click that fired nothing; empty from a build
# without the command
mouse_why() { local m; m=$(dc "$1" mousedev 2>/dev/null); case "$m" in raw=*) printf ' [mousedev: %s]' "$m" ;; esac; }
# click_spot <who>: before a click on the host's game, the cursor is put where a click reaches the game (input.ps1 -Spot): the
# dev channel's `click` presses the button wherever the cursor is when that is a free point of the game's window (and moves it to
# a free point of the window when it is not, or refuses the click when it finds none), and a window kept on top of every other
# one covers part of the game even while the game is in front. On 2026-11-10 a system error box ("cl.exe - Application Error", a compiler crash) sat in the middle of the host's
# screen, right where the cursor was: every host click landed on it and took the front from the game, which saw the button go
# down in its raw input and never fire (`mousedev`: leftDown rising, front=no), while its keys still moved the player. PR 381's
# hits run stopped on it twice. CLICK_SPOT is input.ps1's line (spot=ok, moved, covered, offscreen or nogame; spot=unknown when
# input.ps1 gave no answer twice). Returns 0 only for ok and moved: on anything else the caller sends NO click (the host is the
# maintainer's PC, and a click there lands on whatever covers the game: an error box's button, another app). The cursor stays
# where it is put, so one call places the clicks after it too. The VM's desktop has nothing over its game: a no-op there.
_spot_probe() { powershell -NoProfile -ExecutionPolicy Bypass -File "$INPUT_PS1" -Spot 2>/dev/null | tr -d '\r' | grep '^spot=' | tail -1; }
click_spot() {
	CLICK_SPOT=""; [ "$1" = host ] || return 0
	CLICK_SPOT=$(_spot_probe); [ -n "$CLICK_SPOT" ] || { sleep 1; CLICK_SPOT=$(_spot_probe); }
	case "$CLICK_SPOT" in spot=ok*|spot=moved*) return 0 ;; "") CLICK_SPOT="spot=unknown (input.ps1 -Spot gave no answer, twice)" ;; esac
	return 1
}
# spot_why: why click_spot said no, in words (CLICK_SPOT names the covering window's process and window class, never its title)
spot_why() {
	case "$CLICK_SPOT" in
	spot=covered*) echo "another window covers the host's game, so a click would land on that window: none was sent ($CLICK_SPOT). Close that window; restarting the game would not help" ;;
	spot=offscreen*) echo "the host's game window is parked off every screen (the dev channel's focus did not bring it back), so a click would land on whatever is under the cursor: none was sent ($CLICK_SPOT)" ;;
	spot=nogame*) echo "the host's game has no window to click: none was sent ($CLICK_SPOT)" ;;
	*) echo "where a click would land on the host is not known: none was sent ($CLICK_SPOT)" ;;
	esac
}
# spot_or_broken <who> <label>: click_spot before a run of the dev channel's clicks (a phase of hits.sh, a trails burst). When it
# says a click would not reach the host's game: a rig-broken input failure (spot_why, naming what covers it) and 1, and the
# caller sends none of its clicks.
spot_or_broken() { click_spot "$1" && return 0; rig_broken input "$2" "$(spot_why)"; return 1; }
# host_input <label> <input.ps1 args...>: real input on the host's game through tools/input.ps1 (-Click, -Key ...). input.ps1
# sends nothing and prints one line starting input=refused (exit 2 to 4) when the game is not running, no point of its client
# area is free or its window is off every screen (a click), or the game is not in front (a key). That is the rig's doing, not the
# mod's: a rig-broken input failure quoting the line, and 1, so the caller stops its clicks or throws instead of reading a step
# with no shot, no throw or health that agrees trivially as the mod's failure. 0 when the input went. input.ps1 ending with no
# line and a non-zero exit is said as such. The VM's input goes through the dev channel, never this.
_input_run() { local o; o=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$INPUT_PS1" "$@" 2>/dev/null); local r=$?; printf '%s\n' "$o" | tr -d '\r'; return $r; }
host_input() {
	local label=$1 out rc refused; shift
	out=$(_input_run "$@"); rc=$?
	refused=$(grep -m1 '^input=refused' <<<"$out")
	if [ -n "$refused" ]; then rig_broken input "$label" "the host's input was refused, so nothing was sent: $refused"; return 1; fi
	if [ "$rc" -ne 0 ]; then rig_broken input "$label" "input.ps1 $* exited $rc without saying whether anything was sent$( [ -n "$out" ] && echo " (its last line: $(tail -1 <<<"$out"))")"; return 1; fi
	return 0
}
# _click_fires_ok <who>: rig_click_fires without a record: the game made ready (rig_input_ready), the cursor put on a free point
# (click_spot), one real click; a second try if the shots did not rise. 0 = it fired. CLICK_S0/CLICK_S1 the shots before and
# after the last click, CLICK_TRY its try, CLICK_NOTE what was done before each. When click_spot says no, nothing is clicked
# (CLICK_SENT stays empty for that try) and it returns 1 at once.
_click_fires_ok() {
	local try
	CLICK_NOTE=""; CLICK_S0=""; CLICK_S1=""; CLICK_SENT=""
	for try in 1 2; do
		CLICK_TRY=$try
		rig_input_ready "$1"; [ -n "$INPUT_READY" ] && CLICK_NOTE="$CLICK_NOTE${CLICK_NOTE:+; }before click $try: $INPUT_READY"
		click_spot "$1" || return 1
		case "$CLICK_SPOT" in spot=moved*) CLICK_NOTE="$CLICK_NOTE${CLICK_NOTE:+; }before click $try: $CLICK_SPOT" ;; esac
		sleep 1; CLICK_S0=$(field "$(dc "$1" 'net status')" shots); CLICK_SENT=1; dc "$1" "click 120" >/dev/null; sleep 2; CLICK_S1=$(field "$(dc "$1" 'net status')" shots)
		is_num "$CLICK_S0" && is_num "$CLICK_S1" && [ "$CLICK_S1" -gt "$CLICK_S0" ] && return 0
	done
	return 1
}
# click_dead_why <who>: after _click_fires_ok failed, what kind of failure it was. CLICK_DEAD is one of
#   covered    no point of the game's window was free (CLICK_SPOT names what covers it): no click was sent; close that
#              window; a restart can't help
#   offscreen  the window is parked off every screen: no click was sent
#   nospot     the game has no window, or where a click would land is not known: no click was sent
#   front      the window would not stay in front, or the click took the front from it (`mousedev` front=no right after: the
#              click landed on another window)
#   dead       in front, a free point under the cursor, and the press still never fired: a game whose mouse died (a restart
#              mends it)
# CLICK_WHY says it in words, with the game's own raw input counts (`mousedev`) when its build has the command.
click_dead_why() {
	local m; m=$(dc "$1" mousedev 2>/dev/null); case "$m" in raw=*) ;; *) m="" ;; esac
	if [ "$1" = host ] && [ -n "$CLICK_SPOT" ] && [[ $CLICK_SPOT != spot=ok* && $CLICK_SPOT != spot=moved* ]]; then
		case "$CLICK_SPOT" in spot=covered*) CLICK_DEAD=covered ;; spot=offscreen*) CLICK_DEAD=offscreen ;; *) CLICK_DEAD=nospot ;; esac
		CLICK_WHY=$(spot_why)
	elif [ "$INPUT_FRONT" != yes ]; then
		CLICK_DEAD=front; CLICK_WHY=$(rig_input_why)
	elif [[ $m == *" front=no "* ]]; then
		CLICK_DEAD=front; CLICK_WHY="its window was in front before the click and not after it: the click landed on another window, which took the front"
	else
		CLICK_DEAD=dead; CLICK_WHY="$RIG_INPUT_DEAD (a dead mouse: only a restart of the game mends it)"
	fi
	[ -n "$m" ] && CLICK_WHY="$CLICK_WHY [mousedev: $m]"
	return 0
}
rig_click_fires() {
	if _click_fires_ok "$1"; then pass "$2" "shots $CLICK_S0 -> $CLICK_S1$( [ "$CLICK_TRY" -gt 1 ] && echo " on the second click")${CLICK_NOTE:+ ($CLICK_NOTE)}"; return 0; fi
	click_dead_why "$1"
	case "$CLICK_DEAD" in covered|offscreen|nospot) rig_broken input "$2" "$1's click was not sent (try $CLICK_TRY)${CLICK_NOTE:+ ($CLICK_NOTE)}: $CLICK_WHY"; return 1 ;; esac
	rig_broken input "$2" "$1's click fired nothing, twice (shots '$CLICK_S0' -> '$CLICK_S1')${CLICK_NOTE:+ ($CLICK_NOTE)}: $CLICK_WHY"; return 1
}
# host_busy: the build and test processes running on the host now, counted ("cl.exe x12 f4mptests.exe x1"; empty when none).
# rig_host_load <label>: a WARN naming them, a PASS when there are none. The host is the maintainer's PC, shared with other
# agents' builds: on 2026-11-10 parallel builds starved it (cl.exe crashed five times, Windows warned of exhausted resources),
# the VM froze, and one compiler crash left its error box on top of the host's game, where it took the game's clicks.
host_busy() {
	tasklist //FO CSV //NH 2>/dev/null | tr -d '\r' | awk -F'","' '{n=tolower(substr($1,2))} n=="cl.exe"||n=="link.exe"||n=="f4mptests.exe"||n=="xmake.exe"||n=="msbuild.exe"{c[n]++} END{for(k in c) printf "%s x%d ", k, c[k]}' | sed 's/ $//'
}
rig_host_load() {
	local b; b=$(host_busy)
	if [ -n "$b" ]; then warn "$1" "builds or tests run on the host now ($b): its games hitch and a compiler crash can leave an error box over the host's game; a failure from here on may be the load's"
	else pass "$1" "no build or test runs on the host"; fi
}
# rig_keys_move <who> <label>: a real key press (forward, 0.8 s) moves that game's player at least 20 u; made ready and tried
# twice as rig_click_fires
rig_keys_move() {
	local p0 p1 d try note=""
	for try in 1 2; do
		rig_input_ready "$1"; [ -n "$INPUT_READY" ] && note="$note${note:+; }before press $try: $INPUT_READY"
		sleep 1; p0=$(dc "$1" pos); dc "$1" "key 0x57 800" >/dev/null; sleep 1.6; p1=$(dc "$1" pos)
		d=$(awk -v a="$(field "$p0" x)" -v b="$(field "$p0" y)" -v c="$(field "$p1" x)" -v e="$(field "$p1" y)" 'BEGIN{if (a=="" || c=="") {print ""; exit}; printf "%d", sqrt((c-a)^2 + (e-b)^2)}')
		if is_num "$d" && [ "$d" -ge 20 ]; then pass "$2" "moved $d u$( [ $try -gt 1 ] && echo " on the second press")${note:+ ($note)}"; return 0; fi
	done
	rig_broken input "$2" "$1's player moved '${d}' u on a forward key, twice${note:+ ($note)}: $(rig_input_why)"; return 1
}

is_num() { [[ $1 =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; }
_cmp() { awk -v a="$1" -v b="$3" "BEGIN{exit !(a $2 b)}"; }  # _cmp a <= b

expect_eq() { if [ "$2" = "$3" ]; then pass "$1" "$2"; else fail "$1" "got '$2', want '$3'"; fi; }
expect_zero() { expect_eq "$1" "$2" 0; }
expect_match() { if [[ $2 =~ $3 ]]; then pass "$1" "$2"; else fail "$1" "got '$2', want /$3/"; fi; }
expect_le() {  # expect_le <label> <got> <max> [unit]
	if ! is_num "$2"; then fail "$1" "no number to compare (got '$2'), want at most $3${4:+ $4}"; return; fi
	if _cmp "$2" '<=' "$3"; then pass "$1" "$2${4:+ $4} (at most $3)"; else fail "$1" "$2${4:+ $4}, want at most $3"; fi
}
expect_ge() {  # expect_ge <label> <got> <min> [unit]
	if ! is_num "$2"; then fail "$1" "no number to compare (got '$2'), want at least $3${4:+ $4}"; return; fi
	if _cmp "$2" '>=' "$3"; then pass "$1" "$2${4:+ $4} (at least $3)"; else fail "$1" "$2${4:+ $4}, want at least $3"; fi
}
expect_between() {  # expect_between <label> <got> <lo> <hi> [unit]
	if ! is_num "$2"; then fail "$1" "no number to compare (got '$2'), want $3 to $4${5:+ $5}"; return; fi
	if _cmp "$2" '>=' "$3" && _cmp "$2" '<=' "$4"; then pass "$1" "$2${5:+ $5} ($3 to $4)"; else fail "$1" "$2${5:+ $5}, want $3 to $4"; fi
}
warn_gt() {  # warn_gt <label> <got> <soft limit> [unit]: a WARN above the limit, a PASS otherwise
	if ! is_num "$2"; then warn "$1" "no number (got '$2')"; return; fi
	if _cmp "$2" '>' "$3"; then warn "$1" "$2${4:+ $4}, over the soft limit $3"; else pass "$1" "$2${4:+ $4}"; fi
}

# --- reading values ----------------------------------------------------------------------------------------
field() { grep -oE "(^|[] [(;|,:])$2=[^] ;|,)]*" <<<"$1" | head -1 | sed 's/^[^=]*=//'; }
part() { cut -d/ -f"$2" <<<"$1"; }
ns() { NS[$1]=$(dc "$1" 'net status'); }
nsf() { field "${NS[$1]}" "$2"; }
dcf() { field "$(dc "$1" "$2")" "$3"; }
expect_same() {  # expect_same <label> <command> <field> [tolerance]: the same field in both games
	local h v; h=$(dcf host "$2" "$3"); v=$(dcf vm "$2" "$3")
	if ! is_num "$h" || ! is_num "$v"; then fail "$1" "host '$h' vm '$v': not both numbers"; return; fi
	if awk -v a="$h" -v b="$v" -v t="${4:-0}" 'BEGIN{d=a-b; if (d<0) d=-d; exit !(d<=t)}'; then pass "$1" "host $h vm $v"; else fail "$1" "host $h vm $v (more than ${4:-0} apart)"; fi
}

# --- the plugin log ----------------------------------------------------------------------------------------
# A game's log is F4Multiplayer.log; each start moves it to .1, .2 and .3. A mark remembers the size and the first
# line of the current file; the delta is whatever came after, even when the game restarted (and rotated it) since.
_log_names="F4Multiplayer.log F4Multiplayer.1.log F4Multiplayer.2.log F4Multiplayer.3.log"
_log_fetch() {  # _log_fetch <who> <dir>: newest first, as 0.log 1.log 2.log 3.log
	local n=0 f; mkdir -p "$2"; rm -f "$2"/?.log
	for f in $_log_names; do
		if [ "$1" = host ]; then cp "$HOST_F4SE/$f" "$2/$n.log" 2>/dev/null
		else
			# The newest log always exists: a failed copy of it is a transient scp failure, tried once more (2026-11-12: encounter-fight
			# read no run standing in the VM's log, and its next read a few seconds later saw the run shared 30 s before: most likely one).
			scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$VM_SSH:$VM_F4SE/$f" "$2/$n.log" 2>/dev/null ||
				{ [ $n = 0 ] && sleep 2 && scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$VM_SSH:$VM_F4SE/$f" "$2/$n.log" 2>/dev/null; }
		fi
		n=$((n + 1))
	done
}
log_mark() {
	_log_fetch "$1" "$A_OUT/logmark-$1"
	MARK_SIZE[$1]=$(stat -c %s "$A_OUT/logmark-$1/0.log" 2>/dev/null || echo 0)
	MARK_FIRST[$1]=$(head -1 "$A_OUT/logmark-$1/0.log" 2>/dev/null)
}
_log_delta() {  # _log_delta <who> <dir with 0.log..3.log> : the lines since the mark, to stdout
	local d=$2 i=0 hit=-1
	for i in 0 1 2 3; do [ -s "$d/$i.log" ] && [ "$(head -1 "$d/$i.log")" = "${MARK_FIRST[$1]}" ] && { hit=$i; break; }; done
	if [ $hit -lt 0 ]; then cat "$d/0.log" 2>/dev/null; return; fi   # rotated out of sight: all of the newest file
	tail -c +$((MARK_SIZE[$1] + 1)) "$d/$hit.log"
	for ((i = hit - 1; i >= 0; i--)); do cat "$d/$i.log"; done
}
log_delta_file() {  # log_delta_file <who>: fetches the log now and writes the delta to $A_OUT/logdelta-<who>.txt
	_log_fetch "$1" "$A_OUT/lognow-$1"; _log_delta "$1" "$A_OUT/lognow-$1" > "$A_OUT/logdelta-$1.txt"
	echo "$A_OUT/logdelta-$1.txt"
}
_prefixes() {  # _prefixes <file> <level>: "Prefix xN" per distinct prefix, most first (never the message text)
	grep -E "^\[[^]]*\] \[[^]]*\] \[$2\] " "$1" | sed -E "s/^\[[^]]*\] \[[^]]*\] \[$2\] ([A-Za-z0-9_ .-]+):.*/\1/;t;s/^.*\] \[$2\] (.{0,24}).*/\1/" | sort | uniq -c | sort -rn | awk '{c=$1; $1=""; sub(/^ /,""); printf "%s x%d; ", $0, c}'
}
FUZZ_LEDGER_LINE="\] \[W\] WorldSync: the server's log ends at #[0-9]+; let go of [0-9]+ waiting container changes numbered past it"
log_check() {  # log_check <who> <label>
	local f e w a wf x=0 xs=""; f=$(log_delta_file "$1")
	if [ ! -s "$f" ]; then fail_mod "$2 plugin log" "no new lines since the mark: the log was not read (or the game wrote nothing)"; return; fi
	hang_check "$1" "$f"   # each hang by name (its Hang lines are in the warning count below as well)
	e=$(grep -cE '^\[[^]]*\] \[[^]]*\] \[E\] ' "$f")
	a=$(grep -ciE 'assert(ion)? (failed|failure)|ASSERT:|assertion failed' "$f")
	# Warnings: the fuzz step's own WorldSync line (asserted there) is left out once that step has run; nothing else is.
	wf=$f
	if [ -n "${FUZZ_RAN:-}" ]; then
		x=$(grep -cE "$FUZZ_LEDGER_LINE" "$f"); [ "$x" -gt 1 ] && x=1
		if [ "$x" -gt 0 ]; then wf="$f.warn"; RE="$FUZZ_LEDGER_LINE" awk 'n==0 && $0 ~ ENVIRON["RE"] {n=1; next} {print}' "$f" > "$wf"; xs=" (the fuzz step's own container line left out)"; fi
	fi
	w=$(grep -cE '^\[[^]]*\] \[[^]]*\] \[W\] ' "$wf")
	if [ "$e" -gt 0 ]; then fail_mod "$2 plugin log errors" "$e new [E] line(s): $(_prefixes "$f" E)(text in $f)"; else pass "$2 plugin log errors" "none in $(wc -l < "$f") new lines"; fi
	if [ "$a" -gt 0 ]; then fail_mod "$2 plugin log asserts" "$a line(s) with assertion text (in $f)"; else pass "$2 plugin log asserts" "none"; fi
	if [ "$w" -gt 0 ]; then warn "$2 plugin log warnings" "$w new [W] line(s): $(_prefixes "$wf" W)$xs"; else pass "$2 plugin log warnings" "none$xs"; fi
}
# --- a game that hung --------------------------------------------------------------------------------------------
# The plugin's hang watch logs a main thread that has not read its window messages for 15 s (four times that in a save, a
# load or a loading screen): "[W] Hang: the game's main thread (thread N) hasn't read its window messages for N s, <what
# it was doing>", then where it was ("it was at <module+offset>; return addresses on its stack: ..."), the other threads
# that were busy, and "[I] Hang: the game answers again, after about N s". A game that hangs answers no dev command and
# moves nobody meanwhile, so a check made then fails whatever the mod did. In the rig's runs of 2026-11-08 to 11-14 there
# were 18 hangs, most of them the VM's game (in win32u, the graphics driver, Steam's overlay); four sat beside failed
# checks ("got ''", "vm=DOWN", "not in the game after 60 s"), and all the run said was "Hang x5" among the end check's
# warnings. hang_check reads them from a game's log delta and says each one by name, with whose it is:
#   - the main thread was in the mod's code (it was at F4Multiplayer.dll+...): a FAIL, the hang is the mod's to explain;
#   - the mod's code among the return addresses, or where a busy thread was: a WARN naming the frame. The mod's hooks wrap
#     engine functions, so that alone proves nothing, but the hang is not taken as the rig's: the failures beside it stand;
#   - nothing of the mod in what the watch logged, but what it logged is not the whole stack: no place for the main thread
#     (the watch could not look at it) or one outside every module, no return address, as many return addresses as the
#     watch logs (HANG_CALLERS, 16: the mod's code may be further down), a busy thread at an unknown place, or a stack
#     the watch did not read to its base (it copies at most 64 KB from the stack pointer up and says how much it read:
#     "its stack read to its base (N KB)" or "only the top N KB of its M KB stack read"; a plugin from before it said
#     that says nothing, and is taken as not read to the base), the main thread's or a busy thread's; or a hang whose
#     lines may be cut: none on the other threads (busy ones, or "no other thread ... busy"), no last line (the report
#     file's "wrote", "couldn't write" or "no report file", or the answer: the log read while it was written), or a
#     return address that is not a module and offset. Whose hang it is can't be said: a WARN with the reason, and the
#     failures beside it stand;
#   - the place known, the stack read to its base, a list of return addresses that ends before the watch's limit, and
#     nothing of the mod in it: taken as the rig's (the machine, its graphics, the overlay). The checks that read that
#     game (a dev command to it among their readings) and failed while it hung are set aside (VOID "[was FAIL] ...",
#     listed in the summary under "set aside") and the hang is a RIG line of signal hang whose name never passes, so the
#     run's verdict is RIG: not a PASS, and not the mod's FAIL. Those checks were not made; nothing is made again here (a
#     step run again is smoke.sh's). Never set aside: the mod's own verdicts (a crash, a log error or assert, private
#     text: fail_mod, mod_check), and a check that read only the other game or made no dev call of its own; they
#     stay FAILs and are named. With no failure set aside, a WARN with the same facts.
#     What even this can't see: a hang the mod causes from another thread that is not busy (a lock of the mod's that the
#     main thread waits on) leaves nothing of the mod on the main thread's stack. That is why only the hung game's own
#     checks are set aside, the verdict is RIG and never a PASS, and every set-aside check is named, to be run again.
# "The failures stand" holds for the channel rule too: in the first three cases the hung game's rig fault (channel) lines
# of that time are FAILs again (_channel_stand), since the silence was a hang that is not known to be the rig's.
# "While it hung": record notes each line's time (times.tsv: seconds, kind, name, mark, games read). A check spans from the line before it
# (HANG_LOOK s back at most: what a step does before its first line) to its own line; the hang spans from its first log
# line less the seconds that line says to its "answers again" line (to now when there is none), HANG_SLACK s wider each
# way for the two machines' clocks. The log's stamps are a time of day, taken to be in the host's zone; a hang before the
# run began (a log read whole, its mark rotated out of sight) is not the run's and is left to the warning count. A
# rig-broken FAIL and the line of a stopped or interrupted run stay as they are.
HANG_LOOK=${HANG_LOOK:-45}; HANG_SLACK=${HANG_SLACK:-3}; HANG_CALLERS=${HANG_CALLERS:-16}
# _hang_list <log delta>: a line per hang: its time, the seconds it says, what the game was doing, where the main thread was,
# its first return addresses, the mod's code (0 nowhere, and the stack known | 1 among the callers or where a busy thread
# was | 2 where it was | 3 nowhere in what was logged, which is not the whole stack), that frame (for 3: why it is not),
# the time it answered again and after how long ("-": not given)
_hang_list() {
	awk -v cap="$HANG_CALLERS" '
		function out(   why) {
			if (t == "") return
			if (mod == 0) {
				if (at == "") why = "the hang watch logged no place for the main thread"
				else if (at ~ /^0x/) why = "the main thread was outside every module (" at ")"
				else if (k == 0) why = "no return address of its stack was logged"
				else if (k >= cap) why = "the " k " return addresses logged are as many as the watch logs, so the mod could be further down the stack"
				else if (unk) why = "a busy thread was at an unknown place"
				else if (stk == "") why = "the hang watch did not say how much of the stack it read (a plugin from before it did), so the mod could be further down"
				else if (!whole) why = "the hang watch did not read the stack to its base (" stk "), so the mod could be further down"
				else if (bcut != "") why = "a busy thread'"'"'s stack was not read to its base (" bcut ")"
				else if (!timed) why = "the hang watch logged no line on the game'"'"'s other threads, so a busy one could be missed"
				else if (!done) why = "the hang'"'"'s lines stop before the watch'"'"'s last one (its report file'"'"'s): the log was read while it was written, or a line was cut"
				else if (badc != "") why = "a return address logged is not a module and offset (" badc "): a line cut short"
				if (why != "") { mod = 3; frame = why }
			}
			printf "%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\n", t, n, ph, (at == "" ? "?" : at), (from == "" ? "-" : from), mod, (frame == "" ? "-" : frame), (ans == "" ? "-" : ans), (after == "" ? "-" : after); t = "" }
		function modframe(s) { return match(s, /F4Multiplayer\.dll\+0x[0-9A-Fa-f]+/) ? substr(s, RSTART, RLENGTH) : "" }
		# stackread("<place>; <how much of its stack was read>; return addresses ..."): the middle part ("" when the plugin did not say)
		function stackread(s,   r) { r = s; if (!sub(/^[^;]*; /, "", r) || index(r, "return addresses on its stack: ") == 1) return ""; sub(/; return addresses on its stack: .*/, "", r); return r }
		{ sub(/\r$/, "") }
		!/\] Hang: / { next }
		{ ts = substr($1, 2, 8) }
		/\] \[W\] Hang: the game.s main thread .* for [0-9]+ s, / { out(); t = ts; n = $0; sub(/.* for /, "", n); ph = n; sub(/ s, .*/, "", n); sub(/^[0-9]+ s, /, "", ph); at = from = frame = ans = after = stk = bcut = badc = ""; mod = 0; k = 0; unk = 0; whole = 0; timed = 0; done = 0; next }
		t == "" { next }
		/\] \[W\] Hang: it was at / {
			s = $0; sub(/.*Hang: it was at /, "", s); at = s; sub(/;.*/, "", at)
			stk = stackread(s); whole = index(stk, "its stack read to its base (") == 1
			c = s; if (!sub(/.*return addresses on its stack: /, "", c) || c == "none found") c = ""
			k = split(c, a, ", "); from = (k >= 1 ? a[1] : "") (k >= 2 ? ", " a[2] : "") (k >= 3 ? ", " a[3] : "")
			for (i = 1; i <= k; i++) if (badc == "" && a[i] !~ /^[^ ]+\+0x[0-9A-Fa-f]+$/) badc = a[i]
			if (index(at, "F4Multiplayer.dll") == 1) { mod = 2; frame = at } else if ((fr = modframe(c)) != "") { if (mod < 1) mod = 1; frame = fr }
			next }
		/\] \[W\] Hang: thread [0-9]+ kept a processor / {
			if (mod < 1 && (fr = modframe($0)) != "") { mod = 1; frame = fr " (where a busy thread was)" } if ($0 ~ /meanwhile, at an unknown place/) unk = 1
			s = $0; sub(/.* busy meanwhile, at /, "", s); bs = stackread(s); nb = $0; sub(/.*Hang: /, "", nb); sub(/ kept a processor.*/, "", nb)
			if (bcut == "" && index(bs, "its stack read to its base (") != 1) bcut = nb ": " (bs == "" ? "not said" : bs)
			timed = 1
			next }
		/\] \[W\] Hang: no other thread of the game kept a processor busy/ { timed = 1; next }
		/\] \[W\] Hang: (wrote |couldn.t write |no report file)/ { done = 1; next }   # the last line of a hang: its report file
		/\] \[I\] Hang: the game answers again/ { done = 1; ans = ts; after = $0; sub(/.*after about /, "", after); sub(/ s.*/, "", after); out(); next }
		END { out() }' "$1"
}
# _hang_epoch <HH:MM:SS>: that time of day as seconds of the clock, within this run (today's, or yesterday's for a run over
# midnight); nothing when it is before the run began
_hang_epoch() {
	local e; e=$(date -d "$1" +%s 2>/dev/null) || return 0; _now
	[ "$e" -gt $((NOW + 60)) ] && e=$((e - 86400))
	[ "$e" -ge $((${A_EPOCH0:-0} - HANG_LOOK)) ] && echo "$e"
	return 0
}
# _hang_fails <from> <to> [void <who>]: the names, one a line, of the FAILs of checks made in that time; with void, those of
# them that read <who>'s game (a dev command to it among their readings, times.tsv) are set aside, and only those: never a
# line marked the mod's own verdict (a crash, a log error or assert, private text), and never a check that read only the
# other game or made no dev call of its own (what it failed on is not known to be the hung game's silence)
_hang_fails() {
	[ -s "${A_TIMES:-}" ] && [ -s "$A_RESULTS" ] || return 0
	local tmp; tmp=$(mktemp)
	awk -F'\t' -v OFS='\t' -v a="$1" -v b="$2" -v look="$HANG_LOOK" -v void="${3:-}" -v who="${4:-}" -v names="$tmp.names" '
		FNR == NR { k = $3 SUBSEP (++n[$3]); S[k] = (FNR > 1 && prev > $1 - look) ? prev : $1 - look; E[k] = $1; M[k] = $4; R[k] = " " $5 " "; prev = $1; next }
		{ k = $3 SUBSEP (++m[$3]) }
		$1 == "FAIL" && index($4, "rig broken (") != 1 && index($3, "run stopped") != 1 && $3 != "run interrupted" && (k in E) && S[k] <= b && E[k] >= a &&
			(void == "" || (M[k] != "mod" && who != "" && index(R[k], " " who " "))) {
			print $3 > names; if (void != "") { $4 = "[was FAIL] " $4; $1 = "VOID" } }
		{ print }' "$A_TIMES" "$A_RESULTS" > "$tmp" && [ -n "${3:-}" ] && cat "$tmp" > "$A_RESULTS"
	cat "$tmp.names" 2>/dev/null; rm -f "$tmp" "$tmp.names"
}
hang_check() {  # hang_check <who> <its log delta>: each hang of the run in it, by name (see above)
	local who=$1 t n ph at from mod frame ans after e a b k j names stand facts
	[ -s "$2" ] || return 0
	while IFS=$'\t' read -r t n ph at from mod frame ans after; do
		grep -qxF "$t" "$A_OUT/hangs-$who.txt" 2>/dev/null && continue   # said already (the log read twice)
		e=$(_hang_epoch "$t"); [ -n "$e" ] || continue; echo "$t" >> "$A_OUT/hangs-$who.txt"
		a=$((e - n - HANG_SLACK)); b=""; [ "$ans" != - ] && b=$(_hang_epoch "$ans"); [ -n "$b" ] || { _now; b=$NOW; }; b=$((b + HANG_SLACK))
		facts="its main thread read no window messages for $(if [ "$ans" != - ]; then echo "about $after s"; else echo "$n s and had not answered again when the log was read"; fi), $ph (from about $(date -d "@$((e - n))" +%T 2>/dev/null)), at $at$([ "$from" != - ] && echo " called from $from")"
		case "$mod" in
			2)	names=$(_hang_stand "$who" "$a" "$b"); k=$(grep -c . <<<"$names")
				fail_mod "$who's game hung in the mod's code ($t)" "$facts. The main thread was in the mod's code: the hang is the mod's to explain (the hang report next to the plugin log has every thread)$([ "$k" -gt 0 ] && echo "; $k check(s) failed while it hung and stand: $(_hang_names "$names")")" ;;
			1)	names=$(_hang_stand "$who" "$a" "$b"); k=$(grep -c . <<<"$names")
				warn "$who's game hung with the mod's code on its stack ($t)" "$facts; $frame is on it. The mod's hooks wrap engine functions, so that alone proves nothing, but the hang is not taken as the rig's$([ "$k" -gt 0 ] && echo "; $k check(s) failed while it hung and stand: $(_hang_names "$names")")" ;;
			3)	names=$(_hang_stand "$who" "$a" "$b"); k=$(grep -c . <<<"$names")
				warn "$who's game hung, and whose hang it is can't be said ($t)" "$facts; nothing of the mod in what the hang watch logged, but $frame: the hang is not taken as the rig's$([ "$k" -gt 0 ] && echo "; $k check(s) failed while it hung and stand: $(_hang_names "$names")")" ;;
			*)	names=$(_hang_fails "$a" "$b" void "$who"); k=$(grep -c . <<<"$names")
				stand=$(_hang_fails "$a" "$b"); j=$(grep -c . <<<"$stand")
				stand=$([ "$j" -gt 0 ] && echo "; $j other check(s) failed then and stand (the mod's own verdicts, and checks that read no dev command of $who's game): $(_hang_names "$stand")")
				if [ "$k" -gt 0 ]; then rig_fault hang "$who's game answered while its checks were made (it hung at $t)" "$k check(s) that read $who's game and failed while it hung are set aside, not made (run them again): $(_hang_names "$names"). The hang: $facts; nothing of the mod in what the hang watch logged, its stack read to its base$stand"
				else warn "$who's game hung ($t)" "$facts; nothing of the mod in what the hang watch logged, its stack read to its base, and no check that read $who's game failed while it hung$stand"; fi ;;
		esac
	done < <(_hang_list "$2")
	return 0
}
# _hang_stand <who> <from> <to>: a hang that is not known to be the rig's: that game's channel faults of that time are FAILs
# again; the names of the FAILs of that time, one a line
_hang_stand() { _channel_stand "$1" "$1's game hung then, and the hang is not known to be the rig's, so this failure stands" "$2" "$3" >/dev/null; _hang_fails "$2" "$3"; }
_hang_names() { awk 'NR <= 4 { printf "%s%s", (NR > 1 ? "; " : ""), $0 } END { if (NR > 4) printf "; and %d more", NR - 4 }' <<<"$1"; }
# _hang_at_stop <signal>: a run stopped by the breaker never reaches its end checks, so its hangs are said here, from both
# games' logs since the mark (not the VM's when ssh is what broke; nothing when no mark was made)
_hang_at_stop() {
	local g
	[ -n "${A_OUT:-}" ] || return 0
	for g in host vm; do
		[ -n "${MARK_FIRST[$g]+x}" ] || continue
		[ "$g" = vm ] && [ "$1" = ssh ] && continue
		hang_check "$g" "$(log_delta_file "$g" 2>/dev/null)"
	done
}
log_privacy() {  # log_privacy <who> <label>: the rc gate's scan, on the delta only; counts, never the matching text
	local f p s i pf="${F4MP_PRIVATE_PATTERNS:-$HOME/.f4mp-private-patterns}"; f=$A_OUT/logdelta-$1.txt
	[ -s "$f" ] || f=$(log_delta_file "$1")
	if [ -s "$pf" ]; then p=$(grep -ciF -f "$pf" "$f"); else p=""; fi
	s=$(grep -cE '7656[0-9]{13}' "$f")
	i=$(grep -oE '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b' "$f" | grep -vE '^127\.0\.0\.1$|^0\.0\.0\.0$' | sort -u | wc -l)
	if [ -z "$p" ]; then warn "$2 privacy: patterns file" "no private-patterns file at $pf: that scan was skipped"; else mod_check expect_zero "$2 privacy: private patterns" "$p"; fi
	mod_check expect_zero "$2 privacy: Steam ids" "$s"; mod_check expect_zero "$2 privacy: IP addresses other than 127.0.0.1" "$i"
}

# --- the verdict ---------------------------------------------------------------------------------------------
a_summary() {  # a_summary: prints the one-screen summary, writes $A_OUT/summary.txt, exits non-zero on a FAIL
	_reach_flush
	[ -n "$A_SECTION" ] && printf '%s\t%s\n' "$A_SECTION" "$((SECONDS - A_SEC_T0))" >> "$A_SECLOG"; A_SECTION=""
	local np nf nw nr nu nx nh nv nvf vnote total mins; np=$(grep -c '^PASS' "$A_RESULTS"); nf=$(grep -c '^FAIL' "$A_RESULTS"); nw=$(grep -c '^WARN' "$A_RESULTS"); nr=$(grep -c $'^RIG\t' "$A_RESULTS")
	nx=$(grep -c '^RIGFAULT' "$A_RESULTS"); nh=$(grep -c '^HEAL' "$A_RESULTS"); nv=$(grep -c '^VOID' "$A_RESULTS")
	# failures a re-run set aside after a rig fault: no verdict on the mod, but never out of sight (a PASS names them)
	nvf=$(awk -F'\t' '$1=="VOID" && index($4, "[was FAIL]") == 1 && index($4, "[was FAIL] rig broken (") != 1 {n++} END{print n+0}' "$A_RESULTS")
	vnote=""; [ "$nvf" -gt 0 ] && vnote="; $nvf failure(s) set aside after a rig fault: read them under \"set aside\""
	# rig faults whose check never passed in this run (by name; a soak's rounds repeat each name): those were never made
	nu=$(awk -F'\t' '$1=="PASS"{p[$3]=1} $1=="RIG"{r[$3]=1} END{n=0; for (k in r) if (!(k in p)) n++; print n}' "$A_RESULTS")
	total=$((SECONDS - A_T0)); mins=$(awk -v s=$total 'BEGIN{printf "%.1f", s/60}')
	{
		echo; echo "================ $A_NAME summary ================"
		echo "started $A_START, ran $mins min: $np passed, $nf FAILED, $nw warnings$([ "$nr" -gt 0 ] && echo ", $nr rig fault(s)")$([ "$nx" -gt 0 ] && echo ", $nx RIG FAULT(S) NOT HEALED")$([ "$nh" -gt 0 ] && echo ", $nh healed")$([ "$nv" -gt 0 ] && echo ", $nv set aside")"
		awk -F'\t' '{printf "  %-34s %3d s\n", $1, $2}' "$A_SECLOG"
		if [ "$nf" -gt 0 ]; then echo "FAILED (the mod's):"; awk -F'\t' '$1=="RIG" && index($4, "rig fault (setup)") == 1 {su[$2]=1} $1=="FAIL"{printf "  [%s] %s%s: %s\n", $2, (($2 in su) ? "(after a set-up fault of this section: it may follow from it) " : ""), $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nx" -gt 0 ]; then echo "RIG FAULTS NOT HEALED (the rig was broken; these checks were not made):"; awk -F'\t' '$1=="RIGFAULT"{printf "  %s%s: %s\n", ($2 == "" ? "" : "[" $2 "] "), $3, substr($4,1,230)}' "$A_RESULTS"; fi
		if [ "$nr" -gt 0 ]; then echo "rig faults (checks the rig could not make that time; no verdict on the mod):"; awk -F'\t' '$1=="RIG"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170); if (index($4, "rig fault (setup)") == 1) su=1} END{if (su) print "  (a set-up fault is one game'"'"'s own state that a console command or the rig'"'"'s input did not make; one that comes back the same when run again is to be looked into)"}' "$A_RESULTS"; fi
		if [ "$nh" -gt 0 ]; then echo "healed (the rig found in a wrong state and put right before going on):"; awk -F'\t' '$1=="HEAL"{printf "  %s%s: %s\n", ($2 == "" ? "" : "[" $2 "] "), $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nv" -gt 0 ]; then echo "set aside (an attempt that met a rig fault; the step was healed and run again): $nv line(s), $(awk -F'\t' '$1=="VOID" && index($4, "[was FAIL]") == 1 {n++} END{print n+0}' "$A_RESULTS") of them failures:"; awk -F'\t' '$1=="VOID" && index($4, "[was FAIL]") == 1 {printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nw" -gt 0 ]; then echo "warnings:"; awk -F'\t' '$1=="WARN"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nf" -gt 0 ]; then echo "VERDICT: FAIL ($A_NAME: $nf failed check(s)$([ "$nx" -gt 0 ] && echo "; and $nx rig fault(s) not healed"))"
		elif [ "$nx" -gt 0 ]; then echo "VERDICT: RIG ($A_NAME: no check failed, but $nx rig fault(s) were not healed and their checks were not made: fix the rig and run again$vnote)"
		elif [ "$nu" -gt 0 ]; then echo "VERDICT: RIG ($A_NAME: no check failed; $nu check(s) the rig never made, listed under rig faults: run them again$vnote)"
		elif [ "$np" -eq 0 ]; then echo "VERDICT: RIG ($A_NAME: no check was made, so nothing was tested: a run that tested nothing is never a PASS$vnote)"
		elif [ "$nr" -gt 0 ]; then echo "VERDICT: PASS ($A_NAME; $nr rig fault(s), each check made and passed in another round$vnote)"
		else echo "VERDICT: PASS ($A_NAME$([ "$nh" -gt 0 ] && echo "; $nh rig state(s) healed, every check made")$vnote)"; fi
		echo "details: $A_OUT"
	} | tee "$A_OUT/summary.txt"
	nf=$((nf + nu + nx)); [ "$nf" -gt 125 ] && nf=125
	[ "$nf" -eq 0 ] && [ "$np" -eq 0 ] && nf=1   # no check made: VERDICT RIG above, and never exit 0
	exit "$nf"
}

a_interrupted() {  # the INT/TERM trap of a run (trap a_interrupted INT TERM): an interrupted run is a FAILED run, never a PASS with fewer checks
	record FAIL "run interrupted" "stopped by a signal before the end: the checks after this point were not made"
	echo "interrupted"; a_summary
}

# --- self-test: the helpers without a game ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "$1" = selftest ]; then
	export F4MP_CHECKS=$(mktemp -d); CHECKS=$F4MP_CHECKS; A_OUT=""; a_init selftest >/dev/null; bad=0
	_spot_probe() { echo "spot=ok 1,1"; }   # never input.ps1 in the self-test: it moves the cursor
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	last() { tail -1 "$A_RESULTS" | cut -f1; }
	section s1 >/dev/null
	t "field plain" "$(field 'dead=false hp=57 owner=2' hp)" 57
	t "field negative" "$(field 'dead=true hp=-3' hp)" -3
	t "field slash" "$(field 'x explosions=5/4 ghosts=0 | y' explosions)" 5/4
	t "field first word" "$(field 'status=joined id=2' status)" joined
	t "field not a suffix" "$(field 'undead=1 dead=0' dead)" 0
	t "field absent" "$(field 'a=1' b)" ""
	t "field after bracket" "$(field 'players: [id=2 puppet=FF001' id)" 2
	t "part" "$(part 5/4/9 2)" 4
	expect_le a 3 5 u >/dev/null; t "le pass" "$(last)" PASS
	expect_le a 6 5 u >/dev/null; t "le fail" "$(last)" FAIL
	expect_le a "" 5 >/dev/null; t "le empty is a fail" "$(last)" FAIL
	expect_le a abc 5 >/dev/null; t "le text is a fail" "$(last)" FAIL
	expect_ge a 2.5 2 >/dev/null; t "ge float pass" "$(last)" PASS
	expect_ge a 1 2 >/dev/null; t "ge fail" "$(last)" FAIL
	expect_between a 3 1 5 >/dev/null; t "between pass" "$(last)" PASS
	expect_between a 9 1 5 >/dev/null; t "between fail" "$(last)" FAIL
	expect_eq a x x >/dev/null; t "eq pass" "$(last)" PASS
	expect_eq a x y >/dev/null; t "eq fail" "$(last)" FAIL
	expect_match a joined '^join' >/dev/null; t "match pass" "$(last)" PASS
	warn_gt a 9 5 >/dev/null; t "warn_gt over" "$(last)" WARN
	warn_gt a 4 5 >/dev/null; t "warn_gt under" "$(last)" PASS
	# the log delta, with a rotation in between
	d=$(mktemp -d); mk() { printf '%s\n' "$@"; }
	mk '[10:00:00.000] [1] [I] Boot: build x' '[10:00:01.000] [1] [I] A: one' > "$d/0.log"
	MARK_SIZE[t]=$(stat -c %s "$d/0.log"); MARK_FIRST[t]=$(head -1 "$d/0.log")
	mk '[10:00:00.000] [1] [I] Boot: build x' '[10:00:01.000] [1] [I] A: one' '[10:00:02.000] [1] [E] B: boom' '[10:00:03.000] [1] [W] C: hmm' > "$d/0.log"
	t "delta, same file" "$(_log_delta t "$d" | wc -l)" 2
	cp "$d/0.log" "$d/1.log"; mk '[11:00:00.000] [1] [I] Boot: build y' '[11:00:01.000] [1] [W] C: again' > "$d/0.log"
	t "delta across a rotation" "$(_log_delta t "$d" | wc -l)" 4
	rm -f "$d/1.log"; t "delta, mark rotated out of sight: the newest file" "$(_log_delta t "$d" | wc -l)" 2
	t "prefixes" "$(_prefixes "$d/0.log" W)" "C x1; "
	A_OUT=$A_OUT; ( a_summary >/dev/null ); r=$?; t "summary exit code = failures" "$r" "$(grep -c '^FAIL' "$A_RESULTS")"
	# an interrupted run must fail: a signal while nothing had failed yet used to end in VERDICT PASS and exit 0
	sd=$(mktemp -d); sigout=$( ( A_OUT=$sd; a_init sigtest >/dev/null; section s2 >/dev/null; pass "a check" ok >/dev/null; trap a_interrupted INT TERM; kill -TERM "$BASHPID"; sleep 1; echo "not reached" ) 2>&1 ); r=$?
	rm -rf "$sd"
	[ "$r" -ge 1 ] && echo "ok   interrupted run exits non-zero ($r)" || { echo "FAIL interrupted run exits non-zero: got $r"; bad=1; }
	case "$sigout" in *"VERDICT: FAIL"*"run interrupted"*|*"run interrupted"*"VERDICT: FAIL"*) echo "ok   interrupted run says VERDICT FAIL";; *) echo "FAIL interrupted run says VERDICT FAIL: [$sigout]"; bad=1;; esac
	# the circuit breaker: each case is a run of its own (a subshell with its own folder), its output and exit code read
	brk() {  # brk <body>: runs the body as a fresh run; prints "<exit code>|<output>"
		local o r sd; sd=$(mktemp -d); o=$( ( A_OUT=$sd; A_RIG_STOPPED=""; A_SECTION=""; a_init brk >/dev/null; section b1 >/dev/null; eval "$1"; echo "REACHED"; a_summary ) 2>&1 ); r=$?; rm -rf "$sd"; echo "$r|$o"
	}
	has() { case "$2" in *"$3"*) echo "ok   $1";; *) echo "FAIL $1: no [$3] in [${2:0:300}]"; bad=1;; esac; }
	hasnt() { case "$2" in *"$3"*) echo "FAIL $1: [$3] in [${2:0:300}]"; bad=1;; *) echo "ok   $1";; esac; }
	o=$(brk 'rig_broken input "c1" "no shots" >/dev/null')
	has "breaker: one rig-broken failure lets the run go on" "$o" REACHED; has "breaker: ... and it is a FAIL" "$o" "rig broken (input): no shots"
	o=$(brk 'fail "a mod check" "hp differs" >/dev/null; fail "another" "x" >/dev/null; rig_broken game "c1" "down" >/dev/null')
	has "breaker: mod failures never trip it" "$o" REACHED
	o=$(brk 'rig_broken game "c1" "host down" >/dev/null; rig_broken input "c2" "no shots"')
	hasnt "breaker: two rig-broken failures stop the run at once" "$o" REACHED; has "breaker: the stop line names the signal" "$o" "STOP: rig broken (input)"
	has "breaker: the stop is a FAIL check" "$o" "run stopped: rig broken (input)"; has "breaker: the verdict is FAIL" "$o" "VERDICT: FAIL"
	[ "${o%%|*}" -ge 1 ] 2>/dev/null && echo "ok   breaker: a stopped run exits non-zero" || { echo "FAIL breaker: a stopped run exits non-zero: [${o%%|*}]"; bad=1; }
	o=$(brk '( rig_broken ssh "c1" "x" >/dev/null ); ( rig_broken ssh "c2" "y" >/dev/null ); section b2')
	hasnt "breaker: trips made in subshells stop the run at the next section" "$o" REACHED; has "breaker: ... naming their signal" "$o" "STOP: rig broken (ssh)"
	# the probes, with the games and ssh faked
	o=$(brk 'alive() { echo "host=ingame=true vm=DOWN"; }; vmssh() { :; }; rig_up "up" >/dev/null')
	has "rig_up: VM down and ssh down is signal ssh" "$o" "rig broken (ssh)"
	o=$(brk 'alive() { echo "host=DOWN vm=ingame=true"; }; vmssh() { echo up; }; rig_up "up" >/dev/null')
	has "rig_up: a game down is signal game" "$o" "rig broken (game)"
	o=$(brk 'alive() { echo "host=ingame=true vm=ingame=true"; }; dc() { case "$1 $2" in "vm net status") echo "status=connecting id=0";; *) echo "status=joined id=1";; esac; }; rig_up "up" >/dev/null')
	has "rig_up: a game not connected is signal game" "$o" "not connected to the session"
	o=$(brk 'alive() { echo "host=ingame=true vm=ingame=true"; }; dc() { echo "status=joined id=1"; }; rig_up "up" >/dev/null')
	hasnt "rig_up: both up and joined is no failure" "$o" "rig broken"
	o=$(brk 'sleep() { :; }; dc() { case "$2" in "net status") echo "status=joined shots=3";; *) echo ok;; esac; }; rig_click_fires vm "fires" >/dev/null')
	has "rig_click_fires: shots that stay put are signal input" "$o" "rig broken (input): vm's click fired nothing"
	o=$(brk 'sleep() { :; }; cf=$(mktemp); echo 0 > "$cf"; dc() { case "$2" in "net status") n=$(( $(cat "$cf") + 1 )); echo $n > "$cf"; echo "status=joined shots=$n";; *) echo ok;; esac; }; rig_click_fires vm "fires" >/dev/null; rm -f "$cf"')
	hasnt "rig_click_fires: a shot that counts is no failure" "$o" "rig broken"
	o=$(brk 'sleep() { :; }; cf=$(mktemp); echo 0 > "$cf"; dc() { case "$2" in "net status") n=$(( $(cat "$cf") + 1 )); echo $n > "$cf"; [ $n -ge 4 ] && echo "status=joined shots=4" || echo "status=joined shots=3";; *) echo ok;; esac; }; rig_click_fires vm "fires"; rm -f "$cf"')
	hasnt "rig_click_fires: a second click that fires is no failure" "$o" "rig broken"; has "rig_click_fires: ... and says it took two" "$o" "on the second click"
	o=$(brk 'sleep() { :; }; kf=$(mktemp); dc() { case "$2" in menus) grep -q 0x0D "$kf" && echo "open=HUDMenu:2.0 longest=none" || echo "open=HUDMenu:1.0,MessageBoxMenu:0.5 longest=MessageBoxMenu:0.5";; key*) echo "$2" >> "$kf"; echo pressing;; "net status") echo x >> "$kf.n"; echo "status=joined shots=$(wc -l < "$kf.n")";; *) echo ok;; esac; }; rig_click_fires host "fires"; rm -f "$kf" "$kf.n"')
	hasnt "rig_input_ready: a message box left open is closed before the click" "$o" "rig broken"; has "rig_input_ready: ... and named" "$o" "closed MessageBoxMenu"
	# the click spot (input.ps1 -Spot, faked) and what kind of failure a click that fires nothing is
	# A covered, offscreen or unknown spot sends no click at all: the dev channel never sees `click` (every command dc gets is
	# logged), and the failure is rig broken (input), naming the covering window's process and class
	SPOT_COVERED="spot=covered (no point of the game's client area is free: covered by csrss [#32770] (kept on top); the cursor's point 1280,720 was under csrss [#32770] (kept on top))"
	spotrun() { brk 'sleep() { :; }; _spot_probe() { '"$1"'; }; dl=$(mktemp); dc() { echo "$2" >> "$dl"; case "$2" in focus) echo "front=yes input: x";; "net status") echo "status=joined shots=3";; *) echo ok;; esac; }; '"$2"'; echo "clicks=$(grep -c "^click" "$dl")"; grep -h "$3" "$A_RESULTS"; rm -f "$dl"'; }
	o=$(spotrun 'echo "$SPOT_COVERED"' 'rig_click_fires host "fires" >/dev/null' fires)
	has "click_spot: a covered game gets no click" "$o" "clicks=0"; has "click_spot: ... rig broken (input)" "$o" "rig broken (input): host's click was not sent"
	has "click_spot: ... naming the covering window's process and class" "$o" "covered by csrss [#32770] (kept on top)"; has "click_spot: ... and that a restart won't help" "$o" "restarting the game would not help"
	o=$(spotrun 'echo "spot=offscreen (the game'"'"'s window is at -32000, -32000: parked off every screen)"' 'rig_click_fires host "fires" >/dev/null' fires)
	has "click_spot: an offscreen game gets no click" "$o" "clicks=0"; has "click_spot: ... said as offscreen" "$o" "the host's game window is parked off every screen"; hasnt "click_spot: ... not as covered" "$o" "another window covers"
	o=$(spotrun ':' 'rig_click_fires host "fires" >/dev/null' fires)
	has "click_spot: no answer from input.ps1 gets no click" "$o" "clicks=0"; has "click_spot: ... said as not known" "$o" "spot=unknown"
	o=$(spotrun 'echo "$SPOT_COVERED"' 'spot_or_broken host "phase: the host'"'"'s cursor on its game" && dc host "click 80"' phase)
	has "spot_or_broken: a covered game: the caller clicks nothing" "$o" "clicks=0"; has "spot_or_broken: ... rig broken (input) naming it" "$o" "rig broken (input): another window covers the host's game"
	o=$(spotrun 'echo "spot=ok 1,1"' 'spot_or_broken host "phase" && dc host "click 80"' phase)
	has "spot_or_broken: a free game: the caller clicks (the log sees it)" "$o" "clicks=1"
	o=$(brk '_spot_probe() { echo "$SPOT_COVERED"; }; spot_or_broken vm "burst" && echo VM-OK')
	has "spot_or_broken: the VM is never held back" "$o" "VM-OK"
	o=$(brk 'sleep() { :; }; dc() { case "$2" in focus) echo "front=yes input: x";; "net status") echo "status=joined shots=3";; mousedev) echo "raw=1:2 flags=0x0 to=game front=no focus=none active=none hook=on wmInput mouse=2 leftDown=2 keys=4 fixed=0";; *) echo ok;; esac; }; rig_click_fires host "fires" >/dev/null; grep -h fires "$A_RESULTS"')
	has "click_dead_why: front lost to the click" "$o" "the click landed on another window"
	o=$(brk 'sleep() { :; }; dc() { case "$2" in focus) echo "front=yes input: x";; "net status") echo "status=joined shots=3";; mousedev) echo "raw=1:2 flags=0x0 to=game front=yes focus=game active=game hook=on wmInput mouse=4 leftDown=2 keys=4 fixed=0";; *) echo ok;; esac; }; rig_click_fires host "fires" >/dev/null; grep -h fires "$A_RESULTS"')
	has "click_dead_why: in front, free, nothing fired: a dead mouse" "$o" "a dead mouse"
	o=$(brk 'sleep() { :; }; _spot_probe() { echo "spot=moved 896,504 (the cursor'"'"'s point 1280,720 was under csrss '"'"'x'"'"' (kept on top))"; }; cf=$(mktemp); echo 0 > "$cf"; dc() { case "$2" in "net status") n=$(( $(cat "$cf") + 1 )); echo $n > "$cf"; echo "status=joined shots=$n";; *) echo ok;; esac; }; rig_click_fires host "fires"; rm -f "$cf"')
	hasnt "click_spot: a moved cursor is no failure" "$o" "rig broken"; has "click_spot: ... and the move is said" "$o" "spot=moved 896,504"
	o=$(brk '_spot_probe() { echo SHOULD-NOT-RUN; }; click_spot vm; echo "spot[$CLICK_SPOT]"')
	has "click_spot: a no-op for the VM" "$o" "spot[]"
	# input.ps1 called for a step's own click or key (host_input): a refusal is rig broken (input) with its line, and the caller stops
	o=$(brk '_input_run() { echo "input=refused (spot=covered (no point of the game'"'"'s client area is free: covered by csrss [#32770] (kept on top))): no click was sent"; return 3; }; host_input "combat: the host shoots" -Click -HoldMs 700 && echo WENT || echo STOPPED; grep -h combat "$A_RESULTS"')
	has "host_input: a refused click is rig broken (input)" "$o" "rig broken (input): the host's input was refused, so nothing was sent: input=refused (spot=covered"
	has "host_input: ... and the caller stops" "$o" "STOPPED"; hasnt "host_input: ... not went" "$o" "WENT"
	o=$(brk '_input_run() { echo "input=refused (press 1 of 1: the game is not the foreground window, powershell [ConsoleWindowClass] is): no key was sent"; return 4; }; host_input "grenade: the host throws" -Key 0x12 && echo WENT || echo STOPPED; grep -h grenade "$A_RESULTS"')
	has "host_input: a refused key is rig broken (input) naming the front window by process and class" "$o" "powershell [ConsoleWindowClass] is): no key was sent"; has "host_input: ... and the caller stops" "$o" "STOPPED"
	o=$(brk '_input_run() { echo "spot=moved 896,504 (the cursor'"'"'s point 1280,720 was under csrss [#32770] (kept on top))"; return 0; }; host_input "combat: the host shoots" -Click && echo WENT || echo STOPPED')
	has "host_input: a click that went (a moved cursor) is no failure" "$o" "WENT"; hasnt "host_input: ... nothing recorded" "$o" "rig broken"
	o=$(brk '_input_run() { return 0; }; host_input "combat: the host shoots" -Click && echo WENT || echo STOPPED')
	has "host_input: a silent success is no failure" "$o" "WENT"
	o=$(brk '_input_run() { echo "Add-Type : something"; return 1; }; host_input "combat: the host shoots" -Click -HoldMs 700 && echo WENT || echo STOPPED; grep -h combat "$A_RESULTS"')
	has "host_input: input.ps1 failing without a refusal line is rig broken (input)" "$o" "rig broken (input): input.ps1 -Click -HoldMs 700 exited 1 without saying"; has "host_input: ... with its last line" "$o" "(its last line: Add-Type : something)"; has "host_input: ... and the caller stops" "$o" "STOPPED"
	o=$(brk '_input_run() { echo "input=refused (Fallout 4 is not running): nothing was sent"; return 2; }; host_input "c1" -Click >/dev/null; host_input "c2" -Click')
	has "host_input: two refusals trip the circuit breaker" "$o" "STOP: rig broken (input)"; hasnt "host_input: ... and nothing after" "$o" REACHED
	o=$(brk 'host_busy() { echo "cl.exe x3"; }; rig_host_load "load" >/dev/null')
	has "rig_host_load: builds running is a WARN naming them" "$o" "cl.exe x3"
	o=$(brk 'sleep() { :; }; dc() { case "$2" in pos) echo "x=10.0 y=20.0 z=0";; *) echo ok;; esac; }; rig_keys_move host "moves" >/dev/null')
	has "rig_keys_move: no movement is signal input" "$o" "rig broken (input): host's player moved '0' u"
	# rig faults: no FAIL, never the breaker; PASS when each was made in another round, RIG (non-zero) when one never was
	o=$(brk 'pass "pa distance" ok >/dev/null; rig_fault clock "pa distance" "a clock was set" >/dev/null; rig_fault clock "pa distance" "again" >/dev/null; rig_fault clock "pa distance" "again" >/dev/null')
	has "rig_fault: three never trip the breaker" "$o" REACHED; has "rig_fault: listed" "$o" "rig fault (clock): a clock was set"
	has "rig_fault: made in another round is a PASS" "$o" "VERDICT: PASS (brk; 3 rig fault(s)"; [ "${o%%|*}" = 0 ] && echo "ok   rig_fault: ... exit 0" || { echo "FAIL rig_fault: made in another round exits 0: [${o%%|*}]"; bad=1; }
	o=$(brk 'pass "other" ok >/dev/null; rig_fault clock "pa distance" "a clock was set" >/dev/null')
	has "rig_fault: never made is VERDICT RIG" "$o" "VERDICT: RIG (brk: no check failed; 1 check(s)"; [ "${o%%|*}" = 1 ] && echo "ok   rig_fault: ... exit 1" || { echo "FAIL rig_fault: never made exits 1: [${o%%|*}]"; bad=1; }
	o=$(brk 'rig_fault clock "pa distance" "x" >/dev/null; fail "hp" "differs" >/dev/null')
	has "rig_fault: a FAIL still makes it FAIL" "$o" "VERDICT: FAIL (brk: 1 failed"
	# a run that made no check tested nothing: never a PASS (smoke.sh --only with a name no step had once said PASS, exit 0)
	o=$(brk 'echo "no step ran" >/dev/null')
	has "no checks: VERDICT RIG" "$o" "VERDICT: RIG (brk: no check was made"; hasnt "no checks: ... not PASS" "$o" "VERDICT: PASS"
	[ "${o%%|*}" = 1 ] && echo "ok   no checks: ... exit 1" || { echo "FAIL no checks: exit 1: [${o%%|*}]"; bad=1; }
	o=$(brk 'warn "a warning" "only that" >/dev/null; healed "state" "put right" >/dev/null')
	has "no checks: warnings and heals alone are VERDICT RIG too" "$o" "VERDICT: RIG (brk: no check was made"; [ "${o%%|*}" = 1 ] && echo "ok   no checks: ... exit 1" || { echo "FAIL no checks (warn): exit 1: [${o%%|*}]"; bad=1; }
	# a step's set-up that could not be made (hits.sh setup_fault): not a FAIL, never the breaker's, and the run is not a PASS
	o=$(brk 'pass "set up: raiders at a clear spot" 4 >/dev/null; rig_fault setup "kill: set-up: both shots reach the target" "at none of 3 spots" >/dev/null; rig_fault setup "pause: set-up: both shots reach the target" "at none of 3 spots" >/dev/null; rig_fault setup "round: set-up: the shot reaches the target" "blocked" >/dev/null')
	has "set-up faults: three never trip the breaker" "$o" REACHED; has "set-up faults: listed as the rig's" "$o" "rig fault (setup): at none of 3 spots"
	hasnt "set-up faults: none is a FAIL" "$o" "FAILED (the mod's)"
	has "set-up faults: steps never made are VERDICT RIG" "$o" "VERDICT: RIG (brk: no check failed; 3 check(s)"; [ "${o%%|*}" = 3 ] && echo "ok   set-up faults: ... exit 3" || { echo "FAIL set-up faults: steps never made exit 3: [${o%%|*}]"; bad=1; }
	o=$(brk 'rig_fault setup "kill: set-up: both shots reach the target" "at none of 3 spots" >/dev/null; fail "round: the hit took it over" "runner says 1" >/dev/null')
	has "set-up faults: a FAIL beside one is still VERDICT FAIL" "$o" "VERDICT: FAIL (brk: 1 failed"
	# a step says itself that its own set-up could not be made (setup_fault, or rig_setup before an expect): the step not run
	o=$(brk 'setup_fault "outcomes: set-up: the host shares CZ (sQuests = safe)" "got shared=safe (not shared: sQuests = story)" >/dev/null; rig_setup expect_eq "sharedws (real hooks) (grab): set-up: vm aims at 0001" "" ok >/dev/null; rig_setup expect_ge "set up (host runs, vm shoots): raiders at a clear spot" 3 4 >/dev/null; cat "$A_RESULTS"')
	has "set-up: a set-up the step says could not be made is a rig fault, with what it read" "$o" $'RIG\tb1\toutcomes: set-up: the host shares CZ (sQuests = safe)\trig fault (setup): this set-up could not be made, so what the step tests was not tested: got shared=safe (not shared: sQuests = story)'
	has "set-up: ... through rig_setup and an expect too" "$o" $'RIG\tb1\tsharedws (real hooks) (grab): set-up: vm aims at 0001\trig fault (setup)'; has "set-up: ... and an expect_ge" "$o" $'RIG\tb1\tset up (host runs, vm shoots): raiders at a clear spot\trig fault (setup)'
	hasnt "set-up: ... none a FAIL" "$o" "FAILED (the mod's)"; has "set-up: ... the summary says what a set-up fault is, and that one that comes back is to be looked into" "$o" "(a set-up fault is one game's own state that a console command or the rig's input did not make; one that comes back"
	has "set-up: ... and the run is VERDICT RIG, never a PASS" "$o" "VERDICT: RIG (brk: no check failed; 3 check(s)"
	# never by its name: a check that reads what the mod carried to the other game is a FAIL whatever it is called
	o=$(brk 'fail "outcomes: set-up: the host shares CZ (sQuests = safe)" "no" >/dev/null; expect_eq "sharedws (grab): set-up: vm aims at 0001" "" ok >/dev/null; fail "set up (host runs, vm shoots): raiders at a clear spot" "3" >/dev/null; fail "player sync: precondition: the VM'"'"'s game has the stand-in" "no stand-in" >/dev/null; fail "sharedws: precondition: the VM has its copy of the host'"'"'s generator" "no copy" >/dev/null; cat "$A_RESULTS"')
	has "set-up: a name alone never makes a rig fault" "$o" "VERDICT: FAIL (brk: 5 failed"; hasnt "set-up: ... no RIG line" "$o" "rig fault (setup)"
	has "set-up: a stand-in that is missing is a FAIL" "$o" $'FAIL\tb1\tplayer sync: precondition: the VM\'s game has the stand-in\tno stand-in'
	has "set-up: the VM without its copy of the host's piece is a FAIL" "$o" $'FAIL\tb1\tsharedws: precondition: the VM has its copy of the host\'s generator\tno copy'
	o=$(brk 'rig_setup expect_eq "a: set-up: the ini" 1 1 >/dev/null; expect_eq "b: the VM has it" 1 2 >/dev/null'); has "set-up: rig_setup holds for its own check only" "$o" "VERDICT: FAIL (brk: 1 failed"
	# the rig's own scripts: no plain fail or expect_* there has a name that says set-up (a set-up is said with setup_fault or rig_setup)
	n=$(sed -E 's/rig_setup (expect_[a-z]+|fail)[[:space:]]+"/RS "/g' "$RIG/steps.sh" "$RIG/hits.sh" "$RIG/state.sh" "$RIG/guard.sh" | grep -cE '(^|[^_a-zA-Z])(fail|fail_mod|expect_[a-z]+|warn_gt)[[:space:]]+"(([^"]*[ :)])?set-up: |set up[ :(])')
	t "set-up: no check of steps.sh, hits.sh, state.sh or guard.sh is a set-up by its name alone" "$n" 0
	# what a whole run showed can make a set-up fault a FAIL again (rig_fault_stands: by its signal and name, a re-run's too)
	o=$(brk 'setup_fault "round: set-up: the shooter'"'"'s shot reaches the target" "not in 3 tries" >/dev/null; setup_fault "kill: set-up: a pair inside the cooldown" "6 s apart" >/dev/null; rig_fault clock "round: set-up: the shooter'"'"'s shot reaches the target" "no offset" >/dev/null; printf "VOID\tb0\tround: set-up: the shooter'"'"'s shot reaches the target\t[was RIG] rig fault (setup): this set-up could not be made, so what the step tests was not tested: not in 2 tries\n" >> "$A_RESULTS"; rig_fault_stands setup "round: set-up: the shooter'"'"'s shot reaches the target" "no shot of vm'"'"'s reached a copy in the whole run"; cat "$A_RESULTS"')
	has "set-up: a set-up fault a whole run disproved is a FAIL again, with what it was" "$o" $'FAIL\tb1\tround: set-up: the shooter\'s shot reaches the target\tno shot of vm\'s reached a copy in the whole run (it was first taken as a setup fault: this set-up could not be made, so what the step tests was not tested: not in 3 tries)'
	has "set-up: ... one a re-run set aside too" "$o" $'FAIL\tb0\tround: set-up: the shooter\'s shot reaches the target\tno shot of vm\'s reached a copy in the whole run (it was first taken as a setup fault: this set-up could not be made, so what the step tests was not tested: not in 2 tries)'
	has "set-up: ... another check's set-up fault stays one" "$o" $'RIG\tb1\tkill: set-up: a pair inside the cooldown\trig fault (setup)'
	has "set-up: ... and another signal's fault of that name" "$o" $'RIG\tb1\tround: set-up: the shooter\'s shot reaches the target\trig fault (clock): no offset'
	has "set-up: ... the run is VERDICT FAIL" "$o" "VERDICT: FAIL (brk: 2 failed"
	o=$(brk 'setup_fault "x: set-up: a shot reaches" "no" >/dev/null; pass "x: set-up: a shot reaches" "yes" >/dev/null'); has "set-up: made in another round, the run passes and says so" "$o" "VERDICT: PASS (brk; 1 rig fault(s), each check made and passed in another round"
	o=$(brk 'fail "early: a check" "x" >/dev/null; setup_fault "x: set-up: a shot reaches" "no" >/dev/null; fail "x: the hit lands" "hp 100" >/dev/null; section b2 >/dev/null; fail "y: a check" "z" >/dev/null')
	has "set-up: a FAIL after its section's set-up fault says it may follow from it" "$o" "[b1] (after a set-up fault of this section: it may follow from it) x: the hit lands: hp 100"
	has "set-up: ... one made before it is not marked" "$o" "  [b1] early: a check: x"
	has "set-up: ... and it is still a FAIL" "$o" "VERDICT: FAIL (brk: 3 failed"; has "set-up: ... another section's FAIL is not marked" "$o" "  [b2] y: a check: z"
	# a check that fails on a dev command that got no answer (dcfast.sh's state, with the count of its call) is not made, not failed
	na() { printf '%s\t%s\ttook the command and gave no answer in 20 s (07:31:40, '"'"'net status'"'"')\n' "$(( $(date +%s) - ${2:-0} ))" "${3:-1}" > "$F4MP_DC_STATE.$1"; }   # na <host|vm> [s ago] [its call's count]
	calls() { local i; for ((i = 0; i < $1; i++)); do printf %s "${2:-b}"; done >> "$F4MP_DC_STATE.seq"; }   # calls <n> [h|v|b]: n more dev calls were made (to that game; b: both)
	o=$(brk 'calls 1; na vm; expect_eq "the VM counts one applied" "" "1" >/dev/null; cat "$A_RESULTS"')
	has "channel: a check that read a command the VM gave no answer to is a rig fault, with the command and what it read" "$o" $'RIG\tb1\tthe VM counts one applied\trig fault (channel): vm\'s game took the command and gave no answer in 20 s (07:31:40, \'net status\'), so this check was not made (it read: got \'\', want \'1\')'
	has "channel: ... the run is VERDICT RIG" "$o" "VERDICT: RIG (brk: no check failed; 1 check(s)"
	o=$(brk 'calls 2; na vm 0 2; na host; fail "c" "x" >/dev/null; cat "$A_RESULTS"'); has "channel: both games are named" "$o" "rig fault (channel): host's game took the command and gave no answer in 20 s (07:31:40, 'net status'); vm's game took"
	o=$(brk 'calls 1; na vm; rm -f "$F4MP_DC_STATE.vm"; fail "c" "x" >/dev/null'); has "channel: after the game's next answer a failure is a FAIL again" "$o" "VERDICT: FAIL (brk: 1 failed"
	o=$(brk 'calls 1; na vm 300; fail "c" "x" >/dev/null'); has "channel: a no-answer of five minutes ago is not this check's" "$o" "VERDICT: FAIL (brk: 1 failed"
	o=$(brk 'calls 1; na vm 50; fail "c" "x" >/dev/null'); has "channel: ... nor one of 50 s ago" "$o" "VERDICT: FAIL (brk: 1 failed"
	o=$(brk 'calls 1; na vm 25; fail "c" "x" >/dev/null'); has "channel: ... one of 25 s ago is (a command waits 20 s)" "$o" "VERDICT: RIG"
	# only the check that read the unanswered command: one made of later calls (the other game's) is the FAIL it is
	o=$(brk 'calls 3; pass "before" ok >/dev/null; calls 1; na host 0 4; fail "a: the host counts one" "got nothing" >/dev/null; fail "b: the same reply, another field" "got nothing" >/dev/null; calls 1; fail "c: the VM counts one" "got 0" >/dev/null; cat "$A_RESULTS"')
	has "channel: the check that read the unanswered command is a rig fault" "$o" $'RIG\tb1\ta: the host counts one\trig fault (channel): host'; has "channel: ... and one made of the same reply" "$o" $'RIG\tb1\tb: the same reply, another field\trig fault (channel)'
	has "channel: ... a check made of a later call is a FAIL, though the game has not answered since" "$o" $'FAIL\tb1\tc: the VM counts one\tgot 0'; has "channel: ... so the run is a FAIL" "$o" "VERDICT: FAIL (brk: 1 failed"
	o=$(brk 'calls 1; na host; calls 1; pass "the VM answered" ok >/dev/null; calls 1; fail "the VM counts one" "got 0" >/dev/null'); has "channel: a no-answer from before the last line's readings is not this check's" "$o" "VERDICT: FAIL (brk: 1 failed"
	o=$(brk 'calls 1; na vm; pass "c" "x" >/dev/null; warn "d" "y" >/dev/null'); has "channel: a check that passes meanwhile passes" "$o" "VERDICT: PASS"
	o=$(brk 'calls 1; na vm; rig_broken game "c1" "vm not in the game after 60 s" >/dev/null; cat "$A_RESULTS"'); has "channel: a rig-broken failure stays one (the breaker counts it)" "$o" $'FAIL\tb1\tc1\trig broken (game)'
	o=$(brk 'calls 1; na vm; a_interrupted'); has "channel: an interrupted run is still a FAIL" "$o" "VERDICT: FAIL (brk: 1 failed"
	o=$(brk 'calls 1; na vm; a_attempt_mark; fail "c" "got nothing" >/dev/null; a_attempt_rig'); has "channel: smoke.sh's re-run sees it as the attempt's rig fault" "$o" "c: rig fault (channel)"
	o=$(brk 'calls 1; na vm; rig_setup expect_eq "x: set-up: the ini" "" 1 >/dev/null; cat "$A_RESULTS"'); has "channel: a set-up that read the unanswered command says channel" "$o" $'RIG\tb1\tx: set-up: the ini\trig fault (channel)'
	# the mod's own verdicts read no dev command and are never a channel fault: a crash, the log's errors and asserts, private text
	o=$(brk 'calls 1; na host; na vm; fail_mod "run crash logs (vm)" "new: crash-1.log" >/dev/null; mod_check expect_zero "end privacy: Steam ids" 2 >/dev/null; cat "$A_RESULTS"')
	has "channel: fail_mod is a FAIL with a no-answer state present" "$o" $'FAIL\tb1\trun crash logs (vm)\tnew: crash-1.log'; has "channel: ... and so is an expect under mod_check" "$o" $'FAIL\tb1\tend privacy: Steam ids\t'
	printf '[12:00:00.000] [1] [E] Net: boom\r\n[12:00:01.000] [1] [W] Sync: Assertion failed: x\r\n[12:00:02.000] [1] [I] Net: 7656%013d\r\n' 1 > "$d/err.txt"
	o=$(brk 'log_delta_file() { echo "'"$d/err.txt"'"; }; F4MP_PRIVATE_PATTERNS=/nonexistent; calls 1; na host; na vm; log_check vm "end vm" >/dev/null; log_privacy vm "end vm" >/dev/null; cat "$A_RESULTS"')
	has "channel: the log's errors are a FAIL with a no-answer state present" "$o" $'FAIL\tb1\tend vm plugin log errors\t1 new [E] line(s)'; has "channel: ... its asserts" "$o" $'FAIL\tb1\tend vm plugin log asserts\t1 line(s)'
	has "channel: ... and its privacy scan" "$o" $'FAIL\tb1\tend vm privacy: Steam ids\t'; hasnt "channel: ... none of them a rig fault" "$o" "rig fault (channel)"
	o=$(brk 'log_delta_file() { echo "'"$d/none.txt"'"; }; calls 1; na vm; log_check vm "end vm" >/dev/null; cat "$A_RESULTS"'); has "channel: a log that could not be read is a FAIL too" "$o" $'FAIL\tb1\tend vm plugin log\tno new lines'
	# a game that crashed is a crash: crash_verdict (steps.sh's own, with the two machines' listings stubbed) is a FAIL with a
	# no-answer state present, and that game's channel faults are FAILs again (also one a re-run had set aside)
	crash_list_host() { echo ok; echo "crash-1.log 5"; }; crash_list_vm() { echo ok; echo "crash-1.log 5"; echo "crash-9.log 7"; }; crash_events_host() { echo ok; }; crash_events_vm() { echo ok; }
	eval "$(sed -n '/^crash_new() {/,/^}/p; /^crash_since() {/,/^}/p; /^crash_count() /p; /^crash_names() /p; /^crash_event_lines() /p' "$RIG/crashlib.sh")"; eval "$(sed -n '/^crash_verdict() {/,/^}/p' "$RIG/steps.sh")"
	C_HOST0=$'ok\ncrash-1.log 5'; C_VM0=$'ok\ncrash-1.log 5'; C_T0=1
	o=$(brk 'calls 1; na host; fail "the host sees the hit" "got nothing" >/dev/null; calls 1; na vm 0 2; fail "the VM sees the hit" "got nothing" >/dev/null; a_attempt_mark; fail "the VM sees it again" "got nothing" >/dev/null; a_attempt_void; crash_verdict run >/dev/null; cat "$A_RESULTS"')
	has "crash: a new crash log is a FAIL with a no-answer state present" "$o" $'FAIL\tb1\trun crash logs (vm)\tnew: crash-9.log'; has "crash: ... the other game has none" "$o" $'PASS\tb1\trun crash logs (host)'
	has "crash: the crashed game's channel fault is a FAIL again" "$o" $'FAIL\tb1\tthe VM sees the hit\tvm\'s game crashed in this run'; has "crash: ... one a re-run had set aside too" "$o" $'FAIL\tb1\tthe VM sees it again\tvm\'s game crashed in this run'
	has "crash: ... what it read is kept" "$o" "(it was first taken as a channel fault: vm's game took the command and gave no answer in 20 s (07:31:40, 'net status'), so this check was not made (it read: got nothing))"
	has "crash: the other game's channel fault stays one" "$o" $'RIG\tb1\tthe host sees the hit\trig fault (channel): host'; has "crash: ... and the run is a FAIL" "$o" "VERDICT: FAIL (brk: 3 failed"
	# a listing that could not be read is never "none new" (an --only run without preflight once passed after a VM crash): the
	# listing before unread, the logs written since the snapshot's time are new; nothing to compare with at all is RIG, not PASS
	o=$(brk 'crash_list_vm() { echo ok; echo "crash-1.log 1000"; echo "crash-9.log 5000"; }; C_VM0=""; C_T0=4000; crash_verdict run >/dev/null; cat "$A_RESULTS"')
	has "crash: before unread, a log written since the snapshot is a FAIL" "$o" $'FAIL\tb1\trun crash logs (vm)\tnew: crash-9.log'; hasnt "crash: ... an older one is not new" "$o" "crash-1.log"
	o=$(brk 'crash_list_vm() { echo ok; echo "crash-1.log 1000"; }; C_VM0=""; C_T0=4000; crash_verdict run >/dev/null; cat "$A_RESULTS"')
	has "crash: before unread, none written since is a PASS that says so" "$o" $'PASS\tb1\trun crash logs (vm)\tnone new, written since the snapshot: the listing before was unreadable'
	o=$(brk 'C_HOST0=""; C_VM0=""; C_T0=""; crash_verdict run >/dev/null; cat "$A_RESULTS"')
	has "crash: no snapshot taken is not made (host)" "$o" $'RIG\tb1\trun crash logs (host)\trig fault (setup): can\'t tell whether a crash log is new: no listing from before the run'
	has "crash: ... (vm)" "$o" $'RIG\tb1\trun crash logs (vm)\trig fault (setup): can\'t tell'; hasnt "crash: ... never a PASS" "$o" $'PASS\tb1\trun crash logs'; has "crash: ... the run is VERDICT RIG" "$o" "VERDICT: RIG"
	o=$(brk 'crash_list_vm() { :; }; crash_verdict run >/dev/null; cat "$A_RESULTS"')
	has "crash: the VM's listing unreadable now is not made" "$o" $'RIG\tb1\trun crash logs (vm)\trig fault (ssh): can\'t tell whether a crash log is new: the listing now is unreadable (ssh)'
	has "crash: ... the host's still passes" "$o" $'PASS\tb1\trun crash logs (host)'; has "crash: ... the run is VERDICT RIG" "$o" "VERDICT: RIG"
	unset -f crash_verdict
	# the state guard's records and smoke.sh's re-run after a rig fault: HEAL is no failure, a set-aside (VOID) FAIL is not the
	# mod's, a RIGFAULT (not healed) is never a PASS and counts toward the breaker
	o=$(brk 'healed "state before x: vm was in power armor" "climbed out" >/dev/null; pass "c" ok >/dev/null')
	has "healed: listed" "$o" "vm was in power armor"; has "healed: a PASS" "$o" "VERDICT: PASS"; [ "${o%%|*}" = 0 ] && echo "ok   healed: ... exit 0" || { echo "FAIL healed: exit 0: [${o%%|*}]"; bad=1; }
	# a heal or a re-run never erases a failure of the mod: a FAIL made before the rig line stands, even when the re-run passes
	o=$(brk 'a_attempt_mark; fail "trail" "3 of 6" >/dev/null; rig_fault walk "walk" "stuck" >/dev/null; a_attempt_rig >/dev/null && echo SAW-RIG; a_attempt_void; a_attempt_mark; pass "trail" "6 of 6" >/dev/null; pass "walk" ok >/dev/null')
	has "attempt: a rig fault since the mark is seen" "$o" SAW-RIG
	hasnt "attempt: a FAIL before the rig line is not set aside" "$o" "[was FAIL] 3 of 6"; has "attempt: ... it stands" "$o" "trail: 3 of 6"
	has "attempt: ... and the run is a FAIL" "$o" "VERDICT: FAIL"; [ "${o%%|*}" = 1 ] && echo "ok   attempt: ... exit 1" || { echo "FAIL attempt: exit 1: [${o%%|*}]"; bad=1; }
	# a FAIL after the rig line is set aside; made again and passed, the run passes and its verdict names what it set aside
	o=$(brk 'a_attempt_mark; rig_fault walk "walk back" "stuck" >/dev/null; fail "give back" "got it back" >/dev/null; a_attempt_void; a_attempt_mark; pass "walk back" ok >/dev/null; pass "give back" ok >/dev/null')
	has "attempt: a FAIL after the rig line is set aside" "$o" "[was FAIL] got it back"; has "attempt: ... made again, a PASS" "$o" "VERDICT: PASS"
	has "attempt: ... the PASS names the set-aside failure" "$o" "1 failure(s) set aside after a rig fault"; [ "${o%%|*}" = 0 ] && echo "ok   attempt: ... exit 0" || { echo "FAIL attempt: exit 0: [${o%%|*}]"; bad=1; }
	# a clock fault spoils only the clock's check: the step's other failures stand, the RIG line stays RIG
	o=$(brk 'a_attempt_mark; rig_fault clock "moving" "cannot line up" >/dev/null; fail "settled" "90 u" >/dev/null; a_attempt_void; a_attempt_mark; pass "moving" ok >/dev/null; pass "settled" ok >/dev/null')
	hasnt "attempt: a clock fault sets nothing aside" "$o" "[was FAIL] 90 u"; has "attempt: ... the FAIL after it stands" "$o" "VERDICT: FAIL"
	has "attempt: ... the clock line stays a rig fault" "$o" "rig fault (clock): cannot line up"
	# the second attempt: a check that failed in both attempts stays a FAIL, rig lines or not
	o=$(brk 'a_attempt_mark; f1=$A_ATTEMPT_LINE; rig_fault walk "w" "y" >/dev/null; fail "take" "no claim" >/dev/null; a_attempt_void; a_attempt_mark; rig_fault walk "w" "y" >/dev/null; fail "take" "no claim" >/dev/null; a_attempt_void "$f1"; rig_unhealed walk "drops: both attempts" "w" >/dev/null')
	has "attempt: a FAIL in both attempts stands" "$o" "in both attempts: no claim"; has "attempt: ... the run is a FAIL" "$o" "VERDICT: FAIL"
	o=$(brk 'a_attempt_mark; f1=$A_ATTEMPT_LINE; rig_fault walk "w" "y" >/dev/null; fail "x" "one" >/dev/null; a_attempt_void; a_attempt_mark; rig_fault walk "w" "y" >/dev/null; fail "z" "two" >/dev/null; a_attempt_void "$f1"; rig_unhealed walk "s: both attempts" "w" >/dev/null')
	has "attempt: second attempt, a FAIL after its rig line set aside" "$o" "[was FAIL] two"; has "attempt: ... the run is RIG" "$o" "VERDICT: RIG"
	o=$(brk 'pass "a" ok >/dev/null; a_attempt_mark; fail "b" "real" >/dev/null; a_attempt_rig >/dev/null || echo NO-RIG')
	has "attempt: a FAIL alone is no rig fault" "$o" NO-RIG; has "attempt: ... and stays a FAIL" "$o" "VERDICT: FAIL"
	# the breaker counts a rig-broken failure that a re-run set aside: two rig-broken events still stop the run
	o=$(brk 'a_attempt_mark; rig_broken input "c1" "no shots" >/dev/null; a_attempt_void; a_attempt_mark; rig_broken input "c1" "no shots"; section b2')
	hasnt "breaker: a set-aside rig-broken failure counts" "$o" REACHED; has "breaker: ... the stop names it" "$o" "STOP: rig broken (input)"
	o=$(brk 'rig_unhealed state "state before pa_vm" "vm: in power armor" >/dev/null; pass "c" ok >/dev/null')
	has "rig_unhealed: listed" "$o" "rig fault (state), not healed: vm: in power armor"; has "rig_unhealed: VERDICT RIG" "$o" "VERDICT: RIG"
	[ "${o%%|*}" -ge 1 ] 2>/dev/null && echo "ok   rig_unhealed: exits non-zero" || { echo "FAIL rig_unhealed: exits non-zero: [${o%%|*}]"; bad=1; }
	o=$(brk 'rig_unhealed state "s1" "x" >/dev/null; rig_unhealed state "s2" "y" >/dev/null; section b2')
	hasnt "rig_unhealed: two stop the run" "$o" REACHED
	o=$(brk 'rig_unhealed state "s1" "x" >/dev/null; fail "hp" "differs" >/dev/null')
	has "rig_unhealed: with a FAIL the verdict is FAIL" "$o" "VERDICT: FAIL"
	# a game that hung: read from the log, said by name, and the failures beside a hang that is not the mod's set aside
	HB=$(date -d 12:00:00 +%s); hat() { date -d "@$((HB + $1))" +%T; }
	hl() { printf '[%s.000] [77   ] [%s] Hang: %s\r\n' "$(hat "$1")" "$2" "$3"; }
	hang_log() {  # hang_log <at s> <said s> <where; callers> [answered s]: a hang's lines as the plugin writes them (HL_STACK: how much of the stack it read; set empty, a plugin that doesn't say)
		local s=${HL_STACK-its stack read to its base (12 KB)}
		hl "$1" W "the game's main thread (thread 9) hasn't read its window messages for $2 s, in play or a menu"; hl "$1" W "it used 2% of a processor over 500 ms"
		hl "$1" W "it was at ${3%%; *}${s:+; $s}; ${3#*; }"; hl "$1" W "no other thread of the game kept a processor busy meanwhile"; hl "$1" W "wrote F4Multiplayer-hang-x.txt"
		[ -n "$4" ] && hl "$4" I "the game answers again, after about $(($4 - $1 + $2)) s"
	}
	hang_log 100 18 'win32u.dll+0x8C44; return addresses on its stack: GameOverlayRenderer64.dll+0xB175E, Fallout4.exe+0xC338D0, Fallout4.exe+0x22B1569, Fallout4.exe+0x22AF2BD' 113 > "$d/hang-rig.txt"
	t "hang list: time, seconds, phase, where, callers, not the mod's, answered" "$(_hang_list "$d/hang-rig.txt")" "$(printf '%s\t18\tin play or a menu\twin32u.dll+0x8C44\tGameOverlayRenderer64.dll+0xB175E, Fallout4.exe+0xC338D0, Fallout4.exe+0x22B1569\t0\t-\t%s\t31' "$(hat 100)" "$(hat 113)")"
	hang_log 100 15 'ntdll.dll+0x166D44; return addresses on its stack: KernelBase.dll+0xED4E9, Fallout4.exe+0x1680D6E, F4Multiplayer.dll+0x278A4F' 104 > "$d/hang-frame.txt"
	t "hang list: the mod's frame among the callers" "$(_hang_list "$d/hang-frame.txt" | cut -f6,7)" "$(printf '1\tF4Multiplayer.dll+0x278A4F')"
	hang_log 100 15 'F4Multiplayer.dll+0x1234; return addresses on its stack: Fallout4.exe+0x1' > "$d/hang-mod.txt"
	t "hang list: the main thread in the mod's code, never answered" "$(_hang_list "$d/hang-mod.txt" | cut -f4,6,8)" "$(printf 'F4Multiplayer.dll+0x1234\t2\t-')"
	{ mk '[10:00:00.000] [1] [I] Boot: build x'; hang_log 100 18 'win32u.dll+0x8C44; return addresses on its stack: a.dll+0x1' 113; hang_log 400 15 'ntdll.dll+0x1; return addresses on its stack: b.dll+0x2' 402; } > "$d/hang-two.txt"
	t "hang list: two hangs in a log" "$(_hang_list "$d/hang-two.txt" | wc -l)" 2
	t "hang list: a log with none" "$(_hang_list "$d/0.log" | wc -l)" 0
	# a stack that is not known, or not whole: nothing of the mod in it does not make the hang the rig's (3, with the reason)
	hang_head() { hl "$1" W "the game's main thread (thread 9) hasn't read its window messages for $2 s, in play or a menu"; hl "$1" W "it used 2% of a processor over 500 ms"; }
	{ hang_head 100 18; hl 100 W "wrote F4Multiplayer-hang-x.txt"; hl 113 I "the game answers again, after about 31 s"; } > "$d/hang-noat.txt"
	t "hang list: no place logged for the main thread: can't be said" "$(_hang_list "$d/hang-noat.txt" | cut -f4,6,7)" "$(printf '?\t3\tthe hang watch logged no place for the main thread')"
	c16=$(for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do printf '%sFallout4.exe+0x%X' "$([ $i -gt 1 ] && echo ', ')" $((0xC33000 + i)); done)
	hang_log 100 18 "win32u.dll+0x8C44; return addresses on its stack: $c16" 113 > "$d/hang-16.txt"
	t "hang list: as many return addresses as the watch logs: the mod could be further down" "$(_hang_list "$d/hang-16.txt" | cut -f6,7)" "$(printf '3\tthe 16 return addresses logged are as many as the watch logs, so the mod could be further down the stack')"
	hang_log 100 18 "win32u.dll+0x8C44; return addresses on its stack: ${c16%, *}" 113 > "$d/hang-15.txt"
	t "hang list: one fewer and nothing of the mod: the stack is known" "$(_hang_list "$d/hang-15.txt" | cut -f6,7)" "$(printf '0\t-')"
	hang_log 100 18 "win32u.dll+0x8C44; return addresses on its stack: ${c16%, *}, F4Multiplayer.dll+0x99" 113 > "$d/hang-16mod.txt"
	t "hang list: the mod's frame the 16th of 16: among the callers" "$(_hang_list "$d/hang-16mod.txt" | cut -f6,7)" "$(printf '1\tF4Multiplayer.dll+0x99')"
	hang_log 100 18 'win32u.dll+0x8C44; return addresses on its stack: none found' 113 > "$d/hang-nocallers.txt"
	t "hang list: no return address logged: can't be said" "$(_hang_list "$d/hang-nocallers.txt" | cut -f6,7)" "$(printf '3\tno return address of its stack was logged')"
	hang_log 100 18 '0x7FF612345678; return addresses on its stack: Fallout4.exe+0x1' 113 > "$d/hang-0x.txt"
	t "hang list: the main thread outside every module: can't be said" "$(_hang_list "$d/hang-0x.txt" | cut -f6,7)" "$(printf '3\tthe main thread was outside every module (0x7FF612345678)')"
	{ hang_head 100 18; hl 100 W "it was at win32u.dll+0x8C44; return addresses on its stack: Fallout4.exe+0x1"; hl 100 W "thread 12 kept a processor 97% busy meanwhile, at an unknown place; return addresses on its stack: none found"; hl 113 I "the game answers again, after about 31 s"; } > "$d/hang-unk.txt"
	t "hang list: a busy thread at an unknown place: can't be said" "$(_hang_list "$d/hang-unk.txt" | cut -f6,7)" "$(printf '3\ta busy thread was at an unknown place')"
	{ hang_head 100 18; hl 100 W "it was at win32u.dll+0x8C44; return addresses on its stack: Fallout4.exe+0x1"; hl 100 W "thread 12 kept a processor 97% busy meanwhile, at F4Multiplayer.dll+0x77; return addresses on its stack: Fallout4.exe+0x2"; } > "$d/hang-busy.txt"
	t "hang list: a busy thread in the mod's code: the mod's frame" "$(_hang_list "$d/hang-busy.txt" | cut -f6,7)" "$(printf '1\tF4Multiplayer.dll+0x77 (where a busy thread was)')"
	# the watch copies at most 64 KB of a stack: only a stack it says it read to its base is whole (a plugin that doesn't say: not whole)
	HL_STACK= hang_log 100 18 'win32u.dll+0x8C44; return addresses on its stack: Fallout4.exe+0x1' 113 > "$d/hang-old.txt"
	t "hang list: a plugin that doesn't say how much of the stack it read: can't be said" "$(_hang_list "$d/hang-old.txt" | cut -f6,7)" "$(printf '3\tthe hang watch did not say how much of the stack it read (a plugin from before it did), so the mod could be further down')"
	HL_STACK='only the top 64 KB of its 130 KB stack read' hang_log 100 18 'win32u.dll+0x8C44; return addresses on its stack: Fallout4.exe+0x1' 113 > "$d/hang-part.txt"
	t "hang list: a stack read in part, few callers and nothing of the mod: can't be said" "$(_hang_list "$d/hang-part.txt" | cut -f6,7)" "$(printf '3\tthe hang watch did not read the stack to its base (only the top 64 KB of its 130 KB stack read), so the mod could be further down')"
	t "hang list: ... the stack's words do not spill into the callers" "$(_hang_list "$d/hang-part.txt" | cut -f4,5)" "$(printf 'win32u.dll+0x8C44\tFallout4.exe+0x1')"
	{ hang_head 100 18; hl 100 W "it was at win32u.dll+0x8C44; its stack read to its base (9 KB); return addresses on its stack: Fallout4.exe+0x1"; hl 100 W "thread 12 kept a processor 97% busy meanwhile, at Fallout4.exe+0x5; only the top 64 KB of its 100 KB stack read; return addresses on its stack: Fallout4.exe+0x2"; hl 113 I "the game answers again, after about 31 s"; } > "$d/hang-busycut.txt"
	t "hang list: a busy thread's stack read in part: can't be said" "$(_hang_list "$d/hang-busycut.txt" | cut -f6,7)" "$(printf '3\ta busy thread'"'"'s stack was not read to its base (thread 12: only the top 64 KB of its 100 KB stack read)')"
	sed 's/only the top 64 KB of its 100 KB stack read/its stack read to its base (20 KB)/' "$d/hang-busycut.txt" > "$d/hang-busywhole.txt"
	t "hang list: ... read to its base, with nothing of the mod: the stack is known" "$(_hang_list "$d/hang-busywhole.txt" | cut -f6,7)" "$(printf '0\t-')"
	# a hang's block read before it was all written, or with a line cut, is not taken as the rig's
	grep -v -e "Hang: wrote" -e "answers again" "$d/hang-rig.txt" > "$d/hang-cut.txt"
	t "hang list: a block with no last line (read while it was written): can't be said" "$(_hang_list "$d/hang-cut.txt" | cut -f6,7)" "$(printf '3\tthe hang'"'"'s lines stop before the watch'"'"'s last one (its report file'"'"'s): the log was read while it was written, or a line was cut')"
	{ cat "$d/hang-cut.txt"; hl 100 W "no report file (no Documents folder)"; } > "$d/hang-noreport.txt"
	t "hang list: ... its last line \"no report file\", not answered: the stack is known" "$(_hang_list "$d/hang-noreport.txt" | cut -f6,7)" "$(printf '0\t-')"
	grep -v "no other thread" "$d/hang-rig.txt" > "$d/hang-untimed.txt"
	t "hang list: no line on the other threads: can't be said" "$(_hang_list "$d/hang-untimed.txt" | cut -f6,7)" "$(printf '3\tthe hang watch logged no line on the game'"'"'s other threads, so a busy one could be missed')"
	hang_log 100 18 'win32u.dll+0x8C44; return addresses on its stack: Fallout4.exe+0x1, Fallout4.ex' 113 > "$d/hang-badcaller.txt"
	t "hang list: a return address cut short: can't be said" "$(_hang_list "$d/hang-badcaller.txt" | cut -f6,7)" "$(printf '3\ta return address logged is not a module and offset (Fallout4.ex): a line cut short')"
	# hrun <log> <body>: a run that began at 12:00:00; at <s> sets the clock (a hang said at +100 for 18 s spans 12:01:22 to its answer)
	hrun() { brk 'A_EPOCH0='"$HB"'; at() { A_NOW=$(('"$HB"' + $1)); }; '"$2"'; at 600; hang_check vm "'"$d/$1"'" >/dev/null; cat "$A_RESULTS"'; }
	o=$(hrun hang-rig.txt 'at 50; pass "early" ok >/dev/null; calls 1 v; at 60; fail "before the hang" "real" >/dev/null; calls 1 v; at 95; fail "safe (vm unlocks)" "got '"''"'" >/dev/null; at 110; rig_broken game "vm in the game" "not in the game after 60 s" >/dev/null; calls 1 v; at 300; fail "long after" "real too" >/dev/null')
	has "hang: a FAIL made while the game hung is set aside" "$o" "$(printf 'VOID\tb1\tsafe (vm unlocks)\t[was FAIL] got')"
	has "hang: ... a FAIL before it stands" "$o" "$(printf 'FAIL\tb1\tbefore the hang')"; has "hang: ... a FAIL long after it stands" "$o" "$(printf 'FAIL\tb1\tlong after')"
	has "hang: ... a rig-broken FAIL stays as it is" "$o" "$(printf 'FAIL\tb1\tvm in the game\trig broken (game)')"
	has "hang: ... the hang is a rig fault of signal hang" "$o" "rig fault (hang): 1 check(s) that read vm's game and failed while it hung are set aside, not made (run them again): safe (vm unlocks). The hang: its main thread read no window messages for about 31 s, in play or a menu (from about $(hat 82)), at win32u.dll+0x8C44 called from GameOverlayRenderer64.dll+0xB175E"
	o=$(hrun hang-rig.txt 'at 50; pass "early" ok >/dev/null; calls 1 v; at 95; fail "safe (vm unlocks)" "got '"''"'" >/dev/null')
	has "hang: with only its failures the verdict is RIG" "$o" "VERDICT: RIG"; has "hang: ... the failure is named as set aside" "$o" "1 failure(s) set aside after a rig fault"; hasnt "hang: ... never a PASS" "$o" "VERDICT: PASS"
	o=$(hrun hang-rig.txt 'at 50; pass "early" ok >/dev/null; at 200; pass "late" ok >/dev/null')
	has "hang: no failure beside it: a WARN with the facts" "$o" "$(printf 'WARN\tb1\tvm'"'"'s game hung (%s)\tits main thread read no window messages for about 31 s' "$(hat 100)")"; has "hang: ... and the run passes" "$o" "VERDICT: PASS"
	o=$(hrun hang-rig.txt 'at 20; pass "first" ok >/dev/null; calls 1 v; at 290; fail "a slow step" "x" >/dev/null')
	has "hang: a check whose step began more than HANG_LOOK s before its line, after the hang, stands" "$o" "$(printf 'FAIL\tb1\ta slow step')"
	o=$(hrun hang-rig.txt 'at 20; pass "first" ok >/dev/null; calls 1 v; at 180; fail "a step after the hang" "x" >/dev/null')
	has "hang: ... and so does one whose step began HANG_LOOK s (45) before its line, after the hang" "$o" "$(printf 'FAIL\tb1\ta step after the hang')"
	o=$(hrun hang-rig.txt 'at 90; pass "first" ok >/dev/null; calls 1 v; at 130; fail "a step over the hang" "x" >/dev/null')
	has "hang: a check whose step ran over the hang is set aside" "$o" "$(printf 'VOID\tb1\ta step over the hang')"
	o=$(hrun hang-frame.txt 'at 95; fail "hp the same" "differs" >/dev/null')
	has "hang: the mod's frame on the stack: a WARN naming it" "$o" "F4Multiplayer.dll+0x278A4F is on it"; has "hang: ... the failure beside it stands" "$o" "$(printf 'FAIL\tb1\thp the same\tdiffers')"
	has "hang: ... and is named" "$o" "1 check(s) failed while it hung and stand: hp the same"; hasnt "hang: ... no rig fault" "$o" "rig fault (hang)"
	o=$(hrun hang-mod.txt 'at 95; fail "hp the same" "differs" >/dev/null')
	has "hang: the main thread in the mod's code is a FAIL" "$o" "$(printf 'FAIL\tb1\tvm'"'"'s game hung in the mod'"'"'s code (%s)' "$(hat 100)")"; has "hang: ... not answered again is said" "$o" "for 15 s and had not answered again when the log was read"
	# a hang that is not known to be the rig's sets nothing aside: no place for the main thread, or a stack the watch cut off
	for f in hang-noat hang-16 hang-nocallers hang-0x hang-unk hang-old hang-part hang-busycut; do
		o=$(hrun $f.txt 'calls 1 v; at 50; pass "early" ok >/dev/null; at 95; fail "hp the same" "differs" >/dev/null')
		has "hang ($f): whose hang it is can't be said: a WARN with the reason" "$o" "$(printf 'WARN\tb1\tvm'"'"'s game hung, and whose hang it is can'"'"'t be said (%s)\tits main thread read no window messages for about 31 s' "$(hat 100)")"
		has "hang ($f): ... the failure beside it stands, and is named" "$o" "the hang is not taken as the rig's; 1 check(s) failed while it hung and stand: hp the same"
		has "hang ($f): ... a FAIL in results" "$o" "$(printf 'FAIL\tb1\thp the same\tdiffers')"; hasnt "hang ($f): ... nothing set aside, no rig fault" "$o" "rig fault (hang)"
		has "hang ($f): ... and the run is a FAIL" "$o" "VERDICT: FAIL (brk: 1 failed"
	done
	o=$(hrun hang-noat.txt 'at 50; pass "early" ok >/dev/null'); has "hang: one that can't be said, with no failure beside it: the WARN alone" "$o" "but the hang watch logged no place for the main thread: the hang is not taken as the rig's"$'\n'; has "hang: ... and the run passes" "$o" "VERDICT: PASS"
	o=$(hrun hang-15.txt 'calls 1 v; at 95; fail "safe (vm unlocks)" "x" >/dev/null'); has "hang: a known place and a whole stack with nothing of the mod: still set aside" "$o" "$(printf 'VOID\tb1\tsafe (vm unlocks)')"
	# "the failures stand" holds for the channel rule: the hung game's channel faults of that time are FAILs again, unless the hang is the rig's
	for f in hang-mod hang-frame hang-16 hang-noat; do
		o=$(hrun $f.txt 'at 50; pass "early" ok >/dev/null; calls 1; na host; at 94; fail "the host counts one" "got nothing" >/dev/null; calls 1; na vm 0 2; at 95; fail "hp the same" "got nothing" >/dev/null')
		has "hang ($f): the hung game's channel fault is a FAIL again" "$o" "$(printf 'FAIL\tb1\thp the same\tvm'"'"'s game hung then, and the hang is not known to be the rig'"'"'s, so this failure stands (it was first taken as a channel fault: vm'"'"'s game took the command and gave no answer in 20 s')"
		has "hang ($f): ... and is named as standing" "$o" "1 check(s) failed while it hung and stand: hp the same"
		has "hang ($f): ... the other game's channel fault stays one" "$o" "$(printf 'RIG\tb1\tthe host counts one\trig fault (channel): host')"
		hasnt "hang ($f): ... the verdict is not RIG" "$o" "VERDICT: RIG"; has "hang ($f): ... it is FAIL" "$o" "VERDICT: FAIL (brk: "
	done
	o=$(hrun hang-mod.txt 'calls 1; na vm; at 95; fail "hp the same" "got nothing" >/dev/null'); has "hang: a hang in the mod's code after a no-answer: two FAILs, not VERDICT RIG" "$o" "VERDICT: FAIL (brk: 2 failed"
	o=$(hrun hang-mod.txt 'calls 1; na vm; a_attempt_mark; at 95; fail "hp the same" "got nothing" >/dev/null; a_attempt_void'); has "hang: ... a channel fault a re-run had set aside too" "$o" "$(printf 'FAIL\tb1\thp the same\tvm'"'"'s game hung then')"
	o=$(hrun hang-mod.txt 'calls 1; na vm; at 300; fail "long after" "got nothing" >/dev/null'); has "hang: ... and one made later, since a hang that never answered runs to the log's reading" "$o" "$(printf 'FAIL\tb1\tlong after\tvm'"'"'s game hung then')"
	o=$(hrun hang-frame.txt 'calls 1; na vm; at 300; fail "long after" "got nothing" >/dev/null'); has "hang: a channel fault long after a hang that answered again stays one" "$o" "$(printf 'RIG\tb1\tlong after\trig fault (channel): vm')"
	o=$(hrun hang-rig.txt 'calls 1; na vm; at 95; fail "hp the same" "got nothing" >/dev/null'); has "hang: the rig's hang leaves the channel fault a rig fault" "$o" "$(printf 'RIG\tb1\thp the same\trig fault (channel): vm')"; has "hang: ... the run is VERDICT RIG" "$o" "VERDICT: RIG"
	o=$(hrun hang-rig.txt 'A_EPOCH0=$(('"$HB"' + 400)); at 450; fail "x" "y" >/dev/null')
	hasnt "hang: a hang before the run began is not the run's" "$o" "hung"
	o=$(hrun hang-two.txt 'calls 1 v; at 95; fail "one" "x" >/dev/null; calls 1 v; at 399; fail "two" "y" >/dev/null; at 600; hang_check vm "'"$d/hang-two.txt"'" >/dev/null')
	t "hang: two hangs, each said once though the log is read twice" "$(grep -c "rig fault (hang)" <<<"${o%================*}")" 2
	o=$(brk 'A_EPOCH0='"$HB"'; A_NOW=$(('"$HB"' + 95)); MARK_FIRST[vm]=x; log_delta_file() { echo "'"$d/hang-rig.txt"'"; }; rig_broken game "g1" "down" >/dev/null; calls 1 v; fail "safe" "got nothing" >/dev/null; A_NOW=$(('"$HB"' + 200)); rig_broken game "g2" "down"')
	has "hang: a run the breaker stops says its hangs" "$o" "rig fault (hang): 1 check(s) that read vm's game and failed while it hung"; has "hang: ... and stays stopped" "$o" "run stopped: rig broken (game)"
	# a hang taken as the rig's never sets aside the mod's own verdicts or the other game's checks. The case found in review:
	# the VM hung and never answered again (it crashed, or was still stuck when its log was read), so its span runs to the end
	# checks: crash_verdict and the host's log_check come before the VM's log_check reads the hang
	hang_log 100 18 'ntdll.dll+0x166D44; return addresses on its stack: KernelBase.dll+0xED4E9, Fallout4.exe+0x1680D6E, Fallout4.exe+0x22B1569' > "$d/hang-never.txt"
	t "hang list: the review's hang, its stack read to its base: taken as the rig's" "$(_hang_list "$d/hang-never.txt" | cut -f6,8)" "$(printf '0\t-')"
	eval "$(sed -n '/^crash_verdict() {/,/^}/p' "$RIG/steps.sh")"
	endrun='at 50; pass "early" ok >/dev/null; at 90; fail "the walk arrived" "stuck" >/dev/null; calls 1 v; at 95; fail "the VM counts one" "got 0" >/dev/null; calls 1 h; at 96; fail "the host counts one" "got 0" >/dev/null; calls 1 v; at 140; crash_verdict hits >/dev/null; log_delta_file() { echo "'"$d/err.txt"'"; }; at 142; log_check host "end host" >/dev/null; at 150; hang_check vm "'"$d/hang-never.txt"'" >/dev/null'
	o=$(hrun hang-never.txt "$endrun")
	has "hang (rig's, never answered): the crash stays a FAIL" "$o" $'FAIL\tb1\thits crash logs (vm)\tnew: crash-9.log'; hasnt "hang (rig's, never answered): ... not set aside" "$o" "[was FAIL] new: crash-9.log"
	has "hang (rig's, never answered): the host's log errors stay a FAIL" "$o" $'FAIL\tb1\tend host plugin log errors\t1 new [E] line(s)'; has "hang (rig's, never answered): ... its asserts too" "$o" $'FAIL\tb1\tend host plugin log asserts\t1 line(s)'
	has "hang (rig's, never answered): a check that read only the host's game stands" "$o" $'FAIL\tb1\tthe host counts one\tgot 0'
	has "hang (rig's, never answered): ... and one that read no dev command" "$o" $'FAIL\tb1\tthe walk arrived\tstuck'
	has "hang (rig's, never answered): a check that read the VM's game is set aside" "$o" $'VOID\tb1\tthe VM counts one\t[was FAIL] got 0'
	has "hang (rig's, never answered): ... the rig fault names it, and what stands" "$o" "rig fault (hang): 1 check(s) that read vm's game and failed while it hung are set aside, not made (run them again): the VM counts one."
	has "hang (rig's, never answered): ... the rest are named as standing" "$o" "5 other check(s) failed then and stand"
	has "hang (rig's, never answered): the run is VERDICT FAIL" "$o" "VERDICT: FAIL (brk: 5 failed"; hasnt "hang (rig's, never answered): ... not RIG" "$o" "VERDICT: RIG"
	# ... and the log as the review wrote it, from a plugin that doesn't say how much of the stack it read: nothing set aside at all
	HL_STACK= hang_log 100 18 'ntdll.dll+0x166D44; return addresses on its stack: KernelBase.dll+0xED4E9, Fallout4.exe+0x1680D6E, Fallout4.exe+0x22B1569' > "$d/hang-never-old.txt"
	o=$(hrun hang-never-old.txt "${endrun//hang-never.txt/hang-never-old.txt}")
	has "hang (no stack size said, never answered): can't be said" "$o" "whose hang it is can't be said"; hasnt "hang (no stack size said, never answered): ... nothing set aside" "$o" $'VOID\t'
	has "hang (no stack size said, never answered): ... the run is VERDICT FAIL" "$o" "VERDICT: FAIL (brk: 6 failed"
	unset -f crash_verdict
	# a hang taken as the rig's sets aside only the hung game's checks: the VM's leaves a check of the host's game a FAIL, the host's does not
	o=$(hrun hang-rig.txt 'calls 1 h; at 95; fail "the host counts one" "got 0" >/dev/null')
	has "hang (the VM's, rig's): a check that read the host's game stands" "$o" $'FAIL\tb1\tthe host counts one\tgot 0'; has "hang (the VM's, rig's): ... a WARN says so" "$o" "no check that read vm's game failed while it hung; 1 other check(s) failed then and stand"
	o=$(hrun hang-rig.txt 'calls 1 h; at 95; fail "the host counts one" "got 0" >/dev/null; at 600; hang_check host "'"$d/hang-rig.txt"'" >/dev/null')
	has "hang (the host's, rig's): ... the same check is set aside" "$o" $'VOID\tb1\tthe host counts one\t[was FAIL] got 0'
	# a check that made no dev call of its own reads no game, though it follows one that read the hung game's (the third
	# review's case: a host-log check recorded right after a VM check, the VM's hang the rig's)
	o=$(hrun hang-rig.txt 'calls 2 v; at 90; fail "the VM counts one" "got 0" >/dev/null; at 92; fail "host log: QuestSync line" "no such line" >/dev/null')
	has "hang (the VM's, rig's): the check that read the VM is set aside" "$o" $'VOID\tb1\tthe VM counts one\t[was FAIL] got 0'
	has "hang (the VM's, rig's): ... the one after it with no call of its own stands" "$o" $'FAIL\tb1\thost log: QuestSync line\tno such line'
	has "hang (the VM's, rig's): ... and is named as standing" "$o" "1 other check(s) failed then and stand"; has "hang (the VM's, rig's): ... the run is VERDICT FAIL" "$o" "VERDICT: FAIL"
	hasnt "hang (the VM's, rig's): ... not RIG" "$o" "VERDICT: RIG"
	o=$(hrun hang-rig.txt 'at 50; pass "early" ok >/dev/null; calls 1 v; at 90; fail "A" "x" >/dev/null; at 91; fail "B" "y" >/dev/null; calls 1 h; at 92; fail "C" "z" >/dev/null; at 93; fail "D" "w" >/dev/null; calls 1 v; at 94; fail "E" "v" >/dev/null')
	t "hang (the VM's, rig's): only lines with a VM call of their own are set aside" "$(grep -E $'^(VOID|FAIL)\tb1\t[A-E]\t' <<<"$o" | cut -f1,3 | tr '\t\n' ': ')" "VOID:A FAIL:B FAIL:C FAIL:D VOID:E "
	has "hang: times.tsv gives a line with no call of its own no game" "$(brk 'calls 1 v; fail "A" x >/dev/null; fail "B" y >/dev/null; pass "C" z >/dev/null; cut -f3,5 "$A_TIMES" | tr "\t\n" ": "')" "A:vm B:- C:- "
	# smoke.sh's re-run never sets aside the mod's own verdicts either
	o=$(brk 'a_attempt_mark; rig_fault walk "walk" "stuck" >/dev/null; fail_mod "run crash logs (vm)" "new: crash-9.log" >/dev/null; mod_check expect_zero "end vm privacy: Steam ids" 1 >/dev/null; fail "give back" "x" >/dev/null; a_attempt_void; cat "$A_RESULTS"')
	has "attempt: a crash after the rig line stays a FAIL" "$o" $'FAIL\tb1\trun crash logs (vm)\tnew: crash-9.log'; has "attempt: ... and the privacy scan" "$o" $'FAIL\tb1\tend vm privacy: Steam ids\t'
	has "attempt: ... another FAIL after it is set aside" "$o" $'VOID\tb1\tgive back\t[was FAIL] x'; has "attempt: ... the run is VERDICT FAIL" "$o" "VERDICT: FAIL (brk: 2 failed"
	# the VM's channel reached again (env.sh vm_reach): only heals logged after a_init, each once, even when seen in a subshell
	o=$( ( sd=$(mktemp -d); STAGE=$(mktemp -d); A_OUT=$sd; A_SECTION=""
		printf '1\t09:00:00\tan old heal\n' > "$STAGE/vm-reach.log"; a_init reach >/dev/null; section r1 >/dev/null
		printf '2\t15:14:40\tnothing listened on 7792; tunnel made again\n' >> "$STAGE/vm-reach.log"
		( pass "in a subshell" ok >/dev/null ); pass "after" ok >/dev/null
		a_summary; rm -rf "$sd" "$STAGE" ) 2>&1 ); r=$?
	has "reach: a heal after the start is a HEAL" "$o" "the VM's dev channel reached again (15:14:40): nothing listened on 7792"
	hasnt "reach: a heal before the start is not" "$o" "an old heal"
	t "reach: recorded once" "$(grep -c "reached again (15:14:40)" <<<"$o")" 1   # the summary's line (the subshell's own was not shown)
	has "reach: the verdict is a PASS" "$o" "VERDICT: PASS"; t "reach: exit 0" "$r" 0
	[ $bad -eq 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"
	rm -rf "$d" "$F4MP_CHECKS"; exit $bad
fi
