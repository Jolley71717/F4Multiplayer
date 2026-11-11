#!/bin/bash
# guard.sh: refuse to drive the games unless the caller holds the rig (AGENTS.md: one user at a time, rig.lock).
#   rig_guard   returns 0 when $STAGE/rig.lock exists and its owner is $F4MP_RIG_OWNER; otherwise says why and returns 1.
#               F4MP_SKIP_LOCK_CHECK=1 skips it (for a person at the keyboard who knows).
# The owner is never read from the lock itself: anyone could read it, so it would let a caller that never took the rig drive
# the games. A caller that forgot to export it is told the exact line to run.
rig_guard() {
	[ "$F4MP_SKIP_LOCK_CHECK" = 1 ] && return 0
	local owner; owner=$(head -1 "$STAGE/rig.lock/owner" 2>/dev/null | cut -d' ' -f1)
	if [ -z "$owner" ]; then echo "refusing to run: nobody holds the rig. Take it: bash $STAGE/rigq.sh take <name>; then run with F4MP_RIG_OWNER=<name> (exported, or on the same line)"; return 1; fi
	if [ -z "$F4MP_RIG_OWNER" ]; then echo "refusing to run: F4MP_RIG_OWNER is not set, and the rig is held by '$owner'. If you took it, run again as: F4MP_RIG_OWNER=$owner bash $0 ...; if not, wait for it: bash $STAGE/rigq.sh take <name>"; return 1; fi
	if [ "$owner" != "$F4MP_RIG_OWNER" ]; then echo "refusing to run: the rig is held by '$owner', not by F4MP_RIG_OWNER='$F4MP_RIG_OWNER'"; return 1; fi
	return 0
}
