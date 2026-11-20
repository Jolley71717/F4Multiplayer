#!/bin/bash
# standin-gun-parts.sh [owner] [guns...]: a player's gun shown taken apart on their stand-in in the friend's game (its receiver,
# barrel, magazine and the rest floating apart around the hand; a player's report, the host's 10mm on a friend's screen). The
# owner (host, the default, or vm) holds each gun in turn; the other game (the watcher) shows their stand-in holding it.
# For each gun: every copy of it taken out, then copies added (console additem, which rolls the gun's object template: receivers,
# scopes, barrels, muzzles), up to ROLLS, and the copy with the most mods held first (dev `equipstack`), then one with other mods
# (the stand-in's copy taken away and given again). Each is looked at holstered and drawn (dev `draw`), and drawn again after a
# holster.
# The look: dev `gunparts` in both games, the owner's own player (third person) and the watcher's stand-in: the gun's shapes under
# the skeleton's Weapon node, each where it sits in that node's own frame (its local transforms composed down from there), so a gun
# put together the same way sits the same in both games whatever the actor's place, heading or size. A shape of the stand-in's gun
# more than TOL units from where the owner's sits is a part taken apart. Shapes one game has and the other has not are said (a mod
# this game lacks, not this bug). A picture of the watcher's screen, the watcher turned to the stand-in (dev turnto), is kept
# for each drawn look (shot-<n>.png), for the reader's eyes: the verdict is the 3D's.
# Console set-up, said so: the guns added (additem) and their stacks equipped (equipstack); the gun drawn and holstered by the dev
# channel's `draw`. Nothing moves either player.
# REPRO: YES when a look finds a part of the stand-in's gun displaced, NO when every look finds it whole, VOID when the scene
# (a stand-in holding the owner's copy) could not be set up.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
OWNER=${1:-host}; shift
[ "$OWNER" = host ] || [ "$OWNER" = vm ] || { echo "usage: standin-gun-parts.sh [host|vm] [guns...]"; exit 2; }
WATCHER=$(other_game "$OWNER")
# 10mm pistol, hunting rifle, combat rifle, laser gun, pipe gun
GUNS=("$@"); [ ${#GUNS[@]} -gt 0 ] || GUNS=(00004822 0004F46A 000DF42E 0009983B 00024F55)
ROLLS=${ROLLS:-8}; TOL=${TOL:-3}; SHOTS=${SHOTS:-1}
need_session
PUPPET=$(dc "$WATCHER" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
[ -n "$PUPPET" ] || verdict VOID "the $WATCHER's game has no stand-in for the $OWNER's player (no puppet= in its net status)"
# a build without dev gunparts (0.20.3 itself): the run goes on for the pictures alone, and ends VOID (judged by eye)
EYES=""; case "$(dc "$OWNER" 'gunparts 00000014')" in error*unknown*|"") EYES=1; say "the $OWNER's game has no dev gunparts: pictures only";; esac

# shot <name>: a picture of the watcher's screen, kept as $REPRO_OUT/<name>.png
shot() {
	[ "$SHOTS" = 1 ] || return 0
	local f="$REPRO_OUT/$1.png"
	if [ "$WATCHER" = host ]; then
		powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; \$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds; \$m=New-Object System.Drawing.Bitmap \$b.Width,\$b.Height; \$g=[System.Drawing.Graphics]::FromImage(\$m); \$g.CopyFromScreen(\$b.Location,[System.Drawing.Point]::Empty,\$b.Size); \$m.Save('$(cygpath -w "$f")')" >/dev/null 2>&1
	else
		vmssh "& C:\\F4MP\\in-vm-run.ps1 -Name \"F4MP-Shot-\$(Get-Date -Format HHmmssfff)\" -Exe C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -Arguments '-WindowStyle Hidden -NoProfile -File C:\\F4MP\\vmshot.ps1 -Out C:\\F4MP\\gunparts-shot.png'" >/dev/null
		sleep 3
		scp -q -i "$VM_KEY" -o LogLevel=ERROR "$VM_SSH:C:/F4MP/gunparts-shot.png" "$f" 2>/dev/null
	fi
	[ -s "$f" ] && say "  picture: $(basename "$f")"
}
# parts <who> <actor>: that game's gunparts answer for the actor
parts() { dc "$1" "gunparts $2"; }
# compare <owner's answer> <watcher's answer>: "<worst displacement> <shapes compared> <only owner> <only watcher> <worst parts>"
compare() {
	awk -v a="$1" -v b="$2" '
	function load(s, side,   p, n, i, e, name, xyz, k) {
		sub(/.*parts=/, "", s); n = split(s, p, ";")
		for (i = 1; i <= n; i++) {
			e = p[i]; if (e == "" || substr(e, 1, 1) == "+") continue
			name = e; sub(/@\(.*/, "", name); sub(/\(culled\)$/, "", name)
			xyz = e; sub(/.*@\(/, "", xyz); sub(/\)$/, "", xyz)
			k = side SUBSEP name; cnt[k]++; pos[k, cnt[k]] = xyz; names[name] = 1
		}
	}
	BEGIN {
		load(a, "o"); load(b, "w"); worst = 0; same = 0; onlyO = ""; onlyW = ""; list = ""
		for (name in names) {
			co = cnt["o" SUBSEP name] + 0; cw = cnt["w" SUBSEP name] + 0
			if (co == 0) { onlyW = onlyW name "x" cw ","; continue }
			if (cw == 0) { onlyO = onlyO name "x" co ","; continue }
			m = co < cw ? co : cw
			if (co != cw) { onlyO = onlyO name "x" co "/" cw ","; }
			# each of the owner'"'"'s copies of the name paired with the nearest of the watcher'"'"'s
			for (i = 1; i <= m; i++) {
				split(pos["o" SUBSEP name, i], o, ","); best = -1
				for (j = 1; j <= cw; j++) { split(pos["w" SUBSEP name, j], w, ","); d = sqrt((o[1]-w[1])^2 + (o[2]-w[2])^2 + (o[3]-w[3])^2); if (best < 0 || d < best) best = d }
				same++
				if (best > worst) worst = best
				if (best > '"$TOL"') list = list name "(" pos["o" SUBSEP name, i] "->" sprintf("%.0f", best) "),"
			}
		}
		printf "%.0f %d %s %s %s\n", worst, same, (onlyO == "" ? "-" : onlyO), (onlyW == "" ? "-" : onlyW), (list == "" ? "-" : list)
	}'
}
APART=0; LOOKS=0; WHOLE=0; N=0
# look <label>: both games' gunparts compared; APART counted
look() {
	local label=$1 o w c worst same onlyO onlyW list
	N=$((N + 1))
	[ -z "$EYES" ] || { say "  look $N ($label): pictures only"; return; }
	o=$(parts "$OWNER" 00000014); w=$(parts "$WATCHER" "$PUPPET")
	echo "$label | owner: $o" >> "$REPRO_OUT/parts.log"; echo "$label | watcher: $w" >> "$REPRO_OUT/parts.log"
	if ! grep -q 'parts=' <<<"$o" || ! grep -q 'parts=' <<<"$w"; then say "  look $N ($label): no gun 3D to compare: owner '$(cut -c1-60 <<<"$o")' watcher '$(cut -c1-60 <<<"$w")'"; return; fi
	read -r worst same onlyO onlyW list <<<"$(compare "$o" "$w")"
	LOOKS=$((LOOKS + 1))
	say "  look $N ($label): owner $(grep -oE 'shapes=[0-9]+ spread=[0-9]+' <<<"$o"), stand-in $(grep -oE 'shapes=[0-9]+ spread=[0-9]+' <<<"$w"); $same shapes compared, worst $worst u; only the owner's: $onlyO; only the stand-in's: $onlyW"
	if [ "$same" -gt 0 ] && [ "$worst" -gt "$TOL" ]; then APART=$((APART + 1)); say "  look $N: APART: displaced: $list"; else WHOLE=$((WHOLE + 1)); fi
}
# the watcher's stand-in holds the owner's gun built with these mods
holds() { _gs_holds "$WATCHER" "$PUPPET" "$1" "$2"; }
_gs_mods() { grep -oE "mods\[$2\]=[0-9A-F,]*" <<<"$1" | head -1 | cut -d= -f2 | tr ',' '\n' | grep . | sort | paste -sd, -; }
_gs_holds() { local e; e=$(dc "$1" "equipped $2"); grep -q " $3 '" <<<"$e" && [ "$(_gs_mods "$e" "$3")" = "$4" ]; }
# stacks <gun>: the owner's stacks of it, one a line: "<n> <mod count> <sorted mods>"
stacks() {
	dc "$OWNER" "itemmods $1" | tr ';' '\n' | grep '^ stack' | awk '{ m = $0; if (match(m, /mods=[0-9A-F,]*/)) { m = substr(m, RSTART + 5, RLENGTH - 5) } else m = ""; print NR, m }' |
		while read -r n m; do s=$(tr ',' '\n' <<<"$m" | grep . | sort | paste -sd, -); echo "$n $(tr ',' '\n' <<<"$s" | grep -c .) ${s:-none}"; done
}
strip() { local i; for i in 1 2 3 4 5; do [ "$(dc "$OWNER" "count 00000014 $1")" = 0 ] && return 0; dc "$OWNER" "console player.removeitem $1 99" >/dev/null; sleep 1; done; }
held_mods() { _gs_mods "$(dc "$OWNER" equipped)" "$1"; }

say "standin-gun-parts: the $OWNER's guns on its stand-in $PUPPET in the $WATCHER's game; guns ${GUNS[*]}; up to $ROLLS rolls each; tolerance $TOL u"
say "set-up: the $WATCHER's player turned to the stand-in (dev turnto)"
for gun in "${GUNS[@]}"; do
	say "gun $gun: every copy taken out, up to $ROLLS added (console additem)"
	strip "$gun"
	for i in $(seq 1 "$ROLLS"); do dc "$OWNER" "console player.additem $gun 1" >/dev/null; sleep 0.4; done
	st=$(stacks "$gun"); [ -n "$st" ] || { say "gun $gun: no stack after additem (no such gun in this game?): skipped"; continue; }
	say "gun $gun: stacks: $(paste -sd'|' - <<<"$st")"
	first=$(sort -k2,2nr <<<"$st" | head -1); a=$(cut -d' ' -f1 <<<"$first"); am=$(cut -d' ' -f3 <<<"$first")
	other=$(awk -v m="$am" '$3 != m' <<<"$st" | sort -k2,2nr | head -1); b=$(cut -d' ' -f1 <<<"$other"); bm=$(cut -d' ' -f3 <<<"$other")
	# holstered: the copy with the most mods equipped with the gun away
	dc "$OWNER" "draw off" >/dev/null; sleep 1.5
	r=$(dc "$OWNER" "equipstack $gun $a"); sleep 1; dc "$OWNER" "draw off" >/dev/null
	m=$(held_mods "$gun"); [ -n "$m" ] || m=$am
	if ! wait_for 12 holds "$gun" "$m"; then say "gun $gun: the stand-in does not hold the owner's copy ($m) within 12 s: $(dc "$WATCHER" "equipped $PUPPET" | cut -c1-200)"; continue; fi
	sleep 2; look "$gun stack $a ($m) holstered"
	dc "$WATCHER" "turnto $PUPPET 300 aim" >/dev/null
	dc "$OWNER" "draw on" >/dev/null; sleep 4; look "$gun stack $a drawn"; shot "shot-$N"
	if [ -n "$b" ]; then
		r=$(dc "$OWNER" "equipstack $gun $b"); dc "$OWNER" "draw on" >/dev/null; m=$(held_mods "$gun"); [ -n "$m" ] || m=$bm
		if wait_for 12 holds "$gun" "$m"; then sleep 4; look "$gun swapped to stack $b ($m) drawn"; shot "shot-$N"
		else say "gun $gun: the stand-in does not hold the other copy ($m) within 12 s: $(dc "$WATCHER" "equipped $PUPPET" | cut -c1-200)"; fi
	else say "gun $gun: every copy carries the same mods: no swap"; fi
	dc "$OWNER" "draw off" >/dev/null; sleep 3; dc "$OWNER" "draw on" >/dev/null; sleep 4; look "$gun holstered and drawn again"; shot "shot-$N"
	dc "$OWNER" "draw off" >/dev/null; sleep 1
	strip "$gun"
done
rkeep "$OWNER" standin-gun-parts; rkeep "$WATCHER" standin-gun-parts
say "looks: $N, compared: $LOOKS, whole: $WHOLE, apart: $APART (parts.log has both games' answers)"
[ -z "$EYES" ] || verdict VOID "no dev gunparts in this build: the pictures (shot-*.png) are the evidence, judged by eye"
[ "$LOOKS" -gt 0 ] || verdict VOID "no look found both guns' 3D to compare"
[ "$APART" -gt 0 ] && verdict YES "the stand-in's gun had parts displaced from where the owner's sit in $APART of $LOOKS looks"
verdict NO "the stand-in's gun sat as the owner's in all $LOOKS looks (within $TOL u)"
