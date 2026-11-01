#!/bin/bash
# cleanup.sh scan|run [--enter] [refs-file]
#
# The build-sync checks place a piece in Sanctuary (wsreport place) and used to leave it behind: the pieces were
# saved into the soak save and accumulated. This finds the pieces the tests place, in both games, and takes them
# out: a scrap is reported first (so the other game scraps its copy, as for a player), then the reference is removed.
#   scan   lists them and exits 1 if any is left that a game can still take out
#   run    scraps and removes them (the refs a test wrote to <refs-file>, then whatever a scan still finds), then scans again
# --enter loads Sanctuary in both games first (the scan only sees what is near the player). run and --enter need the rig held (guard.sh: F4MP_RIG_OWNER); a plain scan does not.
# A removed piece is still listed by `refs` (deleted=true), so a scan skips those. What counts as a test piece: a runtime-made reference (id FF......) of a base in $WS_TEST_BASES (the chair, 000DF45C,
# by default) near the player. Nothing else is touched.
#
# Whose a piece is (dev `wsinfo`: copy=, builder=, theirRef=): only its builder's game can scrap a piece for the session
# (WorkshopSync's ReportRef never reports a copy of a friend's piece; the server keeps each piece under its builder's player and
# reference, WorkshopStore). So a game's OWN test pieces are scrapped and removed, and must be gone afterwards (exit 1 if not).
# A COPY of a friend's piece is removed here only (it is sent again at the next join): it is gone for good once its builder's
# game scraps it, so a copy whose builder's game still has the piece is a failure too. A copy whose builder's game no longer
# has it (that game loaded a save from before, or the piece was removed there without a scrap) is an orphan of the session
# file: no game can scrap it, it comes back at every join, and only a session file without it clears it. Orphans are listed
# ("orphaned"), not failed. The rig restores the session files byte for byte after every run (backup.sh), so a session file
# that held orphans before the run holds them again after it.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
. "$RIG/guard.sh"
# field <line> <name>: as assert.sh's (env.sh has none; without it every piece read as the game's own, 2026-10-31)
field() { grep -oE "(^|[] [(;|,:])$2=[^] ;|,)]*" <<<"$1" | head -1 | sed 's/^[^=]*=//'; }
BASES=${WS_TEST_BASES:-000DF45C}; BENCH=${WS_BENCH:-250FE}
# the pieces are furniture (form type 42); `refs` lists at most 40 per call, so a few radii and the name filter are used
NAME=${WS_TEST_NAME:-Chair}; RADII=${WS_SCAN_RADII:-"400 1200 3000 8000"}
mode=$1; shift; enter=0; reffile=""
for a in "$@"; do case "$a" in --enter) enter=1;; *) reffile=$a;; esac; done
# run (and --enter) drive the games: only with the rig held (guard.sh), like smoke.sh and hits.sh. A plain scan only reads.
if [ "$mode" = run ] || [ $enter = 1 ]; then rig_guard || exit 2; fi

scan_one() {  # scan_one <who>: the test pieces this game has near its player, one ref per line
	local r b
	for r in $RADII; do
		for b in $BASES; do
			dc "$1" "refs 42 $r $NAME" | tr ';' '\n' | sed "s/^ *//" | grep -E "^FF[0-9A-F]{6} base=$b" | cut -c1-8 | while read -r q; do case "$(dc "$1" "refinfo $q")" in *"deleted=true"*) ;; *) echo "$q";; esac; done
		done
	done | sort -u
}
# whose <who> <ref>: "own", or "copy <builder's player id> <their ref>" (dev wsinfo)
whose() {
	local w; w=$(dc "$1" "wsinfo $2")
	if [ "$(field "$w" copy)" = true ]; then echo "copy $(field "$w" builder) $(field "$w" theirRef)"; else echo own; fi
}
# held_by_builder <builder id> <their ref>: the game of that player still has the piece: a test base there, not deleted (a
# runtime id is used again after a load, so the id alone says nothing)
held_by_builder() {
	local g w; for g in host vm; do
		[ "$(field "$(dc $g 'net status')" id)" = "$1" ] || continue
		w=$(dc $g "wsinfo $2"); [ "$(field "$w" deleted)" = false ] || continue
		case " $BASES " in *" $(field "$w" base) "*) return 0;; esac
	done
	return 1
}

[ $enter = 1 ] && { enter_both SanctuaryExt 14; sleep 2; }
left_total=0
if [ "$mode" = run ]; then
	for who in host vm; do
		refs="$(scan_one $who)"
		if [ -n "$reffile" ] && [ "$who" = host ] && [ -s "$reffile" ]; then refs="$refs
$(cat "$reffile")"; fi
		for r in $(echo "$refs" | sort -u | grep -E '^[0-9A-F]{8}$'); do
			case "$(whose $who $r)" in
			own) echo "cleanup: $who scraps and removes its own test piece $r"; dc $who "wsreport $r $BENCH scrap" >/dev/null; sleep 1; dc $who "remove $r" >/dev/null;;
			*) echo "cleanup: $who removes its copy $r of a friend's piece ($(whose $who $r)): only the builder's game can scrap it"; dc $who "remove $r" >/dev/null;;
			esac
		done
		sleep 3
	done
fi
for who in host vm; do
	own=""; held=""; orphans=""
	for r in $(scan_one $who); do
		read -r kind b t <<<"$(whose $who $r)"
		if [ "$kind" = own ]; then own="$own $r"
		elif held_by_builder "$b" "$t"; then held="$held $r(player $b's $t)"
		else orphans="$orphans $r(player $b's $t)"; fi
	done
	if [ -n "$own$held" ]; then echo "cleanup: $who still has test pieces:${own:+ own$own}${held:+ copies whose builder still has them$held}"; left_total=$((left_total + 1)); else echo "cleanup: $who has no test pieces a game can take out near the player"; fi
	[ -n "$orphans" ] && echo "cleanup: $who orphaned: copies of pieces only the session file still holds (their builder's game has none):$orphans"
done
[ $left_total -eq 0 ]
