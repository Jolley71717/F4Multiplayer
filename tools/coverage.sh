#!/usr/bin/env bash
# Line coverage of the unit tests, on Linux with gcc (CI: .github/workflows/tests.yml; also WSL Ubuntu 24.04).
#
#   tools/coverage.sh [test name filter]
#
# Builds F4MPTests with --coverage -O0 into build/coverage (with its own xmake configuration there, so the checkout's
# own `xmake f` is left alone), runs it from scratch (old .gcda files are deleted first) and writes, in
# build/coverage/report (or $COVERAGE_OUT):
#   tests.log      the tests' output
#   summary.txt    gcovr's text report, one line per file
#   coverage.xml   Cobertura XML
#   summary.json   gcovr's JSON summary, per file
#   coverage.json  the totals: {"total": {"covered", "lines"}, "dirs": {"common/": {...}, ...}, "tests": {"run", "failed", "known"}}
#   coverage.md    a short markdown table: the total and each top-level folder
# Measured: common/ and src/ (the code the plugin, the server and the bot share; the tests themselves are not counted).
# On Linux only the game-free targets build (the plugin needs MSVC), so of src/ only the headers the tests use count.
#
# Exit code: F4MPTests's own, the number of failed tests (a known bug still failing is not one), with the reports
# written either way; a failed build or report exits non-zero with an error first.
# Needs: g++ 14 or newer (C++23) and its gcov, xmake 3.1, gcovr 7 and python3. $CXX, $CC and $GCOV pick others;
# by default g++-14, gcc-14 and gcov-14 when they are installed, else g++, gcc and gcov.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/build/coverage"
out=${COVERAGE_OUT:-$build/report}
filter=${1:-}

die() { echo "coverage.sh: $*" >&2; exit 125; }

if [ -z "${CXX:-}" ] && command -v g++-14 > /dev/null; then CXX=g++-14 CC=gcc-14; fi
CXX=${CXX:-g++}
CC=${CC:-gcc}
command -v "$CXX" > /dev/null || die "no $CXX (g++ 14 or newer is needed)"
major=$("$CXX" -dumpversion | cut -d. -f1)
[ "$major" -ge 14 ] 2> /dev/null || die "$CXX is gcc $major; C++23 here needs 14 or newer"
if [ -z "${GCOV:-}" ]; then
	if command -v "gcov-$major" > /dev/null; then GCOV="gcov-$major"; else GCOV=gcov; fi
fi
for tool in xmake gcovr python3 "$GCOV"; do
	command -v "$tool" > /dev/null || die "no $tool"
done

cd "$root"
mkdir -p "$build" "$out"
# The project's xmake configuration for this build lives in build/coverage, not in .xmake/.
export XMAKE_CONFIGDIR="$build/.xmake-config"

echo "== configure ($CXX, $GCOV)"
# Atomic counters: the server and the tests run threads, and plain counters raced into negative counts that gcov
# then reports (gcc bug 68080); gcovr is told to warn about any left rather than fail.
xmake f -y -p linux -m releasedbg -o "$build" --cc="$CC" --cxx="$CXX" --ld="$CXX" \
	--cxxflags="--coverage -O0 -fprofile-update=atomic" --ldflags="--coverage" || die "xmake configure failed"
echo "== build F4MPTests"
xmake build -y F4MPTests || die "the build failed"

tests=$(find "$build" -type f -name F4MPTests -perm -u+x | head -n 1)
[ -n "$tests" ] || die "no F4MPTests under $build"
find "$build" -name '*.gcda' -delete

echo "== run $tests ${filter:+(only tests matching \"$filter\")}"
set +e
"$tests" ${filter:+"$filter"} > "$out/tests.log" 2>&1
failed=$?
set -e
tail -n 3 "$out/tests.log"

echo "== gcovr"
rm -f "$out/summary.txt" "$out/coverage.xml" "$out/summary.json" "$out/coverage.json" "$out/coverage.md" "$out/gcovr.log"
# gcov writes a block line of a source line past 9999 with no space after the colon ("236:10012-block 2"), which gcovr 7.0
# (Ubuntu 24.04's) can't read, and src/game/StageRules.h is past 10000 lines. gcovr 7.0 can only turn every parse error
# into a warning (--gcov-ignore-parse-errors=all), so its log is read after it: a line it couldn't read that isn't such a
# block line fails the job, so a new gcov format can't quietly drop coverage. Only those block lines' counts are lost.
gcovr -r "$root" --gcov-executable "$GCOV" -j "$(nproc)" --filter 'common/' --filter 'src/' \
	--gcov-ignore-parse-errors=all \
	--txt "$out/summary.txt" --cobertura "$out/coverage.xml" --json-summary "$out/summary.json" --json-summary-pretty \
	"$build" 2> "$out/gcovr.log" || { cat "$out/gcovr.log" >&2; die "gcovr failed"; }

python3 -I - "$out/gcovr.log" <<'PY' || die "gcovr could not read a gcov line other than a block line past line 9999 (see $out/gcovr.log)"
import re, sys
block = re.compile(r"^\s*[^:\s]+:\d{5,}-block\s+\d+\s*$")  # count:line-block n, the line number grown into the colon's space
unread, bad, inside = 0, [], False
for line in open(sys.argv[1], errors="replace").read().splitlines():
    if "Unrecognized GCOV output for" in line:
        inside = True
    elif inside and "This is indicative of a gcov output parse error" in line:
        inside = False
    elif inside:
        unread += 1
        if not block.match(line):
            bad.append(line.strip())
print(f"gcovr: {unread} gcov line(s) unread, {len(bad)} not a block line past 9999")
if bad:
    print(*bad[:20], sep="\n", file=sys.stderr)
    sys.exit(1)
PY

python3 -I - "$out" <<'PY' || die "the summary failed"
import json, re, sys
out = sys.argv[1]
summary = json.load(open(f"{out}/summary.json"))
dirs = {}
for f in summary["files"]:
    top = f["filename"].replace("\\", "/").split("/")[0] + "/"
    d = dirs.setdefault(top, {"covered": 0, "lines": 0})
    d["covered"] += f["line_covered"]
    d["lines"] += f["line_total"]
total = {"covered": summary["line_covered"], "lines": summary["line_total"]}
log = open(f"{out}/tests.log", errors="replace").read()
m = re.search(r"^(\d+) tests, (\d+) failed(?:, (\d+) known bugs? still failing)?", log, re.M)
tests = {"run": int(m[1]), "failed": int(m[2]), "known": int(m[3] or 0)} if m else None
json.dump({"total": total, "dirs": dict(sorted(dirs.items())), "tests": tests}, open(f"{out}/coverage.json", "w"), indent=1)

pct = lambda c: f"{100 * c['covered'] / c['lines']:.1f}%" if c["lines"] else "n/a"
rows = ["| | Lines covered | Lines | Coverage |", "|---|---:|---:|---:|",
        f"| **Total** | {total['covered']} | {total['lines']} | **{pct(total)}** |"]
rows += [f"| `{name}` | {c['covered']} | {c['lines']} | {pct(c)} |" for name, c in sorted(dirs.items())]
if tests:
    ran = f"{tests['run']} tests, {tests['failed']} failed" + (f", {tests['known']} known bugs still failing" if tests["known"] else "")
else:
    ran = "the tests' summary line is missing (did F4MPTests crash?)"
open(f"{out}/coverage.md", "w").write("Unit test line coverage (`common/`, `src/`); " + ran + ".\n\n" + "\n".join(rows) + "\n")
PY

echo
cat "$out/coverage.md"
echo
echo "Reports: $out"
exit "$failed"
