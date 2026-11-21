#!/bin/bash
# dogmeat-run.sh: the host's Dogmeat, following the host's player as it sprints, is seen squatting along instead of running in the
# friend's game (reported on the joiner: the dog's copy carried along at a run in a crouched pose). The host's game is driven: its
# player sprints (real input: the virtual pad's stick at full and its L3, the sprint) down an open slope east of Red Rocket with
# Dogmeat following, then walks back. Throughout the sprint the VM's copy of the dog is read (dev `mirror`: the gait it was told,
# played=, what its owner said, ownerGait=, and its graph's variables) with the host's own dog's place (its pace), and both games
# log the dog's animation events (dev `animlog`); the friend's screen is recorded (in-VM ffmpeg) for one still frame per state.
#
# Console set-up, said so:
# - both games in the Commonwealth near Red Rocket (coc RedRocketExt when not);
# - Dogmeat's own quest (0006DFAF): stage 4 (he may be a companion) and, when his Red Rocket meeting runs (stage 110: its scene
#   holds him on StayNearAndLookAtPlayer, recruited or not, until the player's talk ends it), the talk's stages 140, 150 and 175;
#   then the Followers script's SetDogmeatCompanion on the host (with evp) when he is not held;
# - the hour set to noon in both games (the frames are of a dog in daylight);
# - the host's player placed at the top of the slope (START) and Dogmeat moved to it, the friend's player placed beside the run's
#   line (VM_AT) facing it. The friend then turns toward the dog (a look, as the rig's steps aim). No movement is injected: the
#   sprint is the pad's stick and L3, the dog's run his own AI's.
#
# The rig's dev calls take 1 to 4 s each and a game serves one at a time, so a read is one call to each game, side by side.
# REPRO: YES when, at the host's dog's run pace (RUN_PACE u/s or more), the copy is told anything but a run or fast trot on most
# reads (played= the walk, or standing), or its graph holds the sit (bForceIdleStop), or it was told again to set off (resyncs) 3
# times or more while running, or its graph sent no FootRunBreath (DogmeatDefault.hkx's run clip's event) while the real dog's did;
# NO when the copy's graph runs with him; VOID when the host's dog never ran (fewer than 2 reads at RUN_PACE).
cd "$(dirname "$0")" || exit 2
. ./lib.sh
DOG=0001D162; QUEST=0006DFAF; MEETING_DONE="140 150 175"
START=${START:-"-62720 81077 7300"}; END=${END:-"-62700 78750"}; VM_AT=${VM_AT:-"-62050 79900 7000 270"}
RUN_PACE=${RUN_PACE:-350}; READS=${READS:-10}; VIDEO=${VIDEO:-1}; VIDEO_S=${VIDEO_S:-40}
need_session

held() { dc host companions | grep -oE "DogmeatCompanion=[0-9A-F]{8}" | cut -d= -f2; }
dogxy() { local r; r=$(dc "$1" "refinfo $DOG"); echo "$(field "$r" x) $(field "$r" y)"; }
ms() { date +%s%3N; }
holds_dog() { [ "$(held)" = "$DOG" ]; }
has_copy() { dc vm "mirror $DOG" | grep -q 'mirror=true'; }
done_stages() { dc host "stages $QUEST" | grep -oE 'done\([0-9]+\)=[0-9,]*' | cut -d= -f2; }
has_done() { grep -qE "(^|,)$1(,|$)" <<<"$(done_stages)"; }
# video_start / video_get: the friend's screen recorded in its own session (C:\F4MP\rec2.ps1: ffmpeg's gdigrab at 15 fps)
video_start() {
	vmssh 'Remove-Item C:\F4MP\rec.mp4,C:\F4MP\rec.start -ErrorAction SilentlyContinue' >/dev/null
	vmssh "& C:\\F4MP\\in-vm-run.ps1 -Name \"F4MP-Rec-\$(Get-Date -Format HHmmssfff)\" -Exe C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -Arguments '-WindowStyle Hidden -NoProfile -ExecutionPolicy Bypass -File C:\\F4MP\\rec2.ps1 -Secs $VIDEO_S'" >/dev/null
	wait_for 20 eval "vmssh 'Test-Path C:\\F4MP\\rec.start' | grep -q True"
}
video_get() {
	local i; for i in $(seq 1 $((VIDEO_S + 20))); do vmssh 'Get-Process ffmpeg -ErrorAction SilentlyContinue | Measure-Object | Select-Object -ExpandProperty Count' | grep -q '^0$' && break; sleep 1; done
	scp -q -i "$VM_KEY" -o LogLevel=ERROR "$VM_SSH:C:/F4MP/rec.mp4" "$REPRO_OUT/friend.mp4" 2>/dev/null
	VIDEO_T0=$(vmssh 'Get-Content C:\F4MP\rec.start' | tr -d '[:space:]')
}

