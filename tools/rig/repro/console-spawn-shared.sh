#!/bin/bash
# console-spawn-shared.sh [host|vm|both]: "Spawning NPCs with the console isn't synced at all" (two players, 0.20.1): an NPC one
# player places with `player.placeatme <base> 1` should stand, move, fight and die in the friend's game too.
#
# For each placing game (both by default, the host first), a calm NPC (CALM, a settler) and a hostile one (HOSTILE, a
# deathclaw) are placed with that game's own console, typed as a player would (`console player.placeatme`: the console
# command is the thing under test, not a way to reach the scene). The new runtime NPC is found in the placing game (dev
# `actors`: an id it did not list before), then looked for in the friend's game: a new actor of the same name within NEAR u of
# where the placing game has it, for up to WAIT s. With a copy found: who runs it in each game, how far apart the two games have
# it over 12 s, for the hostile one whether both games have it fighting, and its death (the placing game's console `kill`, said
# here: how it dies is not the point) seen in the friend's game. Both players are in god mode (rig.sh join) and nobody walks.
#
# REPRO: YES when a calm NPC placed with the console has no copy in the friend's game (the report), or a copy whose death the
# friend's game never saw; NO when every calm one placed had a copy that died with it. The hostile one is reported beside it
# (it was shared before the fix, as any hostile runtime NPC is) and makes a YES only when it has no copy either. VOID when a
# placed NPC could not be found in its own game.
cd "$(dirname "$0")" || exit 2
. ./lib.sh
DIR=${1:-both}; case "$DIR" in host|vm) PLACERS=$DIR;; both) PLACERS="host vm";; *) echo "usage: $0 [host|vm|both]"; exit 2;; esac
CALM=${CALM:-00020593}; HOSTILE=${HOSTILE:-0001DB4C}; WAIT=${WAIT:-30}; NEAR=${NEAR:-600}
need_session

# ids <who>: the runtime actors (FF......) dev `actors` lists round that player, one a line
ids() { dc "$1" "actors 2500" | tr ';' '\n' | grep -oE '(^| )FF[0-9A-F]{6} ' | tr -d ' ' | sort -u; }
name_of() { dc "$1" "actors 2500" | tr ';' '\n' | grep -E "(^| )$2 " | grep -oE "'[^']*'" | head -1; }
xy_of() { local i; i=$(dc "$1" "refinfo $2"); echo "$(field "$i" x) $(field "$i" y)"; }
brief() { dc "$1" "refinfo $2" | grep -oE "(x|y|hp|owner|dead)=[^ ]*" | paste -sd' ' -; }
is_dead() { [ "$(field "$(dc "$1" "refinfo $2")" dead)" = true ]; }
console_fields() { dc "$1" 'net status' | grep -oE 'console(Ran|Found|Made|Shared|Calm|Held)=[0-9]+|place(AtMe|Leveled)=[a-z]+/[0-9]+' | paste -sd' ' -; }

# new_local <who> <ids before>: the first runtime actor that game lists now and did not before (LOCAL), within 15 s
new_local() {
	local i now
	for i in $(seq 1 30); do
		now=$(ids "$1"); LOCAL=$(comm -13 <(echo "$2") <(echo "$now") | head -1)
		[ -n "$LOCAL" ] && return 0; sleep 0.5
	done
	return 1
}
# find_copy <placer> <local> <friend> <friend's ids before> <name>: a new runtime actor of that name in the friend's game within
# NEAR u of where the placer has its own (COPY, COPY_GAP), for WAIT s
find_copy() {
	local end=$((SECONDS + WAIT)) c at d
	COPY=""; COPY_GAP=""
	while [ $SECONDS -lt $end ]; do
		at=$(xy_of "$1" "$2")
		for c in $(comm -13 <(echo "$4") <(ids "$3")); do
			[ "$(name_of "$3" "$c")" = "$5" ] || continue
			d=$(dist_xy "$at" "$(xy_of "$3" "$c")")
			if [ -n "$d" ] && [ "$d" -le "$NEAR" ]; then COPY=$c; COPY_GAP=$d; return 0; fi
		done
		sleep 1
	done
	return 1
}

