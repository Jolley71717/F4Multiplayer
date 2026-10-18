#!/bin/bash
# tools/mutate.sh <max per header> <header>...: mutation testing by hand, run from a clean checkout (it edits and restores the headers). For each header, make small mutants
# (operator flips, boundary changes, constants, dropped conditions), one at a time: apply, rebuild F4MPTests, run the
# header's own tests first (cheap kill), then the whole suite; a mutant no test fails on is a survivor. The file is
# restored after each. Results: build/mutation/<name>.txt, one line per mutant: KILLED|SURVIVED|BUILD_FAIL line: diff.
# Plain declarations with a default value ("bool x = false;") are skipped: every caller sets them (equivalent mutants).
MAX=$1; shift 1
X="/c/Program Files/xmake/xmake.exe"; O=build/mutation; mkdir -p $O
cd "$(dirname "$0")/.." || exit 1
export _MSPDBSRV_ENDPOINT_="mut-$$"
powershell -NoProfile -Command "(Get-Process -Id $(cat /proc/$$/winpid)).PriorityClass = 'BelowNormal'" >/dev/null 2>&1
T=build/windows/x64/releasedbg/F4MPTests.exe
for H in "$@"; do
	name=$(basename "$H" .h); out=$O/$name.txt; : > $out
	word=$(echo "$name" | sed 's/\([A-Z]\)/ \1/g' | awk '{print tolower($1)}')  # "BlastShield" -> "blast": the tests named for it
	# Candidate mutants: "line<TAB>original<TAB>mutated", code lines only (no comments, includes, pragmas, declarations).
	perl -ne 'chomp; s/\r$//; $n=$.; next if /^\s*(\/\/|#)/ || /^\s*$/; next if /^\s*[\w:<>,]+\s+\w+\s*=\s*[^;(]+;/; $o=$_;
		my @ops=(["==","!="],["!=","=="],["<=","<"],[">=",">"],["<","<="],[">",">="],["&&","||"],["||","&&"],["true","false"],["false","true"],["+ 1","- 1"],["- 1","+ 1"],["return true","return false"],["return false","return true"],["++","--"],["\\* 2","* 1"]);
		for my $op (@ops) { my ($f,$t)=@$op; my $i=index($o,$f); next if $i<0; next if $f eq "<" && ($o=~/#include|template|std::|<[A-Za-z_:]+>/); next if $f eq ">" && ($o=~/->|#include|std::|<[A-Za-z_:]+>/);
			my $m=$o; substr($m,$i,length $f)=$t; next if $m eq $o; print "$n\t$o\t$m\n"; }' "$H" | awk -F'\t' '!seen[$1 FS $3]++' | shuf --random-source=<(yes) | head -n $MAX > $O/$name.cands
	total=$(wc -l < $O/$name.cands); i=0
	while IFS=$'\t' read -r line orig mut; do
		i=$((i+1))
		# Apply by line number, keeping CRLF.
		perl -i -pe 'BEGIN{$l=shift; $m=shift} if ($.==$l) { s/^.*?(\r?)$/$m$1/ }' "$line" "$mut" "$H"
		if ! "$X" build -P . -y -j2 F4MPTests >/dev/null 2>&1; then
			echo "BUILD_FAIL $line: $(echo "$orig" | sed 's/^\s*//') -> $(echo "$mut" | sed 's/^\s*//')" >> $out
		else
			# A mutant can make a test loop for ever: a run past 10 minutes counts as killed (the suite takes ~2.5).
			if ! timeout 600 $T "$word" >/dev/null 2>&1; then verdict=KILLED; else timeout 600 $T >/dev/null 2>&1 && verdict=SURVIVED || verdict=KILLED; fi
			echo "$verdict $line: $(echo "$orig" | sed 's/^\s*//') -> $(echo "$mut" | sed 's/^\s*//')" >> $out
		fi
		git checkout -q -- "$H"
		echo "$name $i/$total $(tail -1 $out | cut -c1-100)"
	done < $O/$name.cands
	echo "== $name: $(grep -c ^KILLED $out) killed, $(grep -c ^SURVIVED $out) survived, $(grep -c ^BUILD_FAIL $out) build failures" | tee -a $out
done
"$X" build -P . -y -j2 F4MPTests >/dev/null 2>&1  # leave a clean build behind
