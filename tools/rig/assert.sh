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
	[ $bad -eq 0 ] && echo "selftest: all ok" || echo "selftest: FAILED"
	rm -rf "$d" "$F4MP_CHECKS"; exit $bad
fi
