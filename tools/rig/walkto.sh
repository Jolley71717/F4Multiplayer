#!/bin/bash
# walkto.sh <host|vm> <refHex | x y> [speed 0-1] [radius] [timeoutS]
#
# Walks that game's player to a reference or a point with the dev channel's virtual pad (the walkto command: the left
# stick pushes toward the target and the right stick turns the player to face it, re-aimed every frame inside the
# game; src/game/VirtualPad.h). The game's window need not be in front, and nothing goes through Windows' input.
# Waits for the walk to end and prints its last line: "arrived ...", "stuck ...", "timed out ..." (exit 0 only for
# arrived). Progress is in the plugin log once a second ("Pad: walkto").
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
who=${1:?usage: walkto.sh <host|vm> <refHex | x y> [speed] [radius] [timeoutS]}; shift
[ $# -ge 1 ] || { sed -n '2,8p' "$0"; exit 2; }
start=$(dc "$who" "walkto $*")
echo "$who: $start"
case "$start" in error*|"") exit 2 ;; esac
for i in $(seq 1 1300); do   # up to the longest timeout (600 s) and some
	sleep 0.5
	s=$(dc "$who" walkto)
	case "$s" in
	"not walking; last: "*) last=${s#not walking; last: }; echo "$who: $last"; case "$last" in arrived*) exit 0 ;; *) exit 1 ;; esac ;;
	esac
done
echo "$who: no end to the walk in sight: $s"; dc "$who" "walkto stop" >/dev/null; exit 1
