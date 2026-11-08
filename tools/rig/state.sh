#!/bin/bash
# state.sh: the state guard between a run's steps, and the power armor climb-out it shares with step_pa. Sourced by smoke.sh
# after assert.sh and steps.sh.
#
# Every step assumes both players start out of power armor, alive and not down, with nothing open but the HUD, a normal
# first- or third-person camera, god mode on and no virtual pad plugged. A step that leaves a player otherwise turned every
# step after it into failures of the mod that weren't (2026-11-07, the 0.14.0-rc1 soak: one climb-out that missed left the
# VM in power armor for a whole round, and the safe, the laser trails, a drop's take and the downed repeat all failed in
# the suit). rig_state_guard <step> looks at both games before a step and puts each wrong state right the way a player
# would (real keys and pad buttons; a friend's real walk to help a downed player up), recording each fix as HEAL. One it
# can't put right is a RIGFAULT (assert.sh): that step is not run in this round, its checks are not made, and the run can't
# end in PASS. Getting a player out of power armor by the console, when neither the keyboard nor the pad does,
# is the one heal that is not a player's action: it is said so in the HEAL line, and the climb-out check of step_pa that
# met it has already failed.

# st_open <who>: the menus open in that game other than the HUD's own (HUDMenu, CursorMenu, the power armor HUD, which goes
# with the suit), space-separated; "?" when `menus` gave no answer
st_open() {
	local m; m=$(dc "$1" menus | grep -oE 'open=[^ ]*')
	[ -n "$m" ] || { echo "?"; return; }
	tr ',' '\n' <<<"${m#open=}" | cut -d: -f1 | grep -vxE 'HUDMenu|CursorMenu|PowerArmorHUDMenu|none' | tr '\n' ' ' | sed 's/ $//'
}
# st_close_menus <who>: closes what a player would close (real keys: Enter for a message box, Tab for the Pip-Boy, a terminal,
# a container, a barter or a lockpick, Esc for the pause menu and anything else, ~ for the console; workshop mode is left by
# the dev channel's `workshop exit`, as the bench step leaves it) and waits out a loading screen or the level-up prompt.
# Prints what it closed; 1 when something is still open after 40 s.
st_close_menus() {
	local w=$1 t0=$SECONDS open did=""
	while :; do
		open=$(st_open "$w")
		[ -z "$open" ] && { echo "${did# }"; return 0; }
		[ $((SECONDS - t0)) -ge 40 ] && { echo "${did# }; still open after 40 s: $open"; return 1; }
		dc "$w" focus >/dev/null; sleep 0.3
		case " $open " in
		*" ? "*) ;;   # no answer: a load or a stall; asked again
		*" LoadingMenu "*|*" FaderMenu "*|*" PromptMenu "*|*" LevelUpMenu "*) ;;   # waited out
		*" MessageBoxMenu "*) dc "$w" "key 0x0D 60" >/dev/null; did="$did MessageBoxMenu" ;;
		*" WorkshopMenu "*) dc "$w" "workshop exit" >/dev/null; did="$did WorkshopMenu" ;;
		*" Console "*) dc "$w" "key 0xC0 60" >/dev/null; did="$did Console" ;;
		*" PipboyMenu "*|*" TerminalMenu "*|*" TerminalHolotapeMenu "*|*" ContainerMenu "*|*" BarterMenu "*|*" LockpickingMenu "*) dc "$w" "key 0x09 60" >/dev/null; did="$did ${open%% *}" ;;
		*) dc "$w" "key 0x1B 60" >/dev/null; did="$did ${open%% *}" ;;
		esac
		sleep 1.5
	done
}

