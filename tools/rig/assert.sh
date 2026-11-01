#!/bin/bash
# assert.sh: small helpers that turn a rig run into named checks with a verdict. Source it:
#
#     . tools/rig/assert.sh
#     a_init smoke                         # makes a run folder under $CHECKS, starts the clock
#     section "movement"                   # a heading; the summary also lists the time each section took
#     pass "name" "reason"; fail "name" "reason"; warn "name" "reason"      # record one check by hand
#     expect_eq "label" "$got" "$want"     # string equal
#     expect_le "label" "$got" 10 u        # number at most (an empty or non-number "got" is a FAIL, never a pass)
#     expect_ge "label" "$got" 1 hp        # number at least
#     expect_between "label" "$got" 0 100  # number in a range
#     expect_match "label" "$got" 'regex'  # extended regex
#     expect_zero "label" "$got"           # shorthand for expect_eq "$got" 0
#     warn_gt "label" "$got" 5 u           # number above a soft limit: WARN (printed in the summary), never a FAIL
#     a_summary                            # one screen: counts, every FAIL and WARN, times; exits non-zero on a FAIL
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
	echo "$1 run folder: $A_OUT"
}

section() {  # section <title>
	[ -z "$A_RIG_STOPPED" ] && [ -n "$A_RESULTS" ] && [ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && _rig_stop "$(_rig_last_signal)"
	[ -n "$A_SECTION" ] && printf '%s\t%s\n' "$A_SECTION" "$((SECONDS - A_SEC_T0))" >> "$A_SECLOG"
	A_SECTION=$1; A_SEC_T0=$SECONDS; echo; echo "=== $1 ($(date +%T))"
}

record() {  # record <PASS|FAIL|WARN> <name> <reason>
	printf '%s\t%s\t%s\t%s\n' "$1" "$A_SECTION" "$2" "$3" >> "$A_RESULTS"
	printf '%-4s %s%s\n' "$1" "$2" "${3:+: $3}"
}
pass() { record PASS "$1" "$2"; }
fail() { record FAIL "$1" "$2"; }
warn() { record WARN "$1" "$2"; }

# --- the circuit breaker: a broken rig ends the run ------------------------------------------------------------
# A check that finds the RIG broken, not the mod, says so with rig_broken: a game's input not reaching it (a click that
# fires nothing, keys that move nobody), a game not running or not connected, ssh to the VM down. Such a failure is a FAIL
# like any other, and RIG_BREAK_AT of them (2) end the run at once as FAIL "run stopped: rig broken (<signal>)": the rest
# would measure the rig (2026-11-01: a VM whose mouse never came up made 18 failures that proved nothing). A failure of the
# mod is a plain fail and never trips it. They are counted in results.tsv, so one made in a subshell counts too; section()
# looks again, for a trip made where an exit can't end the run.
RIG_BREAK_AT=${RIG_BREAK_AT:-2}
_rig_last_signal() { awk -F'\t' '$1=="FAIL" && index($4, "rig broken (") == 1 {s=substr($4, 13); s=substr(s, 1, index(s, ")") - 1)} END{print s}' "$A_RESULTS" 2>/dev/null; }
_rig_broken_count() { awk -F'\t' '$1=="FAIL" && index($4, "rig broken (") == 1 {n++} END{print n+0}' "$A_RESULTS" 2>/dev/null; }
_rig_stop() {  # _rig_stop <signal>: the run ends here, its verdict FAIL
	A_RIG_STOPPED=1
	record FAIL "run stopped: rig broken ($1)" "$(_rig_broken_count) rig-broken failure(s), the last: $1. The checks after this point were not made; fix the rig and run again"
	echo "STOP: rig broken ($1)"
	declare -F crash_verdict >/dev/null && [ -n "$A_NAME" ] && crash_verdict "$A_NAME"   # a game that went down may have crashed: that is the mod's, and is said
	a_summary
}
rig_broken() {  # rig_broken <signal: input|game|ssh> <check name> <reason>
	record FAIL "$2" "rig broken ($1): $3"
	[ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && [ -z "$A_RIG_STOPPED" ] && _rig_stop "$1"
	return 0
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
# nothing open but the HUD (dev `menus`). What a player would close is closed: a message box (Enter), the Pip-Boy, a terminal or
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
		open=$(tr ',' '\n' <<<"${m#open=}" | cut -d: -f1 | grep -vxE 'HUDMenu|CursorMenu' | tr '\n' ' '); open=${open% }
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
	case "$f" in front=no*) INPUT_FRONT=no; INPUT_READY="${INPUT_READY:+$INPUT_READY; }not in front after $k tries: ${f%% input:*}";; esac
	[ "$INPUT_FRONT" = yes ] && [ "$k" -gt 1 ] && INPUT_READY="${INPUT_READY:+$INPUT_READY; }brought to the front (try $k)"
	return 0
}
# What a probe that failed twice says. With the window in front and on a screen, a click or key that still does nothing is real
# dead input (a game whose player input died at its first load, about one fresh game in three, which only a restart mends and
# rig.sh join restarts; or a real regression); one that would not come to the front is the desktop's or a build without the fix.
RIG_INPUT_DEAD="its window was in front and on a screen, and its input still never reached the player"
RIG_INPUT_NOFRONT="its window would not stay in front and on a screen (another window kept taking it, or it is parked off every screen and the DLL can't move it): no input goes to a game that is not in front"
rig_input_why() { [ "$INPUT_FRONT" = no ] && echo "$RIG_INPUT_NOFRONT" || echo "$RIG_INPUT_DEAD"; }
# rig_click_fires <who> <label>: one real click fires that game's gun (`net status` shots counts its player's attacks); the game
# made ready first (rig_input_ready), and once more before a second click if the first fired nothing
# mouse_why <who>: what that game's raw input looked like (the `mousedev` answer), for a click that fired nothing; empty from a build
# without the command
mouse_why() { local m; m=$(dc "$1" mousedev 2>/dev/null); case "$m" in raw=*) printf ' [mousedev: %s]' "$m" ;; esac; }
rig_click_fires() {
	local s0 s1 try note=""
	for try in 1 2; do
		rig_input_ready "$1"; [ -n "$INPUT_READY" ] && note="$note${note:+; }before click $try: $INPUT_READY"
		sleep 1; s0=$(field "$(dc "$1" 'net status')" shots); dc "$1" "click 120" >/dev/null; sleep 2; s1=$(field "$(dc "$1" 'net status')" shots)
		if is_num "$s0" && is_num "$s1" && [ "$s1" -gt "$s0" ]; then pass "$2" "shots $s0 -> $s1$( [ $try -gt 1 ] && echo " on the second click")${note:+ ($note)}"; return 0; fi
	done
	rig_broken input "$2" "$1's click fired nothing, twice (shots '$s0' -> '$s1')${note:+ ($note)}: $(rig_input_why)$(mouse_why "$1")"; return 1
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
		else scp -q -i "$VM_KEY" -o ConnectTimeout=20 "$VM_SSH:$VM_F4SE/$f" "$2/$n.log" 2>/dev/null; fi
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
log_check() {  # log_check <who> <label>
	local f e w a; f=$(log_delta_file "$1")
	if [ ! -s "$f" ]; then fail "$2 plugin log" "no new lines since the mark: the log was not read (or the game wrote nothing)"; return; fi
	e=$(grep -cE '^\[[^]]*\] \[[^]]*\] \[E\] ' "$f"); w=$(grep -cE '^\[[^]]*\] \[[^]]*\] \[W\] ' "$f")
	a=$(grep -ciE 'assert(ion)? (failed|failure)|ASSERT:|assertion failed' "$f")
	if [ "$e" -gt 0 ]; then fail "$2 plugin log errors" "$e new [E] line(s): $(_prefixes "$f" E)(text in $f)"; else pass "$2 plugin log errors" "none in $(wc -l < "$f") new lines"; fi
	if [ "$a" -gt 0 ]; then fail "$2 plugin log asserts" "$a line(s) with assertion text (in $f)"; else pass "$2 plugin log asserts" "none"; fi
	if [ "$w" -gt 0 ]; then warn "$2 plugin log warnings" "$w new [W] line(s): $(_prefixes "$f" W)"; else pass "$2 plugin log warnings" "none"; fi
}
log_privacy() {  # log_privacy <who> <label>: the rc gate's scan, on the delta only; counts, never the matching text
	local f p s i pf="${F4MP_PRIVATE_PATTERNS:-$HOME/.f4mp-private-patterns}"; f=$A_OUT/logdelta-$1.txt
	[ -s "$f" ] || f=$(log_delta_file "$1")
	if [ -s "$pf" ]; then p=$(grep -ciF -f "$pf" "$f"); else p=""; fi
	s=$(grep -cE '7656[0-9]{13}' "$f")
	i=$(grep -oE '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b' "$f" | grep -vE '^127\.0\.0\.1$|^0\.0\.0\.0$' | sort -u | wc -l)
	if [ -z "$p" ]; then warn "$2 privacy: patterns file" "no private-patterns file at $pf: that scan was skipped"; else expect_zero "$2 privacy: private patterns" "$p"; fi
	expect_zero "$2 privacy: Steam ids" "$s"; expect_zero "$2 privacy: IP addresses other than 127.0.0.1" "$i"
}

# --- the verdict ---------------------------------------------------------------------------------------------
a_summary() {  # a_summary: prints the one-screen summary, writes $A_OUT/summary.txt, exits non-zero on a FAIL
	[ -n "$A_SECTION" ] && printf '%s\t%s\n' "$A_SECTION" "$((SECONDS - A_SEC_T0))" >> "$A_SECLOG"; A_SECTION=""
	local np nf nw total mins; np=$(grep -c '^PASS' "$A_RESULTS"); nf=$(grep -c '^FAIL' "$A_RESULTS"); nw=$(grep -c '^WARN' "$A_RESULTS")
	total=$((SECONDS - A_T0)); mins=$(awk -v s=$total 'BEGIN{printf "%.1f", s/60}')
	{
		echo; echo "================ $A_NAME summary ================"
		echo "started $A_START, ran $mins min: $np passed, $nf FAILED, $nw warnings"
		awk -F'\t' '{printf "  %-34s %3d s\n", $1, $2}' "$A_SECLOG"
		if [ "$nf" -gt 0 ]; then echo "FAILED:"; awk -F'\t' '$1=="FAIL"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nw" -gt 0 ]; then echo "warnings:"; awk -F'\t' '$1=="WARN"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nf" -eq 0 ]; then echo "VERDICT: PASS ($A_NAME)"; else echo "VERDICT: FAIL ($A_NAME: $nf failed check(s))"; fi
		echo "details: $A_OUT"
	} | tee "$A_OUT/summary.txt"
	[ "$nf" -gt 125 ] && nf=125
	exit "$nf"
}

a_interrupted() {  # the INT/TERM trap of a run (trap a_interrupted INT TERM): an interrupted run is a FAILED run, never a PASS with fewer checks
	fail "run interrupted" "stopped by a signal before the end: the checks after this point were not made"
	echo "interrupted"; a_summary
}

# --- self-test: the helpers without a game ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "$1" = selftest ]; then
	export F4MP_CHECKS=$(mktemp -d); CHECKS=$F4MP_CHECKS; A_OUT=""; a_init selftest >/dev/null; bad=0
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
	o=$(brk 'sleep() { :; }; dc() { case "$2" in pos) echo "x=10.0 y=20.0 z=0";; *) echo ok;; esac; }; rig_keys_move host "moves" >/dev/null')
	has "rig_keys_move: no movement is signal input" "$o" "rig broken (input): host's player moved '0' u"
	[ $bad -eq 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"
	rm -rf "$d" "$F4MP_CHECKS"; exit $bad
fi
