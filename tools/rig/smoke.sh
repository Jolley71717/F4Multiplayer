#!/bin/bash
# smoke.sh [--only step,step,...] [--churn MINUTES] [--skip step,...]
#
# The standing two-game set with a verdict. The rc gate's step 2 (docs/RELEASING.md): every number the old soak only
# printed is now a named check with a threshold (tools/rig/steps.sh holds them), the plugin log and the crash logs are
# read, and the exit code is the number of failed checks (0 = PASS). About 20 minutes.
#
# Needs both games running and joined (tools/rig: `rig.sh deploy` then `rig.sh join` in the rig's stage folder), and the
# rig held by you: take it first, `bash $STAGE/rigq.sh take <name>`, and pass that name as F4MP_RIG_OWNER.
# SMOKE_ZIP=<rc zip> also checks that the DLL on both machines is the zip's.
#
# Steps, in order: preflight cleanup movement combat terminals grenades workshop pa_vm pa_host churn reconnect fuzz final (our frame cost is read after each phase)
. "$(dirname "${BASH_SOURCE[0]}")/assert.sh"
. "$RIG/steps.sh"
. "$RIG/crashlib.sh"
. "$RIG/guard.sh"

ALL="preflight cleanup movement combat terminals grenades workshop pa_vm pa_host churn reconnect fuzz final"
only=""; skip=""; churn_min=${SMOKE_CHURN_MIN:-3}
while [ $# -gt 0 ]; do case "$1" in --only) only=${2//,/ }; shift 2;; --skip) skip=${2//,/ }; shift 2;; --churn) churn_min=$2; shift 2;; *) echo "unknown option $1"; exit 2;; esac; done
steps=${only:-$ALL}
rig_guard || exit 2
a_init smoke; A_NOTE=""
trap a_interrupted INT TERM
tunnel
run() { case " $skip " in *" $1 "*) return;; esac; case " $steps " in *" $1 "*) ;; *) return;; esac; "${@:2}"; case "$1" in movement|combat|terminals|grenades|workshop|pa_vm|pa_host|churn) perf_phase "$1";; esac; [ "$1" = combat ] && ws_mark; [ "$1" = churn ] && ws_check; return 0; }

run preflight step_preflight
run cleanup   smoke_cleanup_start
run movement  step_movement
run combat    step_combat
run terminals step_terminals
run grenades  step_grenades
run workshop  step_workshop
run pa_vm     step_pa vm
run pa_host   step_pa host
run churn     step_churn "$churn_min"
run reconnect step_reconnect
run fuzz      step_fuzz 100
smoke_cleanup_end() {
	section "cleanup (test pieces left in Sanctuary)"
	local r left="" st
	bash "$RIG/cleanup.sh" run "$A_OUT/ws-placed.txt" > "$A_OUT/cleanup-end.log" 2>&1; tail -3 "$A_OUT/cleanup-end.log"
	# What this run placed must be gone. Pieces the session file already held ("N built" in the server's log line) come back as friends'
	# copies at every join and cannot be taken out of that file from here: counted and reported, not failed.
	for r in $(cat "$A_OUT/ws-placed.txt" 2>/dev/null); do st=$(host "refinfo $r"); case "$st" in *"deleted=true"*) ;; *) left="$left $r";; esac; done
	if [ -z "$left" ]; then pass "the pieces this run placed are removed" "$(wc -w < "$A_OUT/ws-placed.txt" 2>/dev/null) piece(s)"; else fail "the pieces this run placed are removed" "still there:$left"; fi
	if grep -q "still has test pieces" "$A_OUT/cleanup-end.log"; then warn "replayed copies of older test pieces remain" "$(grep 'still has' "$A_OUT/cleanup-end.log" | tr '\n' ' ' | cut -c1-150): the session file's built list brings them back at each join; start from a session file with no built pieces (backup.sh) for a clean save"; fi
}
case " $skip " in *" cleanup "*) ;; *) case " $steps " in *" cleanup "*) smoke_cleanup_end;; esac;; esac
run final     step_final
a_summary