# ---- power armor: out of the suit the way a player gets out --------------------------------------------------------
pa_in() { [ "$(dcf "$1" pa extraPA)" = true ]; }
pa_out() { [ "$(dcf "$1" pa extraPA)" = false ]; }
# pa_climb_out <who> [keyboard tries]: that player climbs out of power armor. In turn, each until the player is out:
#   1. the keyboard's E held 1.5 s, the camera looking down first (a crosshair on something it could use takes the press as a
#      use), the game in front with nothing open (rig_input_ready), KEYBOARD_TRIES times (default 3);
#   2. only when no E press was seen to reach the game's window (the rig's input, not the game, kept the player in): the
#      virtual pad's A held 2 s (a gamepad's own climb-out; it needs no window in front). The pad is plugged in with a
#      neutral stick 1.5 s before: a press that is the pad's first input is lost while the game takes it on as the controller
#      (2026-11-07, on the rig: A held at once left the player in, A after a neutral stick got them out).
# An E that reached the game and left the player in is the game's or the mod's, never the rig's: no other route is tried, so
# it can't be passed by another press (a regression that ate a keyboard player's way out would hide behind the pad's).
# The dev channel's `input Activate` (the button event after Windows and the control map) is no route: held 1.5 s or 3 s it
# never got a player out of the suit on the rig (2026-11-07), so it would only say "still in".
# PA_HOW says which got the player out (empty when none did); PA_REACHED says whether a keyboard press was seen to reach the
# game's window (its raw keyboard messages rose: `mousedev` keys=) on a try that left the player in; PA_DIAG has each try's
# reading (window in front, keys before and after, the input tally) and, when none worked, what the game said of its menus, the
# player's power armor, its input layers and where it stood. 0 = out.
pa_climb_out() {
	local w=$1 n=${2:-3} i k0 k1 r f rdy
	PA_HOW=""; PA_REACHED=""; PA_DIAG=""
	pa_out "$w" && { PA_HOW="already out"; return 0; }
	for i in $(seq 1 "$n"); do
		rig_input_ready "$w" 20; rdy=$?
		k0=$(keys_seen "$w"); dc "$w" "mouse 0 -900" >/dev/null; sleep 0.4
		r=$(dc "$w" "key 0x45 1500" | grep -oE 'front=[a-z]+'); sleep 6; k1=$(keys_seen "$w")
		f=$(dc "$w" focus | grep -oE 'input: .*' | cut -c1-90)
		PA_DIAG="$PA_DIAG E try $i: $r keys $k0 -> $k1${INPUT_READY:+ ($INPUT_READY)}; $f;"
		pa_out "$w" && { PA_HOW="E held, try $i"; return 0; }
		# reached: the game ready (nothing open, in front) when the key went down, and its raw key count rose
		[ "$rdy" = 0 ] && [ "$r" = front=yes ] && is_int "$k0" && is_int "$k1" && [ "$k1" -gt "$k0" ] && PA_REACHED=1
	done
	if [ -n "$PA_REACHED" ]; then
		PA_DIAG="$PA_DIAG the E reached the game's window and left the player in, so no other route was tried; menus $(dc "$w" menus | grep -oE 'open=[^ ]*' | cut -c1-90); pa: $(dc "$w" pa); layers: $(dc "$w" controls | grep -oE 'layers=[^|]*' | cut -c1-200)"
		return 1
	fi
	dc "$w" "pad stick l 0 0 300" >/dev/null; sleep 1.5; dc "$w" "pad button a 2000" >/dev/null; sleep 6; dc "$w" "pad unplug" >/dev/null
	pa_out "$w" && { PA_HOW="the pad's A held (the keyboard's E did not get the player out in $n tries)"; return 0; }
	PA_DIAG="$PA_DIAG pad A held: still in; menus $(dc "$w" menus | grep -oE 'open=[^ ]*' | cut -c1-90); pa: $(dc "$w" pa); layers: $(dc "$w" controls | grep -oE 'layers=[^|]*' | cut -c1-200); at $(dc "$w" pos | cut -c1-60)"
	return 1
}
# pa_force_out <who>: the console takes the player out of the suit (the frame's Papyrus SwitchToPowerArmor with none). Not a
# player's action: only to heal a rig whose player no press got out, and said so wherever it is used. 0 = out.
pa_force_out() {
	dc "$1" 'console player.cf "Actor.SwitchToPowerArmor" none' >/dev/null; sleep 6
	pa_out "$1"
}

# ---- down: a friend's real walk helps the player up ------------------------------------------------------------------
st_down() { [ "$(field "$(dc "$1" rules)" down)" = true ]; }
st_up() { [ "$(field "$(dc "$1" rules)" down)" = false ]; }
# st_help_up <who>: the other player walks to the down player's stand-in and stays there until they are up (the rules' help).
st_help_up() {
	local w=$1 h p; h=$(other_game "$w")
	p=$(dc "$h" 'net status' | grep -oE 'puppet=FF[0-9A-F]+' | head -1 | cut -d= -f2)
	[ -n "$p" ] || return 1
	bash "$RIG/walkto.sh" "$h" "$p" 0.8 100 60 >/dev/null 2>&1; dc "$h" "pad unplug" >/dev/null
	wait_for 20 st_up "$w"
}

