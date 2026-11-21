#!/bin/bash
# shelter-diner-unseen.sh [who]: a player's report on 0.20.1: "Port-A-Diner and Pulowski preservation shelters aren't synced."
# <who> (vm, the default, or host) is the player who uses them; the other player watches from a few steps away.
#
# The shelter (a door with PulowskShelterScript on it: it opens on a press, shuts itself once its game's player is inside, and
# again once they have left): <who> walks up and presses E (real), walks in (real), presses E inside and walks out. At each of
# the four steps both games' open state of that door is read (dev `doors`: 1 open, 2 opening, 3 closed, 4 closing). The bug:
# the watcher's door never moves, so the friend's stand-in is seen walking through a shut door and standing in a shut booth.
#
# The Port-A-Diner (an activator with PortaDinerScript: a press rolls dice against the Luck of its own game's player; a win
# opens the hatch and puts a pie in its container, a loss plays the arm missing it). A press is replayed in the other game by
# the presser's stand-in already, but each game rolls for itself. So the dice are loaded, by console, said where it happens:
#   press 1: <who>'s Luck 0, the watcher's far above the script's RandomMax: <who> loses for sure, the watcher's game would win;
#   press 2: the other way round: <who> wins for sure, the watcher's game would lose.
# After each press, once both scripts are back in 'Waiting', both games' PieSuccess, Attempts and the container's contents are
# compared. (A diner that has won plays the win on every later press: on a build with the bug press 2 then agrees by accident;
# press 1 is the one that tells.) Both players' Luck is put back at the end (console).
#
# Set-up, said where it happens: both players are put by console setpos near the shelters south of the Mass Pike interchange
# (cell POIJS024: `rig.sh join POIJS024`); every move after that is a real walk (the pad's stick) or a real key.
# PART=shelter|diner|both (default both). SHELTER, DINER, DINERBOX: other references of the same kinds.
# REPRO: YES when the watcher's door did not follow <who>'s at a step, or the two games' diners disagree after a press;
# NO when every step read alike in both games; VOID when the scene was not reached.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
A=${1:-vm}; B=$(other_game "$A"); PART=${PART:-both}
SHELTER=${SHELTER:-001F9AC8}; DINER=${DINER:-001FDE48}; DINERBOX=${DINERBOX:-001FDE49}; LUCK_AV=000002C8
[ "$A" = host ] || [ "$A" = vm ] || verdict VOID "usage: shelter-diner-unseen.sh [host|vm]"
need_session
BAD=""; GOOD=0
differs() { BAD="$BAD${BAD:+; }$1"; say "  DIFFERS: $1"; }
same() { GOOD=$((GOOD + 1)); say "  the same in both: $1"; }
PUPPET=$(dc "$B" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
standin_at() { local p; p=$(dc "$B" "refinfo $PUPPET"); echo "$(field "$p" x) $(field "$p" y)"; }

# ---- the shelter ----------------------------------------------------------------------------------------------------------------
open_of() { dc "$1" "doors 1200" | tr ';' '\n' | grep -oE "$SHELTER '[^']*' open=[0-9]" | grep -oE '[0-9]$'; }
is_open() { case "$(open_of "$1")" in 1 | 2) return 0 ;; esac; return 1; }
is_shut() { case "$(open_of "$1")" in 3 | 4) return 0 ;; esac; return 1; }
# step <what> <open|shut>: <who>'s door is that way (waited for up to 8 s; 1 when it is not), and the watcher's within 8 s more
step() {
	local what=$1 want=$2
	if ! wait_for 8 "is_$want" "$A"; then say "  $what: $A's own door is not $want (open=$(open_of "$A"))"; return 1; fi
	if wait_for 8 "is_$want" "$B"; then same "$what: the door is $want in both games ($A open=$(open_of "$A"), $B open=$(open_of "$B"))"
	else differs "$what: the door is $want in $A's game (open=$(open_of "$A")) and not in $B's (open=$(open_of "$B"))"; fi
	return 0
}
shelter_part() {
	local s sx sy sz rz k px py d inside=0
	s=$(dc "$A" "refinfo $SHELTER"); sx=$(field "$s" x); sy=$(field "$s" y); sz=$(field "$s" z); rz=$(field "$s" rz)
	[ -n "$sx" ] && [ "$(field "$s" 3d)" = true ] || verdict VOID "$A's game has no loaded shelter $SHELTER ($s): rig.sh join POIJS024 first"
	say "== the shelter $SHELTER at ${sx%.*} ${sy%.*} ${sz%.*}, turned ${rz%.*} degrees"
	# The four sides of the booth, 230 out: the door's side is the one a walk to the booth's middle gets in from.
	side() { awk -v x="$sx" -v y="$sy" -v r="$rz" -v k="$1" -v d="${2:-230}" 'BEGIN { a = (r + k) * 3.14159265 / 180; printf "%d %d", x + d * sin(a), y + d * cos(a) }'; }
	read -r px py <<<"$(side 0)"
	say "set-up: console setpos: $A to $px $py (230 from the shelter), $B to $(side 90 420) (420 from it, the watcher)"
	place "$A" "$px" "$py" $((${sz%.*} + 40)) 0; read -r px py <<<"$(side 90 420)"; place "$B" "$px" "$py" $((${sz%.*} + 40)) 0; sleep 6
	[ -n "$(open_of "$A")" ] && [ -n "$(open_of "$B")" ] || verdict VOID "the shelter's open state is not read in both games ($A '$(open_of "$A")', $B '$(open_of "$B")'): both players in its cell?"
	say "before: the door reads open=$(open_of "$A") in $A's game, open=$(open_of "$B") in $B's (3 = closed)"
	is_shut "$A" && is_shut "$B" || verdict VOID "the shelter is not shut in both games to begin with: a fresh session (rig.sh join POIJS024)"
	rmark "$A" shelter; rmark "$B" shelter
	say "-- step 1: $A presses E on the shelter (real)"
	rpress_e "$A" "$SHELTER"; say "  rollover: '$ROLLOVER'"
	if ! is_open "$A" && ! wait_for 5 is_open "$A"; then rpress_e "$A" "$SHELTER"; say "  pressed again; rollover: '$ROLLOVER'"; fi
	step "after the press" open || verdict VOID "$A's press did not open the shelter in its own game (rollover '$ROLLOVER')"
	say "-- step 2: $A walks into the shelter (real walk, from each side in turn until one lets them in)"
	for k in 0 90 180 270; do
		read -r px py <<<"$(side $k)"; rwalk "$A" "$px" "$py" 45 0.7
		rwalk "$A" "${sx%.*}" "${sy%.*}" 30 0.6; d=$(dist_xy "$(pos_of "$A")" "${sx%.*} ${sy%.*}")
		say "  from the side at $k degrees: $A is ${d:-?} from the shelter's middle ($WALKED)"
		if [ "${d:-999}" -le 55 ]; then inside=1; break; fi
	done
	if [ $inside = 1 ]; then
		step "$A inside" shut
		d=$(dist_xy "$(standin_at)" "${sx%.*} ${sy%.*}"); say "  in $B's game $A's stand-in is ${d:-?} from the shelter's middle"
		say "-- step 3: $A presses E inside (real), 3 s after the door shut (its script blocks the door for 2 s)"
		sleep 3; rpress_e "$A" "$SHELTER"; say "  rollover: '$ROLLOVER'"
		if ! wait_for 5 is_open "$A"; then dc "$A" "mouse 600 0" >/dev/null; sleep 0.6; rkey "$A" "$VK_E" 100; say "  turned and pressed again; rollover: '$(rrollover "$A")'"; fi
		if step "after the press inside" open; then
			say "-- step 4: $A walks out (real walk)"
			read -r px py <<<"$(side $k 260)"; rwalk "$A" "$px" "$py" 50 0.7; say "  $WALKED"
			step "$A outside again" shut
		fi
	else
		say "  $A did not get into the shelter from any side: steps 2 to 4 are not judged"
	fi
	say "what the two games' logs say of the door:"
	for w in "$A" "$B"; do rsince "$w" shelter | grep -E "WorldSync: $SHELTER|$SHELTER" | tail -4 | cut -c1-260 | sed "s/^/  $w: /" | tee -a "$REPRO_OUT/run.log"; done
}