say "dogmeat-run: from ($START) to ($END), the friend at ($VM_AT); run pace $RUN_PACE u/s, $READS reads, video $VIDEO"
read -r sx sy sz <<<"$START"; read -r ex ey <<<"$END"
for who in host vm; do
	st=$(dc "$who" status); far=$(dist_xy "$(pos_of "$who")" "$sx $sy")
	if [ "$(field "$st" worldspace)" != 0000003C ] || [ -z "$far" ] || [ "$far" -gt 9000 ]; then
		say "set-up: the $who's player is not near Red Rocket ($(grep -oE "location=[0-9A-F]+ '[^']*'" <<<"$st"), ${far:-?} u): console coc RedRocketExt"
		dc "$who" focus >/dev/null; dc "$who" "console coc RedRocketExt" >/dev/null; sleep 6
		wait_ingame "$who" 90 || verdict VOID "the $who's game did not come back from its coc"
	fi
done
need_session
for who in host vm; do dc "$who" "pad unplug" >/dev/null; dc "$who" "console set gamehour to 12" >/dev/null; done
say "set-up: the hour set to noon in both games (console)"
if ! has_done 4; then say "set-up: console setstage $QUEST 4 (Dogmeat may be a companion)"; dc host "console setstage $QUEST 4" >/dev/null; sleep 2; fi
if has_done 110 && ! has_done 175; then
	say "set-up: his Red Rocket meeting is running (done $(done_stages)): console setstage $QUEST $MEETING_DONE, as his talk would"
	for s in $MEETING_DONE; do dc host "console setstage $QUEST $s" >/dev/null; sleep 2; done
fi
if [ "$(held)" != "$DOG" ]; then
	say "set-up: FollowersScript.SetDogmeatCompanion $DOG and evp on the host (console)"
	dc host "console cqf Followers \"FollowersScript.SetDogmeatCompanion\" $DOG" >/dev/null; sleep 1
	wait_for 25 holds_dog || verdict VOID "the host's Dogmeat alias does not hold him ('$(held)')"
fi
dc host "console $DOG.evp" >/dev/null
say "set-up: his quest done $(done_stages); his AI $(dc host "mirror $DOG" | grep -oE 'ai: [^|]*' | head -1 | cut -c1-100)"
say "set-up: the host's player placed at ($START), Dogmeat moved to it, the friend's player placed at ($VM_AT) (console)"
dc host "setpos 00000014 $sx $sy $sz 180" >/dev/null; sleep 3
dc host "console $DOG.moveto player" >/dev/null
dc vm "setpos 00000014 $VM_AT" >/dev/null; sleep 4
wait_for 30 has_copy || verdict VOID "the VM has no copy of Dogmeat: $(dc vm "mirror $DOG" | cut -c1-160)"
say "the VM's copy before: $(dc vm "mirror $DOG" | grep -oE '(loaded|moving|speed|played|ownerGait|resyncs|idleStops|taken|bForceIdleStop)=[^ ;|,]*' | paste -sd' ')"
sleep 3

# the sprint: the stick full ahead, L3 (the sprint), the reads in the meantime; the friend looks at the dog
[ "$VIDEO" = 1 ] && { video_start && say "the friend's screen is recorded ($VIDEO_S s)" || say "the friend's screen recorder did not start"; }
rmark host run; rmark vm run
dc vm "turnto $DOG 300 aim" >/dev/null
dc host "animlog on $DOG" >/dev/null; dc vm "animlog on $DOG" >/dev/null
dc host focus >/dev/null
say "the host sprints to ($ex,$ey) (real stick at 1 and L3)"
dc host "walkto $ex $ey 1 100 30" >/dev/null
dc host "pad button ls 400" >/dev/null
fast=0; runs=0; walks=0; sits=0; reads=0; trail=""; t0=$(ms); read -r x0 y0 <<<"$(dogxy host)"; r0=$(field "$(dc vm "mirror $DOG")" resyncs)
: > "$REPRO_OUT/reads.txt"
for i in $(seq 1 "$READS"); do
	dc vm "mirror $DOG" > "$REPRO_OUT/r-vm" &
	dc host "refinfo $DOG" > "$REPRO_OUT/r-hp" &
	wait
	vmr=$(<"$REPRO_OUT/r-vm"); hp=$(<"$REPRO_OUT/r-hp"); t1=$(ms)
	x1=$(field "$hp" x); y1=$(field "$hp" y)
	v=$(awk -v a="$x0" -v b="$y0" -v c="$x1" -v e="$y1" -v s="$t0" -v t="$t1" 'BEGIN{if (a=="" || c=="" || t<=s) {print ""; exit}; printf "%d", sqrt((c-a)^2+(e-b)^2) / ((t-s)/1000)}')
	x0=$x1; y0=$y1; t0=$t1
	played=$(field "$vmr" played); sit=$(field "$vmr" bForceIdleStop)
	echo "$i $(date +%T.%N | cut -c1-12) pace=$v dog=($x1,$y1) vm[$(grep -oE '(moving|speed|played|ownerGait|resyncs|idleStops|taken|bForceIdleStop|iSyncLocomotionRangeID|fRunClipMult|fTrotFastClipMult|fTrotClipMult|fWalkClipMult)=[^ ;|,]*' <<<"$vmr" | paste -sd' ')]" >> "$REPRO_OUT/reads.txt"
	if is_int "$v"; then
		reads=$((reads + 1)); trail="$trail $v:$played"
		if [ "$v" -ge "$RUN_PACE" ]; then
			fast=$((fast + 1))
			case "$played" in 2|3) runs=$((runs + 1));; 0|10|-1) walks=$((walks + 1));; esac
			[ "$sit" = true ] && sits=$((sits + 1))
		fi
	fi
	[ "$i" = 2 ] && { dc host "pad button ls 400" >/dev/null; dc vm "turnto $DOG 300 aim" >/dev/null; }
	[ "$i" = 4 ] && dc vm "turnto $DOG 300 aim" >/dev/null
	[ "$i" -ge 5 ] && case "$(dc host walkto)" in "not walking"*) [ "$v" = 0 ] && break;; esac
