#!/bin/bash
# smoke.sh [--only step,step,...] [--skip step,...] [--churn MINUTES] [--soak MINUTES]
#
# The standing two-game set with a verdict. The rc gate's step 2 (docs/RELEASING.md): every number the old soak only
# printed is now a named check with a threshold (tools/rig/steps.sh holds them), the plugin log and the crash logs are
# read, and the exit code is the number of failed checks (0 = PASS), plus any check the rig never managed to make (a rig fault:
# the clocks could not line a trace up: an offset unread over ssh, or a clock set outside the trace; recorded RIG, not FAIL, and
# VERDICT: RIG when no round made that check). About 30 minutes.
# Each loop step starts behind the state guard (state.sh, run() below): a wrong rig state is healed (HEAL) or, when it can't be,
# the step is not run (RIGFAULT); a step that met a rig fault has its lines from that fault on set aside (VOID) and runs once
# more. A heal or a re-run never erases evidence of the mod's failure (a FAIL before the fault, or in both attempts, stands; a
# PASS names what was set aside). A RIGFAULT never ends in PASS: a FAIL is the mod's, a rig that stayed broken is VERDICT: RIG.
#
# --soak MINUTES is the rc soak (docs/RELEASING.md): after preflight and cleanup it runs the loop steps (movement to churn,
# each with its checks, both ways where a step has two) round after round until the time is up, then the end steps
# (reconnect, fuzz, the cleanup and final checks) once. A round is 25 to 30 minutes with the default 3-minute churn; the
# next starts only while at least half the last one's length is left, so a run ends within about half a round of the
# time asked. Each round's sections are named "(round N)". --only and --skip still pick the steps (leaving none of the loop steps
# is refused up front). A round that starts and doesn't finish, or a loop that ends early, is a FAIL.
# The bench step needs the host's bSharedStorage = true (F4Multiplayer.ini); without it, it fails saying so.
# The questactors step needs the host's sQuests = safe (F4Multiplayer.ini); without it, it fails saying so.
#
# Needs both games running and joined (tools/rig: `rig.sh deploy` then `rig.sh join` in the rig's stage folder), and the
# rig held by you: take it first, `bash $STAGE/rigq.sh take <name>`, and pass that name as F4MP_RIG_OWNER.
# SMOKE_ZIP=<rc zip> also checks that the DLL on both machines is the zip's; CHECK_DLL=<a built F4Multiplayer.dll, or its md5> that it
# is that build (a branch's: rig.sh deploy copies the main checkout's build folder). A mismatch stops the run.
#
# Steps, in order: preflight cleanup, the loop steps movement combat terminals grenades trails workshop bench loot doors quests questactors drops downed
# pa_vm pa_host churn, then reconnect fuzz final (our frame cost is read after each loop step)
. "$(dirname "${BASH_SOURCE[0]}")/assert.sh"
. "$RIG/steps.sh"
. "$RIG/crashlib.sh"
. "$RIG/guard.sh"
. "$RIG/state.sh"

LOOP="movement combat terminals grenades trails workshop bench loot doors quests questactors drops downed pa_vm pa_host churn"
ALL="preflight cleanup $LOOP reconnect fuzz final"
only=""; skip=""; churn_min=${SMOKE_CHURN_MIN:-3}; soak_min=""
while [ $# -gt 0 ]; do case "$1" in --only) only=${2//,/ }; shift 2;; --skip) skip=${2//,/ }; shift 2;; --churn) churn_min=$2; shift 2;; --soak) soak_min=$2; shift 2;; *) echo "unknown option $1"; exit 2;; esac; done
[ -z "$soak_min" ] || [[ $soak_min =~ ^[1-9][0-9]*$ ]] || { echo "--soak wants whole minutes, at least 1"; exit 2; }
steps=${only:-$ALL}
if [ -n "$soak_min" ]; then   # --soak repeats the loop steps: with --only/--skip leaving none there is nothing to repeat, and it would spin to the time limit
	n=0; for s in $LOOP; do case " $skip " in *" $s "*) continue;; esac; case " $steps " in *" $s "*) n=1;; esac; done
	[ $n = 1 ] || { echo "--soak: --only/--skip leave none of the loop steps ($LOOP) to repeat"; exit 2; }
