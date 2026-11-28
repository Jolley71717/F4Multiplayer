#!/bin/bash
# castle-siege.sh: the Castle's siege (MinDefendCastle, 000BDB04) is the session host's, as its settlement attacks are
# (common/AttackBook.h, SIEGES; src/game/SettlementAttacks.cpp). Before, each game ran its own siege at the Castle: two sets of waves
# (each game's own, and the copies of the other's), each game's end stage set by its own waves' deaths, and a friend's own siege could
# begin while the host's was not on. Now the host's waves are the only ones, a friend's own fight is held back (its 150 marked done
# without its script), and the host's end stage (200, the waves dead) sets the friend's own 200 once.
#   1. Both games at the Castle (console coc $CASTLE_CELL when far, said), MinDefendCastle running in both with its 70 done (the player
#      arrived) and 150 not: a save after Taking Independence with the siege started. CASTLE_SETUP=1 starts it by console in a game
#      that hasn't it (startquest, setstage 70: said); otherwise VOID. The Castle's workbench (the siege's Workshop alias, read off
#      dev `attacks`) owned by the player in both games, else VOID.
#   2. The host's fight begins: console setstage 000BDB04 150 on the host (it stands for the 2 game-hour timer that its 70 started).
#      Within $SETTLE s the VM's game must hold its own (its log: "our siege 000BDB04 ... is held"), its 150 done with no fight of its
#      own (its log has no "began before the host's word"), and the host's attackers reach it (the host shares spawns, the VM has
#      copies of them near the Castle's workbench); the VM's objective 30 is shown while its player and those copies are there.
#   3. Both players fight the same wave: the VM's player shoots the host's attackers' copies in front of it (real clicks, up to
#      $SHOTS; its kills are said, and the host's game's deaths come through the usual hit sync), the host fights in its own game.
#      Then the rest of the wave: console setstage 000BDB04 200 on the host (it stands for DoBattleEnd, 98% dead; said).
#   4. The end stage once in each game: the host's 200 done and reported once (its log: one "phase 2" report of 000BDB04); the VM's
#      200 done, set once (its log: one "sets stage 200 (its end, once)"), still once $SETTLE s later, and no fight of its own
#      begun all the while.
#   5. The Castle stays owned: its workbench's owner the player in both games after the siege.
# REPRO: YES when any of 2-5 fails (named); NO when all hold. VOID when the scene can't be made (no session, the quest not at its
# 70, the Castle not owned, no attackers shared). It changes both saves (the siege won): throwaway saves only.
# Rig 2026-11-28: steps 2 and 4 held (the friend held its siege at once, the host's 200 set the friend's once); step 3 got no kills;
# a friend's siege started by console after the host's was booked had its own defenders (its 10) beside the copies of the host's.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
QUEST=000BDB04                       # MinDefendCastle
CASTLE_CELL=${CASTLE_CELL:-TheCastleExt}
CASTLE_SETUP=${CASTLE_SETUP:-0}
SETTLE=${SETTLE:-30}                 # s, for a phase to reach the other game and its look to take a step (looks every 0.5 s)
SHOTS=${SHOTS:-15}
run_cap 1800
need_session

# done_has <who> <stage>: the stage is done in that game (dev `stages`: done(<n>)=<a,b,...>)
done_has() { dc "$1" "stages $QUEST" | grep -oE 'done\([0-9]+\)=[0-9,a-z]+' | cut -d= -f2 | tr ',' '\n' | grep -qx "$2"; }
running() { [ $(( 0x$(field "$(dc "$1" "stages $QUEST")" flags) & 1 )) -ne 0 ]; }
siege_entry() { dc "$1" attacks | tr ';' '\n' | grep -E "^ ?$QUEST .* siege " | head -1; }
bench_of() { field "$(siege_entry "$1")" workshop; }
# refinfo shows no owner for a workbench: its script's OwnedByPlayer does (as workbench-not-owned.sh reads it)
owned() { [ "$(field "$(dc "$1" "scriptvars $2 WorkshopScript OwnedByPlayer")" OwnedByPlayer)" = true ]; }
count_since() { rsince "$1" "$2" | grep -cE "$3"; }
info() { dc "$1" "refinfo $2"; }
dead() { [ "$(field "$(info "$1" "$2")" dead)" = true ]; }

# 1. both at the Castle, the siege waiting at its 70
for w in host vm; do
	if ! running $w || ! done_has $w 70; then
		[ "$CASTLE_SETUP" = 1 ] || verdict VOID "$w's MinDefendCastle isn't running at its 70 ($(dc $w "stages $QUEST" | cut -c1-120)): load a save with the siege started (or CASTLE_SETUP=1)"
		say "set-up: $w's siege started by console (startquest $QUEST, setstage $QUEST 70)"
		running $w || { dc $w "console startquest $QUEST" >/dev/null; sleep 3; }
		dc $w "console setstage $QUEST 70" >/dev/null; sleep 2
	fi
	# The friend's 150 is marked done at once when the host's run is already booked: that is the hold, not a fight
	if done_has $w 150 && ! { [ $w = vm ] && field "$(siege_entry vm)" held | grep -qx true; }; then
		verdict VOID "$w's siege has its 150 done already: start before its fight"
	fi
