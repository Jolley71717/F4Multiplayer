#!/bin/bash
# containers-on-leave.sh: a friend's own container, with their things in it, stands where the host builds a container, and nothing links
# it to a workbench (one a mod or the console placed): the friend's game took it for a leftover copy of the host's, deleted it and put
# what it held into the friend's pockets (Nexus report on 0.21.x: "any time I leave my friends game all my items I have in my containers
# get put into my inventory"). A player's own container is never a copy and is never emptied into their pockets.
#   1. Both games at Sanctuary, joined. Sanctuary is the host's (console WorkshopScript.SetOwnedByPlayer, as the rig's checks do).
#      The host places a container (console placement: wsreport place + wsfinish) and the friend's game gets its copy: that proves
#      the pieces reach the friend (set-up).
#   2. The friend's copy is removed (console markfordelete), and the friend places a container of their own on its spot (console
#      placeatme, setpos) and puts 777 caps in it (console additem, a from-nothing add the mod never reports).
#   3. The host places a second container on the same spot: the friend's game applies the report with its own container under it.
#   4. The friend leaves the session (net leave). Asserted: the friend's own container still stands with its caps, nothing came
#      into the friend's pockets.
# REPRO: YES when the friend's container was deleted or its caps reached the pockets; NO when it stands with them and the pockets are 0.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE; BASE=00059A9A; CAPS=0000000F
need_session
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) verdict VOID "$w is not at Sanctuary ($loc): console coc SanctuaryExt";; esac
done
owned() { field "$(dc "$1" "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer; }
[ "$(owned host)" = true ] || { say "set-up: the host's Sanctuary made the host's (console $BENCH.cf WorkshopScript.SetOwnedByPlayer 1)"; dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3; }
[ "$(owned host)" = true ] || verdict VOID "the host's Sanctuary is not the host's after SetOwnedByPlayer"
if ! _ws_in host; then ws_enter_real host $BENCH || verdict VOID "the host's real E opened no workshop mode ($ENTER_SAID)"; fi
ws_leave host
place() { local out ref; out=$(dc host "wsreport place $BASE $BENCH"); ref=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1); [ -n "$ref" ] || return 1; sleep 2; dc host "wsfinish $ref $BENCH" >/dev/null; sleep 3; echo "$ref"; }
dc vm "console player.removeitem $CAPS 100000" >/dev/null; sleep 1
H1=$(place) || verdict VOID "the host's container was not placed"
has_copy() { [ -n "$(ws_copy_of vm "$H1")" ]; }
wait_for 20 has_copy || verdict VOID "the friend's game made no copy of the host's container"
C1=$(ws_copy_of vm "$H1"); ri=$(dc vm "refinfo $C1"); X=$(field "$ri" x); Y=$(field "$ri" y); Z=$(field "$ri" z)
say "set-up: the host's container $H1, the friend's copy $C1 at ($X, $Y, $Z)"
rmark vm start
dc vm "console $C1.disable" >/dev/null; dc vm "console $C1.markfordelete" >/dev/null; sleep 3
before=$(dc vm "containers" | grep -oE "FF[0-9A-F]{6} 'Toolbox'" | cut -d' ' -f1 | sort)
dc vm "console player.placeatme $BASE" >/dev/null; sleep 3
U=$(comm -13 <(echo "$before") <(dc vm "containers" | grep -oE "FF[0-9A-F]{6} 'Toolbox'" | cut -d' ' -f1 | sort) | head -1)
[ -n "$U" ] || verdict VOID "the friend's own container was not placed"
for a in "x $X" "y $Y" "z $Z"; do dc vm "console $U.setpos $a" >/dev/null; done
dc vm "console $U.additem $CAPS 777" >/dev/null; sleep 2
say "the friend's own container $U at its spot with $(dc vm "count $U $CAPS") caps in it (pockets: $(dc vm "count 00000014 $CAPS")); linked to no workbench ($(field "$(dc vm "wsinfo $U")" bench))"
H2=$(place) || verdict VOID "the host's second container was not placed"
say "the host placed another container $H2 on the same spot; the friend's game applies it"
sleep 8
say "after the host's piece: the friend's $U $(dc vm "wsinfo $U" | grep -oE 'deleted=[a-z]*|blocked=[a-z]*' | tr '\n' ' '), caps in it $(dc vm "count $U $CAPS"), pockets $(dc vm "count 00000014 $CAPS")"
dc vm "net leave" >/dev/null; sleep 6
inU=$(dc vm "count $U $CAPS"); pocket=$(dc vm "count 00000014 $CAPS"); gone=$(field "$(dc vm "wsinfo $U")" deleted)
say "after leaving: container $U deleted=$gone, caps in it $inU, pockets $pocket"
rsince vm start | grep -E "what it held went|buried|WorkshopSync: copy" | head -5 | while read -r l; do say "friend: ${l:0:200}"; done
rkeep vm containers-on-leave
dc vm "console player.removeitem $CAPS 100000" >/dev/null
if [ "$gone" = true ] || [ "$pocket" != 0 ] || [ "$inU" != 777 ]; then verdict YES "the friend's own container was deleted or emptied (deleted=$gone, caps in it $inU, pockets $pocket)"; fi
verdict NO "the friend's own container stands with its 777 caps, nothing in the pockets"