fi
rig_guard || exit 2
a_init "$([ -n "$soak_min" ] && echo soak || echo smoke)"; A_NOTE=""
# Ctrl-C or TERM: the virtual pads are unplugged first (a pad left plugged takes the keyboard and mouse from the next run), once: a second
# signal while a game is slow to answer goes straight to the verdict.
smoke_interrupted() { [ -z "$SMOKE_INT" ] && { SMOKE_INT=1; pads_off; }; a_interrupted; }
trap smoke_interrupted INT TERM
tunnel
# run <step> <function...>: one step. A loop step starts behind the state guard (state.sh: both players out of power armor, up,
# nothing open, a normal camera, god mode on; a wrong state is healed, one that stays is a RIGFAULT and the step is not run).
# An attempt that meets a rig fault (a RIG line: a clock that couldn't line a trace up, a set-up walk that never arrived, a press
# that never reached the game; or a FAIL saying "rig broken") has its lines from that rig line on set aside (VOID; assert.sh
# a_attempt_void: what was recorded before it stands, and a clock fault sets nothing aside), the guard runs again and the step
# is run once more, so its checks are still made and a failure that followed from the rig's fault never stands as the mod's. A
# second attempt that meets one too is a RIGFAULT (its lines from its rig line on set aside, but a check that failed in both
# attempts stays a FAIL): the rig was broken, the run is not a PASS.
run() {
	case " $skip " in *" $1 "*) return;; esac; case " $steps " in *" $1 "*) ;; *) return;; esac
	local loop="" sig first
	case " $LOOP " in *" $1 "*) loop=1;; esac
	if [ -n "$loop" ] && ! rig_state_guard "$1"; then ST_PREV=$1; return 0; fi
	a_attempt_mark; first=$A_ATTEMPT_LINE; "${@:2}"
	if [ -n "$loop" ] && [ -z "$A_RIG_STOPPED" ] && sig=$(a_attempt_rig); then
		a_attempt_void
		echo "RIG: $1 met a rig fault ($sig): its lines from there on are set aside; healed and run again"
		healed "$1${A_ROUND:+ (round $A_ROUND)}: run again after a rig fault" "$sig"
		ST_PREV=$1
		if rig_state_guard "$1"; then
			A_AGAIN=1; a_attempt_mark; "${@:2}"; A_AGAIN=""
			if [ -z "$A_RIG_STOPPED" ] && sig=$(a_attempt_rig); then
				a_attempt_void "$first"
				local s; s=$(sed -nE 's/.*rig (fault|broken) \(([a-z]+)\).*/\2/p' <<<"$sig" | head -1)
				rig_unhealed "${s:-rig}" "$1${A_ROUND:+ (round $A_ROUND)}: a rig fault in both attempts" "$sig"
			fi
		fi
	fi
	[ -n "$loop" ] && { ST_PREV=$1; perf_phase "$1"; }
	[ "$1" = combat ] && [ -z "$WS_MARKED" ] && { ws_mark; WS_MARKED=1; }   # the working set's growth is read from the first combat on
	[ "$1" = churn ] && ws_check; return 0
}
loop_steps() {
	run movement  step_movement
	run combat    step_combat
	run terminals step_terminals
	run grenades  step_grenades
	run trails    step_trails
	run workshop  step_workshop
	run bench     step_bench
	run loot      step_loot
	run doors     step_doors
	run quests    step_quests
	run questactors step_questactors
	run drops     step_drops
	run downed    step_downed
	run pa_vm     step_pa vm
	run pa_host   step_pa host
	run churn     step_churn "$churn_min"
}

run preflight step_preflight
run cleanup   smoke_cleanup_start
if [ -z "$soak_min" ]; then loop_steps
else
	soak_end=$((SECONDS + soak_min * 60)); last=0; A_ROUND=0; A_DONE=0
	while [ $((SECONDS + last / 2)) -lt "$soak_end" ] && [ -z "$A_RIG_STOPPED" ]; do
		A_ROUND=$((A_ROUND + 1)); t=$SECONDS; echo; echo "######## soak round $A_ROUND ($(( (soak_end - SECONDS + 59) / 60 )) min left)"
		loop_steps; A_DONE=$A_ROUND; last=$((SECONDS - t))
	done
	echo; echo "######## soak: $A_ROUND round(s) of the loop steps in $(( (SECONDS - soak_end) / 60 + soak_min )) min; the end steps follow"; A_ROUND=""
fi
# Belt and braces: a shell error inside a round (a syntax error in arithmetic) ends the whole if/while above without a word, skipping the
# rest of the round, every later round and the summary line. A round that started and never finished, or a loop that ended with more than
# the half round the rule above allows still unspent, is a FAIL, not a short PASS.
if [ -n "$soak_min" ] && [ -z "$A_RIG_STOPPED" ]; then
	if [ "$A_DONE" != "$A_ROUND" ] && [ -n "$A_ROUND" ]; then fail "soak: every round that started ran to its end" "round $A_ROUND started and did not finish (a shell error ended the soak loop?): rounds finished $A_DONE"; A_ROUND=""
	elif [ "$A_DONE" = 0 ]; then fail "soak: at least one round ran" "no round of the loop steps ran"
	elif [ $((SECONDS + last / 2)) -lt "$soak_end" ]; then fail "soak: ran as long as asked" "$(( (SECONDS - soak_end) / 60 + soak_min )) min of the $soak_min asked for, with more than half a round ($((last / 120)) min) left"; fi
