#!/bin/bash
# putaway-uncovered.sh: a friend's own pieces at a settlement the host owns are put away for the session (0.22.0), and the
# ones the host's settlement has nothing at the same spot for vanished with nothing to show for it (player report: back at
# Sanctuary every bed, generator and purifier built earlier was invisible to them alone, yet solid, working and counted).
# Only a piece the host's version covers (a copy of a host piece of the same base at the same workbench within 4 units) is hidden.
#   1. Both games at Sanctuary, joined. The VM (the friend) makes four pieces of its own (console placement, linked to the
#      workbench as a build is): a generator G1, a light L1, a bed B1 and a second generator G2.
#   2. The host's Sanctuary is made the host's (console WorkshopScript.SetOwnedByPlayer). The host then places (console placement,
#      wsfinish) a generator exactly on G1's spot, a bed exactly on B1's spot, and a generator 40 units from G2: copies the VM's
#      game makes. L1 has no host piece at all.
#   3. Read each piece's culled flag in the VM's game (dev wsputaway <ref>).
# REPRO: YES when a piece the host's version does not cover (L1, G2) is culled; NO when only G1 and B1 are, and the rest are drawn.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
BENCH=000250FE; GEN=00030386; LIGHT=000560DF; BED=00118F57
need_session
for w in host vm; do
	loc=$(dc $w status | grep -oE "location=[0-9A-F]+ '[^']*'")
	case "$loc" in *"Sanctuary"*) ;; *) verdict VOID "$w is not at Sanctuary ($loc): console coc SanctuaryExt";; esac
done
owned() { field "$(dc host "scriptvars $BENCH WorkshopScript OwnedByPlayer")" OwnedByPlayer; }
PLACED_HOST=""; PLACED_VM=""
cleanup() { for r in $PLACED_VM; do dc vm "wsscrap $r" >/dev/null; done; for r in $PLACED_HOST; do dc host "wsscrap $r" >/dev/null; done; dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 0" >/dev/null; }
on_end cleanup
dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 0" >/dev/null; sleep 3
pos=$(dc vm pos); PX=$(field "$pos" x); PY=$(field "$pos" y); PZ=$(field "$pos" z)
{ [ -n "$PX" ] && [ -n "$PY" ] && [ -n "$PZ" ]; } || verdict VOID "the VM's position is unreadable ($pos)"
mine() {  # mine <base> <dx> <dy>: a piece of the VM's own at that offset, linked to the workbench; prints its ref
	local out ref; out=$(dc vm "wsreport place $1 $BENCH quiet"); ref=$(grep -oE 'placed [0-9A-F]{8}' <<<"$out" | grep -oE '[0-9A-F]{8}')
	[ -n "$ref" ] || return 1
	dc vm "wslink $ref $BENCH" >/dev/null
	dc vm "setpos $ref $(awk -v a="$PX" -v b="$PY" -v c="$PZ" -v dx="$2" -v dy="$3" 'BEGIN{printf "%.1f %.1f %.1f", a+dx, b+dy, c}')" >/dev/null
	PLACED_VM="$PLACED_VM $ref"; echo "$ref"
}
theirs() {  # theirs <base> <ref of the VM's piece> <dx> <dy>: a host piece at that piece's spot (plus the offset), reported and finished
	local ri x y z out ref; ri=$(dc vm "refinfo $2"); x=$(field "$ri" x); y=$(field "$ri" y); z=$(field "$ri" z)
	out=$(dc host "wsreport place $1 $BENCH quiet"); ref=$(grep -o "[0-9A-F]\{8\}" <<<"$out" | head -1); [ -n "$ref" ] || return 1
	PLACED_HOST="$PLACED_HOST $ref"
	dc host "setpos $ref $(awk -v a="$x" -v b="$y" -v c="$z" -v dx="$3" -v dy="$4" 'BEGIN{printf "%.1f %.1f %.1f", a+dx, b+dy, c}')" >/dev/null
	dc host "wsreport $ref $BENCH" >/dev/null; sleep 2; dc host "wsfinish $ref $BENCH" >/dev/null; sleep 2; echo "$ref"
}
G1=$(mine $GEN 200 0); L1=$(mine $LIGHT 200 200); B1=$(mine $BED 0 200); G2=$(mine $GEN -200 0)
for r in "$G1" "$L1" "$B1" "$G2"; do [ -n "$r" ] || verdict VOID "the VM's pieces were not made ($G1 $L1 $B1 $G2)"; done
say "set-up (console placement): the VM's own generator $G1, light $L1, bed $B1, generator $G2 at Sanctuary"
rmark vm start
dc host "console $BENCH.cf \"WorkshopScript.SetOwnedByPlayer\" 1" >/dev/null; sleep 3
[ "$(owned)" = true ] || verdict VOID "Sanctuary is not the host's after SetOwnedByPlayer"
dc vm "wsmode host" >/dev/null
if ! _ws_in host; then ws_enter_real host $BENCH || verdict VOID "the host's real E opened no workshop mode ($ENTER_SAID)"; fi
ws_leave host
H1=$(theirs $GEN $G1 0 0) || verdict VOID "the host's generator was not placed"
H3=$(theirs $BED $B1 0 0) || verdict VOID "the host's bed was not placed"
H2=$(theirs $GEN $G2 40 0) || verdict VOID "the host's near-miss generator was not placed"
say "the host placed generator $H1 on $G1's spot, bed $H3 on $B1's spot, generator $H2 40 units from $G2's"
sleep 10
for p in "H1 $H1" "H3 $H3" "H2 $H2"; do c=$(ws_copy_of vm "${p#* }"); say "the VM's copy of ${p% *}: ${c:-none}"; done
state() { field "$(dc vm "wsputaway $1")" culled; }
say "VM $(dc vm wsputaway | cut -c1-160)"
cG1=$(state $G1); cL1=$(state $L1); cB1=$(state $B1); cG2=$(state $G2)
say "culled in the VM: generator $G1 (host's on it) $cG1, bed $B1 (host's on it) $cB1, light $L1 (none of the host's) $cL1, generator $G2 (host's 40 u off) $cG2"
say "HUD at the VM: '$(rhud vm)'"
rkeep vm putaway-uncovered
if [ "$cL1" = true ] || [ "$cG2" = true ]; then verdict YES "a piece the host's version does not cover is hidden (light $cL1, near-miss generator $cG2; covered: generator $cG1, bed $cB1)"; fi
if [ "$cG1" = true ] && [ "$cB1" = true ]; then verdict NO "only the covered pieces are hidden (generator $cG1, bed $cB1); light $cL1 and near-miss generator $cG2 are drawn"; fi
verdict VOID "nothing was put away at all (generator $cG1, bed $cB1, light $cL1, generator $cG2)"