done
hlog=$(dc host "animlog off"); vlog=$(dc vm "animlog off")
r1=$(field "$(dc vm "mirror $DOG")" resyncs)
say "the friend's aim at the copy: $(dc vm "aim $DOG" | cut -c1-200)"
dc host "walkto stop" >/dev/null; dc host "pad unplug" >/dev/null
echo "$hlog" > "$REPRO_OUT/animlog-host.txt"; echo "$vlog" > "$REPRO_OUT/animlog-vm.txt"
rsince host run > "$REPRO_OUT/run-host.log"; rsince vm run > "$REPRO_OUT/run-vm.log"
cat "$REPRO_OUT/reads.txt" | while read -r l; do say "  read $l"; done
say "reads (pace u/s:played):$trail"
# the animation logs hold every graph's events in that game (the players' too): the dog's own are the quadruped's (Foot*Front/Back,
# FootRunBreath) and his idles'
say "the host's animation events: $(tr ' ' '\n' <<<"$hlog" | sed 's/(.*//' | sort | uniq -c | sort -rn | head -14 | awk '{printf "%s x%s, ", $2, $1}')"
say "the friend's animation events: $(tr ' ' '\n' <<<"$vlog" | sed 's/(.*//' | sort | uniq -c | sort -rn | head -14 | awk '{printf "%s x%s, ", $2, $1}')"
grep "Puppets: $DOG" "$REPRO_OUT/run-vm.log" | tail -5 | while read -r l; do say "  vm: $l"; done
[ "$VIDEO" = 1 ] && { video_get; say "the friend's screen: friend.mp4 (started at ${VIDEO_T0:-?})"; }

# back to the top of the slope (a walk)
rwalk host "$sx" "$sy" 120 0.6 >/dev/null
rkeep host dogmeat-run; rkeep vm dogmeat-run

say "at the run's pace: $fast reads; the copy told a run or fast trot $runs, the walk or standing $walks; in the sit $sits; resyncs during the sprint: ${r0:-?} -> ${r1:-?}"
[ "$fast" -ge 2 ] || verdict VOID "the host's Dogmeat never ran: $fast of $reads reads at $RUN_PACE u/s or more (pace:played:$trail)"
why=""
[ $((walks * 2)) -ge "$fast" ] && why="$why the copy was told the walk or to stand on $walks of $fast reads at a run's pace;"
[ "$sits" -gt 0 ] && why="$why its graph held the sit on $sits of $fast;"
is_int "$r0" && is_int "$r1" && [ $((r1 - r0)) -ge 3 ] && why="$why it was told again to set off $((r1 - r0)) times while running;"
hrun=$(grep -o 'FootRunBreath' <<<"$hlog" | wc -l); vrun=$(grep -o 'FootRunBreath' <<<"$vlog" | wc -l)
say "the run clip's FootRunBreath in the animation logs: the host's dog $hrun, the copy $vrun"
[ "$hrun" -gt 0 ] && [ "$vrun" = 0 ] && why="$why its graph sent no FootRunBreath (the run clip's) while the real dog's sent $hrun;"
[ -n "$why" ] && verdict YES "the friend's copy of the running Dogmeat does not run:$why"
verdict NO "the friend's copy of the running Dogmeat ran with him: told a run or fast trot on $runs of $fast reads at a run's pace, no sit, resyncs ${r0:-?} -> ${r1:-?}, FootRunBreath $vrun (the real dog's $hrun)"
