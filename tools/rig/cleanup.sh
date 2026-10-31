#!/bin/bash
# cleanup.sh scan|run [--enter] [refs-file]
#
# The build-sync checks place a piece in Sanctuary (wsreport place) and used to leave it behind: the pieces were
# saved into the soak save and accumulated. This finds the pieces the tests place, in both games, and takes them
# out: a scrap is reported first (so the other game scraps its copy, as for a player), then the reference is removed.
#   scan   lists them and exits 1 if there are any
#   run    scraps and removes them (the refs a test wrote to <refs-file>, then whatever a scan still finds), then scans again
# --enter loads Sanctuary in both games first (the scan only sees what is near the player).
# A removed piece is still listed by `refs` (deleted=true), so a scan skips those. What counts as a test piece: a runtime-made reference (id FF......) of a base in $WS_TEST_BASES (the chair, 000DF45C,
# by default) near the player. Nothing else is touched.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
BASES=${WS_TEST_BASES:-000DF45C}; BENCH=${WS_BENCH:-250FE}
# the pieces are furniture (form type 42); `refs` lists at most 40 per call, so a few radii and the name filter are used
NAME=${WS_TEST_NAME:-Chair}; RADII=${WS_SCAN_RADII:-"400 1200 3000 8000"}
mode=$1; shift; enter=0; reffile=""
for a in "$@"; do case "$a" in --enter) enter=1;; *) reffile=$a;; esac; done

scan_one() {  # scan_one <who>: the test pieces this game has near its player, one ref per line
	local r b
	for r in $RADII; do
		for b in $BASES; do
			dc "$1" "refs 42 $r $NAME" | tr ';' '\n' | sed "s/^ *//" | grep -E "^FF[0-9A-F]{6} base=$b" | cut -c1-8 | while read -r q; do case "$(dc "$1" "refinfo $q")" in *"deleted=true"*) ;; *) echo "$q";; esac; done
		done
	done | sort -u
}

[ $enter = 1 ] && { enter_both SanctuaryExt 14; sleep 2; }
left_total=0
if [ "$mode" = run ]; then
	for who in host vm; do
		refs="$(scan_one $who)"
		if [ -n "$reffile" ] && [ "$who" = host ] && [ -s "$reffile" ]; then refs="$refs
$(cat "$reffile")"; fi
		for r in $(echo "$refs" | sort -u | grep -E '^[0-9A-F]{8}$'); do
			echo "cleanup: $who removes test piece $r"
			dc $who "wsreport $r $BENCH scrap" >/dev/null; sleep 1
			dc $who "remove $r" >/dev/null
		done
		sleep 3
	done
fi
for who in host vm; do
	left="$(scan_one $who | tr '\n' ' ')"
	if [ -n "$left" ]; then echo "cleanup: $who still has test pieces: $left"; left_total=$((left_total + 1)); else echo "cleanup: $who has no test pieces near the player"; fi
done
[ $left_total -eq 0 ]