done
BENCH=$(bench_of host)
[ -n "$BENCH" ] || verdict VOID "the host's dev 'attacks' lists no siege entry with a workbench: $(dc host attacks | cut -c1-160)"
for w in host vm; do
	b=$(bench_of $w); [ "$b" = "$BENCH" ] || verdict VOID "$w's siege is at workbench '$b', the host's at $BENCH"
	owned $w "$BENCH" || verdict VOID "the Castle's workbench $BENCH isn't the player's in $w's game ($(field "$(info $w "$BENCH")" owner))"
	if [ "$(field "$(info $w "$BENCH")" 3d)" != true ]; then
		say "set-up: $w is not at the Castle: console coc $CASTLE_CELL"
		dc $w focus >/dev/null; dc $w "console coc $CASTLE_CELL" >/dev/null; sleep 12; wait_ingame $w 60
	fi
	dc $w "console player.additem 00004822 1" >/dev/null; dc $w "console player.additem 0001F276 300" >/dev/null
	dc $w "console player.equipitem 00004822" >/dev/null; dc $w "draw on" >/dev/null
	say "set-up: $w at $(pos_of $w), a 10mm and 300 rounds (console additem); its siege: $(dc $w "stages $QUEST" | cut -c1-140)"
done
on_end dc vm "console tgm"   # god mode back off on the VM if the run set it
dc vm "console tgm" >/dev/null   # the VM's player survives the wave while it looks
BAD=""
rmark host run; rmark vm run

# 2. the host's fight begins; the VM's held, the host's attackers reach it
say "the host's fight begins: console setstage $QUEST 150 on the host (standing for its timer)"
dc host "console setstage $QUEST 150" >/dev/null
_held() { rsince vm run | grep -qE "SettlementAttacks: our siege $QUEST .* is held"; }
scene_live "the VM's game holds its own siege for the host's" "$SETTLE" _held
done_has vm 150 || BAD="$BAD the VM's 150 isn't done though held;"
[ "$(count_since vm run 'began before the host.s word')" = 0 ] || BAD="$BAD the VM's own fight began;"
_shared() { rsince host run | grep -qE 'Spawns: sharing [0-9A-F]{8}'; }
scene_live "the host's game shares its attackers" 120 _shared
_copies() { dc vm 'actors 8000' | tr ';' '\n' | grep -E '^ ?FF[0-9A-F]{6} ' | grep -q 'dead=false'; }
scene_live "the VM has the host's attackers' copies near the Castle" 120 _copies
sleep 5
rsince vm run | grep -qE "our objective 30 is shown" || say "note: the VM's objective 30 wasn't shown yet ($(rsince vm run | grep -E 'SettlementAttacks: .*objective' | tail -1))"
say "[wave] host: $(dc host 'net status' | grep -oE 'attacks: [^|]*' | cut -c1-200)"
say "[wave] vm: $(dc vm 'net status' | grep -oE 'attacks: [^|]*' | cut -c1-200)"

# 3. the VM fights the host's wave: its kills reach the host's game
kills=0
for i in $(seq 1 "$SHOTS"); do
	target=$(dc vm 'actors 4000' | tr ';' '\n' | grep -E '^ ?FF[0-9A-F]{6} ' | grep 'dead=false' | grep -oE '^ ?FF[0-9A-F]{6}' | tr -d ' ' | head -1)
	[ -n "$target" ] || break
	dc vm "turnto $target 300 aim" >/dev/null; sleep 0.5; dc vm "click 80" >/dev/null; sleep 1.2
	if dead vm "$target"; then
		kills=$((kills + 1))
		say "[wave] the VM killed the copy $target"
	fi
done
say "[wave] the VM's player killed $kills of the host's attackers in $SHOTS tries"

# 4. the end stage once in each game
say "the rest of the wave: console setstage $QUEST 200 on the host (standing for DoBattleEnd)"
dc host "console setstage $QUEST 200" >/dev/null
_vm_end() { done_has vm 200; }
wait_for "$SETTLE" _vm_end || BAD="$BAD the VM's 200 wasn't set within $SETTLE s of the host's;"
sleep "$SETTLE"
hr=$(count_since host run "our attack $QUEST .* phase 2")
ve=$(count_since vm run "sets stage 200 \(its end, once\)")
[ "$hr" = 1 ] || BAD="$BAD the host reported its 200 $hr times;"
[ "$ve" = 1 ] || BAD="$BAD the VM set its 200 $ve times;"
done_has host 200 || BAD="$BAD the host's 200 isn't done;"
[ "$(count_since vm run 'began before the host.s word')" = 0 ] || BAD="$BAD the VM's own fight began;"
say "end: host 200 reported $hr time(s); the VM set its 200 $ve time(s); host $(dc host "stages $QUEST" | cut -c1-120); vm $(dc vm "stages $QUEST" | cut -c1-120)"

# 5. the Castle stays owned in both
for w in host vm; do owned $w "$BENCH" || BAD="$BAD the Castle's workbench isn't the player's in $w's game after ($(field "$(info $w "$BENCH")" owner));"; done
rsince vm run | grep -E 'SettlementAttacks:' | tail -12 | sed 's/^/  vm| /' | tee -a "$REPRO_OUT/run.log"
rkeep host castle-siege; rkeep vm castle-siege
[ -n "$BAD" ] && verdict YES "the Castle's siege isn't one in both games:$BAD"
verdict NO "one wave (the host's), the VM's held; the end stage once in each game; the Castle owned in both"