fi
run radiants  step_radiants
run follow    step_follow     # opt-in (--only follow): it sets quest stages for good, on throwaway saves
run sharedws-power step_sharedws_power  # opt-in (--only sharedws-power): the shared workshop's build and wire hand-over, with in-session saves (throwaway saves)
run sharedws-prereq step_sharedws_prereq  # opt-in (--only sharedws-prereq): a deleted copy stays deleted across cell reloads, a save and a solo load; machinegun turrets shared, their buried copies survive two loads (throwaway saves)
run sharedws-scrap step_sharedws_scrap  # opt-in (--only sharedws-scrap): a friend's scrap of the host's piece as a request (the real Scrap key and confirmation; throwaway saves)
run quests_safe step_quests_safe  # opt-in (--only quests_safe): SAFE's newer quests by the host's console setstage, on throwaway saves
run encounter-fight step_encounter_fight  # opt-in (--only encounter-fight): walks both to two RE triggers; quest stages for good, on throwaway saves
run radiant-rr step_radiant_rr  # opt-in (--only radiant-rr): a Railroad job's courser made at runtime shared by its run; quest stages for good, on throwaway saves
run radiant-inst step_radiant_inst  # opt-in (--only radiant-inst): an Institute job's calm synth made at runtime shared by its run; quest stages for good, on throwaway saves
run companions step_companions  # opt-in (--only companions): recruits and dismisses Dogmeat by console, kills and restarts the VM's game
run companions_human step_companions_human  # opt-in (--only companions_human): the Companion slot with Codsworth at Sanctuary, by console
run attacks   step_attacks    # opt-in (--only attacks): rolls attacks on the host's Sanctuary by console and sets their stages, on throwaway saves
run hush-probe step_hush_probe  # opt-in (--only hush-probe): the save's hush among 150+ powered pieces at Sanctuary, travels and saves both ways (throwaway saves)
run attacks-addons step_attacks_addons  # opt-in (--only attacks-addons): the add-ons' attacks, one each, started on the host's Sanctuary by radiantstart, on throwaway saves
run quests_outcomes step_quests_outcomes  # opt-in (--only quests_outcomes): the add-ons' endings set for good, on throwaway saves (QOUT_SWAP=1 the other way)
run companion_quests step_companion_quests  # opt-in (--only companion_quests): Curie's and MacCready's quests by console setstage, on throwaway saves; restarts the VM's game
run companion_quests_2 step_companion_quests_2  # opt-in (--only companion_quests_2): Cait's quest, Nick's after the Memory Den and MS07c 305 then MS07cPostQuest 20 by console setstage, on throwaway saves
run rewards   step_rewards    # opt-in (--only rewards): it sets quest stages for good, on throwaway saves; REWARD_AWAY=1 turns the second (the sale) in with the VM's game killed, then restarts it
run reconnect step_reconnect
run fuzz      step_fuzz 100
smoke_cleanup_end() {
	section "cleanup (test pieces left in Sanctuary)"
	local r left="" st rc
	bash "$RIG/cleanup.sh" run "$A_OUT/ws-placed.txt" > "$A_OUT/cleanup-end.log" 2>&1; rc=$?; tail -4 "$A_OUT/cleanup-end.log"
	# What this run placed must be gone, and so must every test piece a game can still take out (its own, or a copy whose builder's
	# game still has it: cleanup.sh exits 1). Copies of pieces only the session file still holds (orphans: no game has them to scrap)
	# come back at every join until the session file is one without them; they are listed, and are not this run's doing.
	# Removed is deleted=true, or "no such reference" once the game has let the scrapped piece go altogether (2026-11-05).
	for r in $(cat "$A_OUT/ws-placed.txt" 2>/dev/null); do st=$(host "refinfo $r"); case "$st" in *"deleted=true"*|*"no such reference"*) ;; *) left="$left $r";; esac; done
	if [ -z "$left" ]; then pass "the pieces this run placed are removed" "$(wc -w < "$A_OUT/ws-placed.txt" 2>/dev/null) piece(s)"; else fail "the pieces this run placed are removed" "still there:$left"; fi
	if [ "$rc" = 0 ]; then pass "no test piece left that a game can take out (own pieces, copies whose builder has them)" "$(grep -c orphaned "$A_OUT/cleanup-end.log") game(s) show orphans of the session file (cleanup-end.log)"
	else fail "no test piece left that a game can take out (own pieces, copies whose builder has them)" "$(grep 'still has' "$A_OUT/cleanup-end.log" | tr '\n' ' ' | cut -c1-170)"; fi
}
case " $skip " in *" cleanup "*) ;; *) case " $steps " in *" cleanup "*) smoke_cleanup_end;; esac;; esac
run final     step_final
a_summary