# ---- the guard ----------------------------------------------------------------------------------------------------------
# rig_state_guard <step>: both games put in the state every step starts from (above). 0 = ready (each fix recorded HEAL);
# 1 = a state that stayed wrong (recorded RIGFAULT), or a player found dead (a FAIL): the caller does not run the step.
# A state only the mod (or the game) could have caused is never a quiet heal. God mode is on through every step but the
# downed one (this guard puts it on; only step_downed turns it off, and back on): a player found down after any other step
# (ST_PREV, set by smoke.sh's run()) was downed through god mode, a FAIL with what the rules said (damage sync has let that
# through before), then helped up; after the downed step its own checks cover it, and the help-up is a HEAL. A player found
# dead is a FAIL, never a camera to fix. A message box found open may be the mod's own notice: a WARN, then closed. An E that
# reached the game and left a player in power armor is a FAIL (pa_climb_out), then the console takes them out.
rig_state_guard() {
	local step=$1 w g c v r what="state before $1" bad="" dead=""
	local since="the run's start"; [ -n "$ST_PREV" ] && since="the $ST_PREV step"
	for w in host vm; do
		wait_ingame "$w" 30 || { rig_broken game "$what: $w in the game" "$(alive): $w not in the game after 60 s"; return 1; }
	done
	pads_off
	for w in host vm; do
		# menus
		c=$(st_open "$w")
		if [ -n "$c" ] && [ "$c" != "?" ]; then
			case " $c " in *" MessageBoxMenu "*) warn "$what: $w had a message box open" "after $since, at $(date +%T) (the plugin log around then says whose it was; the dev channel can't read its text): closed with Enter";; esac
			if r=$(st_close_menus "$w"); then healed "$what: $w had menus open" "$c: closed ($r)"
			else bad="$bad $w: menus open ($c; $r);"; fi
		fi
		# power armor
		if pa_in "$w"; then
			if pa_climb_out "$w" 2; then healed "$what: $w was in power armor" "climbed out: $PA_HOW"
			elif [ -n "$PA_REACHED" ]; then
				fail "$what: $w climbs out of power armor with the keyboard's E" "found in the suit before the step; the E reached the game's window and left them in:$PA_DIAG"
				if pa_force_out "$w"; then healed "$what: $w was in power armor" "taken out by the console (Actor.SwitchToPowerArmor none) after the E failed: not a player's action"
				else bad="$bad $w: in power armor, the E and the console did not get them out;"; fi
			elif pa_force_out "$w"; then healed "$what: $w was in power armor" "no press got them out ($PA_DIAG); taken out by the console (Actor.SwitchToPowerArmor none): not a player's action"
			else bad="$bad $w: in power armor, no press or the console got them out ($PA_DIAG);"; fi
		fi
		# down
		if st_down "$w"; then
			case "$ST_PREV" in
			downed) ;;
			"") warn "$what: $w is not down" "found down before the run's first step (god mode not known to be on yet): $(dc "$w" rules | grep -oE 'downed: [^|]*' | cut -c1-160); hp $(dcf "$w" "refinfo 00000014" hp)";;
			*) fail "$what: $w is not down (god mode was on through $ST_PREV)" "found down after $ST_PREV: $(dc "$w" rules | grep -oE 'downed: [^|]*' | cut -c1-160); hp $(dcf "$w" "refinfo 00000014" hp)";;
			esac
			_god "$w" on >/dev/null
			if st_help_up "$w"; then healed "$what: $w was down" "helped up by the other player's walk to their stand-in"
			else bad="$bad $w: down, and not helped up by the other player's walk ($(dc "$w" rules | grep -oE 'downed: [^|]*' | cut -c1-120));"; fi
		fi
		# god mode (each step's set-up: a player who can die ends the run's later steps)
		g=$(_god "$w" on); [ "$g" = "god=true" ] || bad="$bad $w: god mode would not go on ('$g');"
		# alive: a player found dead is the mod's failure (or the game's), not the rig's; the step is not run
		v=$(_view "$w")
		if [[ $v =~ life=([0-9]+) ]] && [ "${BASH_REMATCH[1]}" != 0 ]; then
			g=""; [ -n "$ST_PREV" ] && [ "$ST_PREV" != downed ] && g=" (god mode was on through $ST_PREV)"
			fail "$what: $w is alive$g" "found dead after $since ($v; life 0 is alive): $(dc "$w" rules | grep -oE 'downed: [^|]*' | cut -c1-160); hp $(dcf "$w" "refinfo 00000014" hp)"
			dead="$dead $w"; continue
		fi
		# the camera: a first- or third-person view
		if ! [[ $v =~ ^camera=(0|8)\ life=0$ ]]; then
			sleep 5; v=$(_view "$w")
			if ! [[ $v =~ ^camera=(0|8)\ life=0$ ]]; then
				dc "$w" focus >/dev/null; sleep 0.3; dc "$w" "key 0x46 100" >/dev/null; sleep 1.5; dc "$w" "key 0x46 100" >/dev/null; sleep 2; r=$(_view "$w")
				if [[ $r =~ ^camera=(0|8)\ life=0$ ]]; then healed "$what: $w's camera was not a first- or third-person view" "$v; the view key (F) twice: $r"
				else bad="$bad $w: camera not a normal view ($v, then $r after the view key twice);"; fi
			fi
		fi
	done
	[ -n "$dead" ] && [ -z "$bad" ] && { echo "GUARD: $step not run: found dead:$dead (a FAIL)"; return 1; }
	[ -z "$bad" ] && return 0
	rig_unhealed state "$what${A_ROUND:+ (round $A_ROUND)}" "${bad# }: the step is not run this time, its checks not made"
	return 1
}