CALM_PLACED=0; CALM_SHARED=0; CALM_DIED=0; HOSTILE_PLACED=0; HOSTILE_SHARED=0; NOTES=""
# place <placer> <base> <calm|hostile>
place() {
	local who=$1 base=$2 kind=$3 other before_w before_o name gap maxgap=0 i a b
	other=$(other_game "$who")
	say "--- $who places a $kind NPC: console player.placeatme $base 1 (players $(dist_xy "$(pos_of host)" "$(pos_of vm)") u apart)"
	before_w=$(ids "$who"); before_o=$(ids "$other"); rmark "$who" "$kind"; rmark "$other" "$kind"
	say "$who's console fields before: $(console_fields "$who")"
	dc "$who" "console player.placeatme $base 1" >/dev/null
	if ! new_local "$who" "$before_w"; then
		say "$who's game lists no new runtime actor 15 s after the command (actors: $(dc "$who" 'actors 2500' | cut -c1-300))"
		verdict VOID "the NPC $who placed with the console ($base) was not found in its own game"
	fi
	name=$(name_of "$who" "$LOCAL")
	say "$who's game: $LOCAL $name $(brief "$who" "$LOCAL")"
	[ "$kind" = calm ] && CALM_PLACED=$((CALM_PLACED + 1)) || HOSTILE_PLACED=$((HOSTILE_PLACED + 1))
	if ! find_copy "$who" "$LOCAL" "$other" "$before_o" "$name"; then
		say "$other's game: NO copy of it within $WAIT s (new runtime actors there: $(comm -13 <(echo "$before_o") <(ids "$other") | paste -sd' ' -))"
		say "$who's log since: $(rsince "$who" "$kind" | grep -E 'Spawns:|ConsolePlace' | tail -4 | cut -c1-260 | paste -sd'|' -)"
		say "$other's log since: $(rsince "$other" "$kind" | grep -E 'Spawns:' | tail -4 | cut -c1-260 | paste -sd'|' -)"
		say "$who's console fields after: $(console_fields "$who")"
		NOTES="$NOTES; the $kind one $who placed ($LOCAL $name) had no copy in $other's game"
		dc "$who" "console $LOCAL.kill" >/dev/null; sleep 1; dc "$who" "console $LOCAL.disable" >/dev/null
		return 1
	fi
	say "$other's game: copy $COPY $(brief "$other" "$COPY") ($COPY_GAP u from where $who has it)"
	say "$who's log: $(rsince "$who" "$kind" | grep -E 'Spawns: sharing' | tail -2 | cut -c1-260 | paste -sd'|' -)"
	say "$who's console fields after: $(console_fields "$who")"
	[ "$kind" = calm ] && CALM_SHARED=$((CALM_SHARED + 1)) || HOSTILE_SHARED=$((HOSTILE_SHARED + 1))
	# where the two games have it, over 12 s
	for i in 1 2 3 4; do
		sleep 3; a=$(xy_of "$who" "$LOCAL"); b=$(xy_of "$other" "$COPY"); gap=$(dist_xy "$a" "$b")
		[ -n "$gap" ] && [ "$gap" -gt "$maxgap" ] && maxgap=$gap
	done
	say "over 12 s: $who has it at ($a), $other at ($b); the furthest the two were apart: $maxgap u"
	if [ "$kind" = hostile ]; then
		say "fighting: $who's game says '$(dc "$who" "combat $LOCAL" | cut -c1-120)', $other's '$(dc "$other" "combat $COPY" | cut -c1-120)'"
	fi
	# its death: the placing game's console kill
	dc "$who" "console $LOCAL.kill" >/dev/null
	if wait_for 15 is_dead "$other" "$COPY"; then
		say "death: killed in $who's game (console kill), dead in $other's too ($(brief "$other" "$COPY"))"
		[ "$kind" = calm ] && CALM_DIED=$((CALM_DIED + 1))
	else
		say "death: killed in $who's game (console kill: $(brief "$who" "$LOCAL")), NOT dead in $other's after 15 s ($(brief "$other" "$COPY"))"
		NOTES="$NOTES; the $kind one $who placed died in its own game only"
	fi
	return 0
}

for W in $PLACERS; do
	place "$W" "$CALM" calm
	place "$W" "$HOSTILE" hostile
	sleep 3
done
rkeep host end; rkeep vm end
say "calm: placed $CALM_PLACED, with a copy in the friend's game $CALM_SHARED, whose death was shared $CALM_DIED; hostile: placed $HOSTILE_PLACED, with a copy $HOSTILE_SHARED"
if [ "$CALM_SHARED" -lt "$CALM_PLACED" ]; then verdict YES "a calm NPC placed with the console stood in its own game only${NOTES}"; fi
if [ "$HOSTILE_SHARED" -lt "$HOSTILE_PLACED" ]; then verdict YES "a hostile NPC placed with the console stood in its own game only${NOTES}"; fi
if [ "$CALM_DIED" -lt "$CALM_SHARED" ]; then verdict YES "a calm console NPC had a copy, but its death was not shared${NOTES}"; fi
verdict NO "every NPC placed with the console ($CALM_PLACED calm, $HOSTILE_PLACED hostile) stood in the friend's game too, and each calm one's death was shared${NOTES}"
