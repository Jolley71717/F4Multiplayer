#!/usr/bin/env bash
# Runs the unit tests over and over, to catch a run that ends early once in many (a crash, an abort, a hang).
#
#   tools/testloop.sh [runs] [test name filter]
#
#   runs     how many times (default 50)
#   filter   passed to F4MPTests: only tests whose name holds it run (default: all)
#
# $TESTS is the binary (default build/windows/x64/releasedbg/F4MPTests.exe, else build/linux/x86_64/releasedbg/F4MPTests);
# build it first (xmake build -y F4MPTests). $TESTLOOP_OUT is where the logs go (default build/testloop/<date-time>):
# run-NNN.log holds one run's stdout and stderr. $TESTLOOP_TIMEOUT is one run's limit in seconds (default 1800; the
# full suite takes ~9 min): a run past it is stopped and counts as a hang.
#
# A run "ended early" when it has no summary line ("N tests, M failed"): F4MPTests then printed a "CRASHED in <test>"
# or "EXITED in <test>" line naming the test that was running (tests/main.cpp). The loop stops at the first such run
# and prints that line. A run that ends with a summary and failed tests is noted and the loop goes on.
#
# Exit code: 0 when every run ended with a summary and no failures, 1 when a run had failed tests, 2 when a run ended
# early or hung, 125 on a usage error.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
runs=${1:-50}
filter=${2:-}
limit=${TESTLOOP_TIMEOUT:-1800}

die() { echo "testloop.sh: $*" >&2; exit 125; }

[[ "$runs" =~ ^[1-9][0-9]*$ ]] || die "runs must be a whole number of at least 1, got '$runs'"
[[ "$limit" =~ ^[1-9][0-9]*$ ]] || die "TESTLOOP_TIMEOUT must be a whole number of seconds, got '$limit'"
tests=${TESTS:-}
if [ -z "$tests" ]; then
	for candidate in "$root/build/windows/x64/releasedbg/F4MPTests.exe" "$root/build/linux/x86_64/releasedbg/F4MPTests"; do
		if [ -x "$candidate" ]; then tests=$candidate; break; fi
	done
fi
[ -n "$tests" ] && [ -x "$tests" ] || die "no F4MPTests binary (build it, or set TESTS): '${tests:-none found}'"
out=${TESTLOOP_OUT:-$root/build/testloop/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out" || die "cannot make $out"

echo "testloop: $runs runs of $tests${filter:+ (filter '$filter')}, logs in $out"
summary_re='^[0-9]+ tests, [0-9]+ failed'
had_failures=0
for ((i = 1; i <= runs; i++)); do
	log=$(printf '%s/run-%03d.log' "$out" "$i")
	start=$SECONDS
	if [ -n "$filter" ]; then
		timeout "$limit" "$tests" "$filter" > "$log" 2>&1
	else
		timeout "$limit" "$tests" > "$log" 2>&1
	fi
	code=$?
	took=$((SECONDS - start))
	summary=$(grep -E "$summary_re" "$log" | tail -n 1 | tr -d '\r')
	if [ "$code" -eq 124 ]; then
		last=$(grep -E '^(ok    |FAILED|known  |FIXED\?) ' "$log" | tail -n 1 | tr -d '\r')
		echo "run $i: HUNG: no end after ${limit} s; the last test that finished: ${last:-none} ($log)"
		exit 2
	fi
	if [ -z "$summary" ]; then
		crash=$(grep -E '^(CRASHED|EXITED) in ' "$log" | tail -n 1 | tr -d '\r')
		last=$(grep -E '^(ok    |FAILED|known  |FIXED\?) ' "$log" | tail -n 1 | tr -d '\r')
		echo "run $i: ENDED EARLY with exit code $code after ${took} s: ${crash:-no CRASHED/EXITED line}; the last test that finished: ${last:-none} ($log)"
		exit 2
	fi
	if [ "$code" -ne 0 ]; then
		had_failures=1
		echo "run $i: $summary (exit code $code, ${took} s); failed: $(grep -E '^(FAILED|FIXED\?) ' "$log" | tr -d '\r' | sed -E 's/^(FAILED|FIXED\?) //; s/: passes now.*$/ (passes now)/' | awk '{printf "%s%s", (NR > 1 ? "; " : ""), $0}') ($log)"
	else
		echo "run $i: $summary (${took} s)"
	fi
done
echo "testloop: $runs runs, every one ended with a summary$([ "$had_failures" -eq 1 ] && echo '; some had failed tests (above)')"
exit "$had_failures"