# ---- the diner ------------------------------------------------------------------------------------------------------------------
dvars() { dc "$1" "scriptvars $DINER PortaDinerScript PieSuccess Attempts RandomMax fSuccessMod ForcePieSuccess"; }
dstate() { dvars "$1" | grep -oE "state='[^']*'" | sed "s/state=//; s/'//g"; }
waiting() { [ "$(dstate "$1")" = Waiting ]; }
box() { dc "$1" "contents $DINERBOX" | tr ';' '\n' | sed 's/^ *//' | sort | paste -sd';' -; }
luck_of() { dc "$1" "av 00000014 $LUCK_AV" | grep -oE '^-?[0-9]+'; }
set_luck() { say "set-up: console player.setav luck $2 in $1's game"; dc "$1" "console player.setav luck $2" >/dev/null; }
# press <n> <what must come of it>: <who> presses E on the diner (real); both scripts back in Waiting; the two games compared
press() {
	local n=$1 va vb
	rmark "$A" "diner$n"; rmark "$B" "diner$n"
	say "-- press $n ($2): $A presses E on the diner (real)"
	rpress_e "$A" "$DINER"; say "  rollover: '$ROLLOVER'"; sleep 1.5
	wait_for 25 waiting "$A" || say "  $A's diner is still '$(dstate "$A")' 25 s after the press"
	wait_for 15 waiting "$B" || say "  $B's diner is still '$(dstate "$B")'"
	sleep 1; va=$(dvars "$A"); vb=$(dvars "$B")
	say "  $A: $va | container: $(box "$A")"
	say "  $B: $vb | container: $(box "$B")"
	if [ "$(field "$va" PieSuccess)" = "$(field "$vb" PieSuccess)" ] && [ "$(box "$A")" = "$(box "$B")" ] && [ "$(field "$va" Attempts)" = "$(field "$vb" Attempts)" ]; then
		same "press $n: PieSuccess=$(field "$va" PieSuccess), Attempts=$(field "$va" Attempts), container '$(box "$A")'"
	else
		differs "press $n: $A's diner has PieSuccess=$(field "$va" PieSuccess) Attempts=$(field "$va" Attempts) container '$(box "$A")', $B's PieSuccess=$(field "$vb" PieSuccess) Attempts=$(field "$vb" Attempts) container '$(box "$B")'"
	fi
	say "  $B's count of friends' presses: $(dc "$B" 'net status' | grep -oE 'activations[A-Za-z]*=[0-9]+' | paste -sd' ' -)"
}
diner_part() {
	local s dx dy dz va vb max hi la lb
	s=$(dc "$A" "refinfo $DINER"); dx=$(field "$s" x); dy=$(field "$s" y); dz=$(field "$s" z)
	[ -n "$dx" ] && [ "$(field "$s" 3d)" = true ] || verdict VOID "$A's game has no loaded diner $DINER ($s): rig.sh join POIJS024 first"
	say "== the Port-A-Diner $DINER at ${dx%.*} ${dy%.*} ${dz%.*} (its container $DINERBOX)"
	say "set-up: console setpos: $A to $((${dx%.*} + 260)) ${dy%.*}, $B to $((${dx%.*} + 330)) $((${dy%.*} + 200))"
	place "$A" $((${dx%.*} + 260)) "${dy%.*}" $((${dz%.*} + 40)) 270; place "$B" $((${dx%.*} + 330)) $((${dy%.*} + 200)) $((${dz%.*} + 40)) 270; sleep 6
	va=$(dvars "$A"); vb=$(dvars "$B"); say "before: $A: $va | $(box "$A")"; say "before: $B: $vb | $(box "$B")"
	{ [ "$(field "$va" PieSuccess)" = false ] && [ "$(field "$vb" PieSuccess)" = false ]; } ||
		verdict VOID "the diner has given its pie already in one of the games ($A PieSuccess=$(field "$va" PieSuccess), $B PieSuccess=$(field "$vb" PieSuccess)): a fresh session"
	max=$(field "$va" RandomMax); max=${max%.*}; is_int "$max" || verdict VOID "the diner's RandomMax is not read ($va)"
	hi=$((max * 4 + 20)); la=$(luck_of "$A"); lb=$(luck_of "$B")
	say "the dice: RandomMax=$max, fSuccessMod=$(field "$va" fSuccessMod); Luck now: $A ${la:-?}, $B ${lb:-?}"
	rwalk "$A" "$DINER" 110 0.6; say "$A walked to the diner (real walk): $WALKED"
	set_luck "$A" 0; set_luck "$B" "$hi"; sleep 1
	press 1 "$A cannot win, $B's own dice would"
	set_luck "$A" "$hi"; set_luck "$B" 0; sleep 1
	press 2 "$A wins for sure, $B's own dice would lose"
	is_int "$la" && set_luck "$A" "$la"; is_int "$lb" && set_luck "$B" "$lb"
	say "what the two games' logs say of the diner:"
	for w in "$A" "$B"; do rsince "$w" diner1 | grep -E "$DINER|Diner|diner" | tail -4 | cut -c1-260 | sed "s/^/  $w: /" | tee -a "$REPRO_OUT/run.log"; done
}

[ -n "$PUPPET" ] || verdict VOID "$B's game has no stand-in for $A's player (no puppet= in its net status)"
say "$A uses them, $B watches; $A's stand-in in $B's game: $PUPPET; build: $A $(dc "$A" status | grep -oE 'version=[^ ]*') | $B $(dc "$B" status | grep -oE 'version=[^ ]*')"
case "$PART" in shelter) shelter_part ;; diner) diner_part ;; *) shelter_part; diner_part ;; esac
for w in "$A" "$B"; do rkeep "$w" end; done
[ -n "$BAD" ] && verdict YES "$BAD"
[ "$GOOD" -gt 0 ] && verdict NO "every step read alike in both games ($GOOD steps)"
verdict VOID "nothing was judged"
