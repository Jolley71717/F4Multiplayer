# Performance

What the plugin costs the game, how to read it, and what the numbers were when each release went out.

## The `perf` dev command

Every frame of ours on the game's main thread (`RunFrame` in src/main.cpp) is timed part by part: the stretches of
`Session::Frame` (Story, Steam, Packets, WorldSync, NpcSync, ... Puppets) and the work around it (posted dev-channel
work, DevCommands, Background, Menu). `src/game/Perf.h` keeps the last 3600 frames of each part. The dev channel's
`perf` answers

    frames=N ours p50=… p99=… max=…ms worst_ever=…ms hitches=H ours_in_hitches=K | Story p50/p99/max | …

in milliseconds: `ours` is the whole of our frame; `hitches` counts the game's frames over 100 ms (as `frames` does)
and `ours_in_hitches` those where our work took at least half of the frame, which is the number that says whether a
stutter is ours. `perf reset` starts over. Parts that never took a microsecond are left out.

A frame of ours is marked with `PERF_SCOPE("Name")` (the rest of the block) or, inside `Session::Frame`, with
`PERF_NEXT(perf, "Name")` (the stretch up to the next one). The recording is pure and unit-tested
(tests/PerfTests.cpp).

## Baselines

`docs/perf/<version>-baseline.txt`: the standing two-game set (solo idle and moving, session idle, movement trace,
Corvega combat, terminals/loot/safe, grenades, power armor, a 3-minute cell churn), with `perf` and `frames` read on
both games after each phase (`perf-baseline.sh` on the rig). Compare a release candidate's run against the last one:
a part whose p50 or p99 grew, or an `ours_in_hitches` above zero, is a regression to look at before promotion.

0.8.7 (the first baseline, with builds running on the host at low priority): ours p50 0.13 ms alone and 0.18–0.27 ms
in a session on the host, 0.15 and 0.29–0.35 ms on the VM; p99 at most 1.7 ms (host) and 2.1 ms (VM); the worst
single frame 8 ms (host) and 28 ms (VM, one Voice spike); `ours_in_hitches` 0 in every phase. The steady top cost is
Voice (p50 0.06–0.13 ms, every frame), then Puppets with 16 mirrors (p99 1.1–1.4 ms) and QuestSync (p99 0.3 ms);
Achievements and PartyClock showed single spikes of 5–8 ms.

## Tracy, for a frame-by-frame look

A developer build with Tracy's client (https://github.com/wolfpld/tracy, the profiler's version must match the
source's; 0.14.1 is what the rig uses) shows the same parts as zones on a timeline, with each of our frames as a
frame mark:

    xmake f --tracy=<Tracy's source folder>    # then build as usual
    xmake f --tracy=                           # back to a normal build

The client listens on the game's machine only (port 8086) and sends nothing until a capture connects; sampling, call
stacks and the crash handler stay off (the crash logger keeps those). It starts when the plugin loads, not in DllMain.
`tools/package.ps1` always configures without it and refuses a DLL built with it: a profiling build is never released.

Without a screen: `tracy-capture -o run.tracy -f -s 60` records a minute from the running game, and
`tracy-csvexport run.tracy` gives each zone's count, mean, min and max; open the `.tracy` file in `tracy-profiler`
to see where a slow frame went.

`docs/perf/0.8.7-tracy-host-solo.csv` is a minute of the host alone in Sanctuary on 0.8.7 (2887 frames, 72k zones): our frame
means 0.25 ms, of which Voice is 38% (94 µs every frame, with no session at all), then PartyClock, WorldSync, NpcSync
and QuestSync at 12–30 µs each.

## ETW

Not used: a WPR trace of the whole PC is 2 GB a minute and needs admin rights. If a stutter is not ours
(`ours_in_hitches` stays 0 while the game hitches), that is the tool for the rest of the machine.
