#include "Test.h"

#include "game/ArmorFrames.h"
#include "game/Gait.h"
#include "game/IdleRelease.h"
#include "game/MoveSync.h"
#include "Protocol.h"

#include <cstring>

// Walk or run for a humanoid puppet (src/game/Gait.h), driven frame by frame together with MoveSync the way
// Puppets::DriveAnimation does, against a model of the humanoid locomotion graph: standing takes only
// "moveStart", which enters the gait its sync variable names; moving, the gait state machine changes only on
// "Walk"/"Run" and writes its own state back into the sync variable; nothing changes the gait by Speed.
namespace
{
	using namespace std::chrono_literals;

	const auto     t0 = MoveSync::Clock::time_point{} + 1h;
	constexpr auto FRAME = 62ms;  // a 16 fps game, the slow side
	constexpr int  WALK = 0;
	constexpr int  RUN = 2;

	struct Graph
	{
		bool         moving = false;
		int          gait = WALK;  // the state the graph plays
		std::int32_t sync = WALK;  // iSyncLocomotionSpeed
		int          gaitEvents = 0;
		int          stops = 0;

		bool Notify(const char* a_event)
		{
			if (std::strcmp(a_event, "moveStart") == 0) {
				if (moving) {
					return false;
				}
				moving = true;
				gait = sync;
				return true;
			}
			if (std::strcmp(a_event, "moveStop") == 0) {
				if (!moving) {
					return false;
				}
				moving = false;
				++stops;
				return true;
			}
			const int to = std::strcmp(a_event, "Run") == 0 ? RUN : std::strcmp(a_event, "Walk") == 0 ? WALK : -1;
			if (to < 0 || !moving) {
				return false;  // the weapon-drawn graph's events, or not in locomotion
			}
			++gaitEvents;
			gait = to;
			return true;
		}

		void SetSync(std::int32_t a_value)
		{
			if (!moving) {
				sync = a_value;  // while active the state machine writes its own state over it
			}
		}

		void Update()
		{
			if (moving) {
				sync = gait;
			}
		}
	};

	struct Puppet
	{
		bool                        moving = false;
		MoveSync::Clock::time_point nextResync{};
		Gait::Kind                  gait = Gait::Kind::kWalk;
		float                       gaitSpeed = 0.0f;
		bool                        started = false;
	};

	// One DriveAnimation frame, in its order.
	void Frame(Graph& a_graph, Puppet& a_puppet, float a_speed, MoveSync::Clock::time_point a_now, std::uint16_t a_moveMode = 0)
	{
		const float gaitSpeed = Gait::Smooth(a_puppet.gaitSpeed, a_speed, a_puppet.started ? std::chrono::duration<float>(FRAME).count() : 0.0f);
		const auto  gait = Gait::Pick(gaitSpeed, a_puppet.gait, a_moveMode);
		a_graph.SetSync(Gait::SyncValue(gait));
		const auto step = MoveSync::Drive(a_puppet.moving, a_puppet.nextResync, a_speed, a_now, [&](MoveSync::Event a_event) {
			return a_graph.Notify(MoveSync::NamesOf(a_event, false).first);
		});
		if (Gait::Send(step.moving, a_puppet.moving, gait, a_puppet.gait, step.resent != nullptr)) {
			a_graph.Notify(Gait::Event(gait));
			a_graph.Notify(Gait::WeaponEvent(gait));
		}
		a_puppet.moving = step.moving;
		a_puppet.nextResync = step.nextResync;
		a_puppet.gait = gait;
		a_puppet.gaitSpeed = gaitSpeed;
		a_puppet.started = true;
		a_graph.Update();
	}

	// Frames until the graph runs (or -1 within a_limit).
	int FramesToRun(Graph& a_graph, Puppet& a_puppet, float a_speed, MoveSync::Clock::time_point& a_now, int a_limit = 40)
	{
		for (int i = 1; i <= a_limit; ++i) {
			Frame(a_graph, a_puppet, a_speed, a_now += FRAME);
			if (a_graph.moving && a_graph.gait == RUN) {
				return i;
			}
		}
		return -1;
	}
}

TEST("gait: a friend who starts running is shown running within half a second, and stops")
{
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	// The host's speed as the joiner's game saw it as they set off, live: 86, 205, 457, 545, then a run.
	for (const float speed : { 86.0f, 205.0f, 457.0f, 545.0f }) {
		Frame(graph, puppet, speed, now += FRAME);
	}
	const int frames = graph.gait == RUN ? 0 : FramesToRun(graph, puppet, 373.0f, now);
	CHECK(frames >= 0);
	CHECK(4 + frames <= 8);  // 8 frames at 16 fps
	CHECK(graph.gaitEvents == 2);  // the walk it set off in, then the run
	for (int i = 0; i < 10; ++i) {
		Frame(graph, puppet, 0.0f, now += FRAME);
	}
	CHECK(!graph.moving);
	// Off again after a short stop: the run comes back as quickly.
	const int again = FramesToRun(graph, puppet, 373.0f, now);
	CHECK(again >= 0);
	CHECK(again <= 8);
}

TEST("gait: walking stays a walk, at its own pace, and a sprint plays the run no faster than 1.6 times")
{
	for (const auto& [speed, expected] : { std::pair{ 95.0f, WALK }, std::pair{ 130.0f, WALK }, std::pair{ 373.0f, RUN }, std::pair{ 700.0f, RUN } }) {
		Graph  graph;
		Puppet puppet;
		auto   now = t0;
		for (int i = 0; i < 40; ++i) {
			Frame(graph, puppet, speed, now += FRAME);
		}
		CHECK(graph.moving);
		CHECK(graph.gait == expected);
		CHECK(graph.gaitEvents <= 2);
	}
	CHECK(Gait::Playback(373.0f, Gait::RUN_CLIP) == 1.0f);
	CHECK(Gait::Playback(700.0f, Gait::RUN_CLIP) == 1.6f);
	CHECK(Gait::Playback(110.0f, Gait::WALK_CLIP) > 1.1f);
	CHECK(Gait::Playback(10.0f, Gait::WALK_CLIP) == 0.5f);
}

TEST("gait: a run that sets off after a stop plays its clip at the run's pace at once, not at the slowest rate")
{
	// Standing: the smoothed speed sits at 0 whichever way it is seeded.
	float standing = 0.0f;
	for (int i = 0; i < 40; ++i) {
		standing = Gait::Smoothed(standing, 0.0f, 0.062f);
	}
	// The first frame of the run, and the next ones.
	float speed = Gait::Smoothed(standing, 373.0f, 0.062f);
	CHECK(Gait::Playback(speed, Gait::RUN_CLIP) > 0.99f);
	speed = Gait::Smoothed(speed, 373.0f, 0.062f);
	CHECK(Gait::Playback(speed, Gait::RUN_CLIP) > 0.99f);
	// Once moving, a swing is still smoothed, not followed.
	CHECK(Gait::Smoothed(373.0f, 88.0f, 0.062f) > 300.0f);
}

TEST("gait: a friend running while the graph refuses the start keeps swinging speeds smoothed")
{
	// Seeded once from the first reading, then every swing is averaged, whatever the graph took.
	constexpr float live[] = { 623.0f, 145.0f, 747.0f, 177.0f, 425.0f, 200.0f, 88.0f, 378.0f };
	float           speed = Gait::Smoothed(0.0f, live[0], 0.062f);
	CHECK(speed == live[0]);
	for (int i = 1; i < 80; ++i) {
		speed = Gait::Smoothed(speed, live[i % 8], 0.062f);
		CHECK(speed > 150.0f);
		CHECK(speed < 650.0f);
	}
}

TEST("gait: a slow friend's speeds, which swing from frame to frame, show one steady run")
{
	// What the host's game read for a 16 fps joiner running, live: 350 on average.
	constexpr float live[] = { 623.0f, 145.0f, 747.0f, 177.0f, 425.0f, 200.0f, 88.0f, 378.0f };
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	for (int i = 0; i < 16; ++i) {
		Frame(graph, puppet, live[i % 8], now += FRAME);
	}
	CHECK(graph.gait == RUN);
	const int settled = graph.gaitEvents;
	CHECK(settled <= 2);
	// The same samples in another order, for 10 s.
	for (int i = 0; i < 160; ++i) {
		Frame(graph, puppet, live[(i * 3) % 8], now += FRAME);
		CHECK(graph.gait == RUN);
	}
	CHECK(graph.gaitEvents == settled);
	CHECK(Gait::Playback(puppet.gaitSpeed, Gait::RUN_CLIP) > 0.6f);
	CHECK(Gait::Playback(puppet.gaitSpeed, Gait::RUN_CLIP) < 1.4f);
}

TEST("gait: slowing from a run to a walk walks, and a pace between them keeps the gait it had")
{
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	CHECK(FramesToRun(graph, puppet, 373.0f, now) >= 0);
	for (int i = 0; i < 40; ++i) {
		Frame(graph, puppet, 180.0f, now += FRAME);  // between RUN_DOWN and RUN_UP
	}
	CHECK(graph.gait == RUN);
	for (int i = 0; i < 40; ++i) {
		Frame(graph, puppet, 110.0f, now += FRAME);
	}
	CHECK(graph.moving);
	CHECK(graph.gait == WALK);
	for (int i = 0; i < 40; ++i) {
		Frame(graph, puppet, 180.0f, now += FRAME);
	}
	CHECK(graph.gait == WALK);
}

TEST("gait: a graph put back in its walk while running (a jump, a stagger) is told to run again")
{
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	CHECK(FramesToRun(graph, puppet, 373.0f, now) >= 0);
	// A landing drops it back to standing in its walk; the resync starts it again, in the run.
	graph.moving = false;
	graph.gait = WALK;
	graph.sync = WALK;
	CHECK(FramesToRun(graph, puppet, 373.0f, now, 12) >= 0);
}

TEST("gait: the events go only on a change, a start or a resync, never every frame")
{
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	for (int i = 0; i < 200; ++i) {
		Frame(graph, puppet, 373.0f, now += FRAME);
	}
	CHECK(graph.gaitEvents == 2);  // set off walking, then the run
	CHECK(Gait::Send(true, false, Gait::Kind::kRun, Gait::Kind::kRun, false));
	CHECK(!Gait::Send(true, true, Gait::Kind::kRun, Gait::Kind::kRun, false));
	CHECK(Gait::Send(true, true, Gait::Kind::kRun, Gait::Kind::kRun, true));
	CHECK(!Gait::Send(false, true, Gait::Kind::kWalk, Gait::Kind::kRun, false));
	CHECK(std::strcmp(Gait::WeaponEvent(Gait::Kind::kRun), "runStart") == 0);
	CHECK(Gait::SyncValue(Gait::Kind::kRun) == RUN);
	CHECK(Gait::WeaponState(Gait::Kind::kRun) == 1);
}

namespace
{
	// The owner's game to the stand-in's: a state the owner's Session sends, through the wire format, to the
	// frame that plays it.
	void Sent(Graph& a_graph, Puppet& a_puppet, float a_speed, std::uint16_t a_moveMode, MoveSync::Clock::time_point a_now)
	{
		Protocol::PlayerState state;
		state.speed = a_speed;
		state.moveMode = a_moveMode;
		const auto got = Protocol::DecodePlayerState(Protocol::Encode(state));
		REQUIRE(got);
		Frame(a_graph, a_puppet, got->speed, a_now, got->moveMode);
	}

	// moveMode as the owner's game reported it, live, running and walking forward and sprinting.
	constexpr std::uint16_t RUNNING = 0xA6;
	constexpr std::uint16_t WALKING = 0x66;
	constexpr std::uint16_t SPRINTING = 0x1A6;
	constexpr float         slowRun[] = { 623.0f, 145.0f, 747.0f, 177.0f, 425.0f, 200.0f, 88.0f, 378.0f };
}

TEST("gait: a friend's own run, sent with their state, shows at once, however unevenly their speed arrives")
{
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	// A slow game's first speeds after setting off: the gait goes by what the owner plays, not by these.
	Sent(graph, puppet, 88.0f, RUNNING, now += FRAME);
	Sent(graph, puppet, 145.0f, RUNNING, now += FRAME);
	CHECK(graph.moving);
	CHECK(graph.gait == RUN);
	CHECK(graph.gaitEvents == 1);  // entered in its run: no walk first
	for (int i = 0; i < 160; ++i) {
		Sent(graph, puppet, slowRun[(i * 3) % 8], RUNNING, now += FRAME);
		CHECK(graph.gait == RUN);
	}
	CHECK(graph.gaitEvents == 1);
}

TEST("gait: a friend walking stays a walk though a slow game reports a run's speed, and a sprint plays faster")
{
	{
		Graph  graph;
		Puppet puppet;
		auto   now = t0;
		for (int i = 0; i < 80; ++i) {
			Sent(graph, puppet, slowRun[i % 8] * 0.5f, WALKING, now += FRAME);  // 175 on average, up to 373
			CHECK(!graph.moving || graph.gait == WALK);
		}
		CHECK(graph.moving);
		CHECK(Gait::Playback(puppet.gaitSpeed, Gait::WALK_CLIP) <= 1.6f);
	}
	{
		Graph  graph;
		Puppet puppet;
		auto   now = t0;
		for (int i = 0; i < 40; ++i) {
			Sent(graph, puppet, 500.0f, SPRINTING, now += FRAME);
		}
		CHECK(graph.gait == RUN);
		CHECK(Gait::Playback(puppet.gaitSpeed, Gait::RUN_CLIP) > 1.25f);
	}
}

TEST("gait: a friend going from a run to a walk walks on the next state, and standing still stops")
{
	Graph  graph;
	Puppet puppet;
	auto   now = t0;
	for (int i = 0; i < 20; ++i) {
		Sent(graph, puppet, 373.0f, RUNNING, now += FRAME);
	}
	CHECK(graph.gait == RUN);
	// The speed falls away over a few frames as it does live; the owner's walk is already in the state.
	Sent(graph, puppet, 300.0f, WALKING, now += FRAME);
	CHECK(graph.gait == WALK);
	for (int i = 0; i < 10; ++i) {
		Sent(graph, puppet, 92.0f, WALKING, now += FRAME);
		CHECK(graph.gait == WALK);
	}
	Sent(graph, puppet, 0.0f, 0, now += FRAME);
	CHECK(!graph.moving);
	// And off again at a run: in the run from the start.
	const int before = graph.gaitEvents;
	Sent(graph, puppet, 373.0f, RUNNING, now += FRAME);
	CHECK(graph.moving);
	CHECK(graph.gait == RUN);
	CHECK(graph.gaitEvents == before + 1);
}

TEST("gait: a state with no gait in it (an older friend, a scripted NPC) goes by the speed")
{
	CHECK(Gait::Pick(373.0f, Gait::Kind::kWalk, 0) == Gait::Kind::kRun);
	CHECK(Gait::Pick(95.0f, Gait::Kind::kWalk, 0) == Gait::Kind::kWalk);
	CHECK(Gait::Pick(373.0f, Gait::Kind::kRun, 0x01) == Gait::Kind::kRun);  // forward only
	CHECK(Gait::Pick(373.0f, Gait::Kind::kRun, WALKING) == Gait::Kind::kWalk);
	CHECK(Gait::Pick(95.0f, Gait::Kind::kWalk, RUNNING) == Gait::Kind::kRun);
}

TEST("gait: a slow friend's own animation speed, sent instead of the ground they covered, keeps the run going without a stop")
{
	// A 16 fps owner running a steady 373: the ground covered between its samples, live, with the hitches a
	// slow game has (a sample taken before its next frame moved it at all).
	constexpr float travelled[] = { 623.0f, 0.0f, 747.0f, 177.0f, 0.0f, 200.0f, 88.0f, 378.0f };
	const auto      run = [&](bool a_own) {
		Graph  graph;
		Puppet puppet;
		auto   now = t0;
		for (int i = 0; i < 160; ++i) {
			Sent(graph, puppet, Gait::Reported(a_own ? 373.0f : 0.0f, travelled[i % 8]), RUNNING, now += FRAME);
		}
		return std::pair{ graph, puppet };
	};
	const auto [own, ownPuppet] = run(true);
	CHECK(own.moving);
	CHECK(own.gait == RUN);
	CHECK(own.stops == 0);
	CHECK(own.gaitEvents == 1);
	CHECK(Gait::Playback(ownPuppet.gaitSpeed, Gait::RUN_CLIP) > 0.95f);
	CHECK(Gait::Playback(ownPuppet.gaitSpeed, Gait::RUN_CLIP) < 1.05f);
	// The ground covered alone (no animation speed: an older build's way) stopped and started it on each hitch.
	const auto [covered, coveredPuppet] = run(false);
	CHECK(covered.stops > 10);
	// Standing still is 0 either way, and a friend carried by a lift with their legs still goes by the ground.
	CHECK(Gait::Reported(0.0f, 0.0f) == 0.0f);
	CHECK(Gait::Reported(0.0f, 120.0f) == 120.0f);
	CHECK(Gait::Reported(92.6f, 300.0f) == 92.6f);
}

// A friend in power armor (src/game/ArmorFrames.h, Puppets::GraphRebuilt, Puppets::ReleaseIdle). The engine's switch-in
// rebuilds the stand-in's 3D with a fresh animation graph: whatever the old one was told, the new one stands in its
// base state, and in the 0.8.7-rc1 soak the one it stood in took neither "moveStart" nor "moveStop" until something
// ended it; it played an idle while the friend walked off, and the stand-in glided after them. What the AI does for an
// NPC held so is to end the idle ("IdleStop", "turnStop") before walking it off; the stand-in now gets the same, with
// IdleRelease's timing, and its bookkeeping starts over with the graph.
namespace
{
	struct ArmorGraph : Graph
	{
		bool held = true;  // the state the rebuild left it in: neither standing nor moving, as the graph takes it
		int  idleStops = 0;
		int  refused = 0;

		bool Notify(const char* a_event)
		{
			if (held) {
				if (std::strcmp(a_event, "IdleStop") == 0 || std::strcmp(a_event, "turnStop") == 0) {
					held = false;
					++idleStops;
					return true;
				}
				++refused;
				return false;
			}
			return Graph::Notify(a_event);
		}
	};

	struct ArmorPuppet : Puppet
	{
		IdleRelease::Gate release;
	};

	// One frame of the stand-in's update in its frame: DriveAnimation, then ReleaseIdle when the graph refused the start.
	// a_stopsTaken: whether the graph takes the stops (false: held by something they do not reach; the stops are counted).
	void ArmorFrame(ArmorGraph& a_graph, ArmorPuppet& a_puppet, float a_speed, MoveSync::Clock::time_point a_now, bool a_stopsTaken = true)
	{
		const bool  wasMoving = a_puppet.moving;
		const float gaitSpeed = Gait::Smooth(a_puppet.gaitSpeed, a_speed, a_puppet.started ? std::chrono::duration<float>(FRAME).count() : 0.0f);
		const auto  gait = Gait::Pick(gaitSpeed, a_puppet.gait, RUNNING);
		a_graph.SetSync(Gait::SyncValue(gait));
		const auto step = MoveSync::Drive(a_puppet.moving, a_puppet.nextResync, a_speed, a_now, [&](MoveSync::Event a_event) {
			return a_graph.Notify(MoveSync::NamesOf(a_event, false).first);
		});
		if (Gait::Send(step.moving, a_puppet.moving, gait, a_puppet.gait, step.resent != nullptr)) {
			a_graph.Notify(Gait::Event(gait));
			a_graph.Notify(Gait::WeaponEvent(gait));
		}
		a_puppet.moving = step.moving;
		a_puppet.nextResync = step.nextResync;
		a_puppet.gait = gait;
		a_puppet.gaitSpeed = gaitSpeed;
		a_puppet.started = true;
		a_graph.Update();
		// Puppets::UpdateHook: a standing puppet asked to move whose graph refused gets ReleaseIdle, which stops the idle
		// as IdleRelease says.
		if (!wasMoving && a_speed > MoveSync::START && a_puppet.release.Due(!step.moving, a_now)) {
			if (a_stopsTaken) {
				a_graph.Notify("IdleStop");
				a_graph.Notify("turnStop");
			} else {
				++a_graph.idleStops;
			}
		}
	}

	// Puppets::GraphRebuilt: what RemotePlayers does as the power armor skeleton is found on the stand-in.
	void Rebuilt(ArmorPuppet& a_puppet)
	{
		a_puppet.moving = false;
		a_puppet.nextResync = {};
		a_puppet.release = {};
	}
}

TEST("gait: a stand-in whose power armor graph refuses to move is freed as a mirror is, and runs on the next frame")
{
	// The friend got in standing (the stand-in's old graph was told to stop) and walks off once the suit closes.
	ArmorGraph  graph;
	ArmorPuppet puppet;
	auto        now = t0;
	Rebuilt(puppet);
	ArmorFrame(graph, puppet, 0.0f, now += FRAME);
	CHECK(graph.idleStops == 0);  // standing: nothing to free
	ArmorFrame(graph, puppet, 312.0f, now += FRAME);
	CHECK(graph.idleStops == 1);  // the start refused: the idle stopped the same frame
	ArmorFrame(graph, puppet, 312.0f, now += FRAME);
	CHECK(graph.moving);
	CHECK(graph.gait == RUN);
	CHECK(graph.idleStops == 1);
	for (int i = 0; i < 60; ++i) {
		ArmorFrame(graph, puppet, 312.0f, now += FRAME);
	}
	CHECK(graph.moving);
	CHECK(graph.idleStops == 1);
	CHECK(graph.stops == 0);
}

TEST("gait: a stand-in switched in mid-walk starts its fresh graph at once, not half a second on by the resync")
{
	// A fresh stand-in placed while the friend walks: told to move before the switch-in, and its bookkeeping kept
	// moving=true over the rebuild. The soak's anim line: moving=true, Speed=312, iSyncIdleLocomotion=1, legs still.
	ArmorGraph  stale;
	ArmorPuppet keptMoving;
	keptMoving.moving = true;
	keptMoving.started = true;
	auto now = t0;
	keptMoving.nextResync = now + MoveSync::RESYNC;
	for (int i = 0; i < 48; ++i) {  // 3 s
		ArmorFrame(stale, keptMoving, 312.0f, now += FRAME);
	}
	CHECK(!stale.moving);         // the resync's "moveStart" is refused by the held graph, and nothing frees it
	CHECK(stale.idleStops == 0);  // a puppet believed moving is never released
	// With the bookkeeping started over as the skeleton comes: the start goes on the first frame, is refused, the idle
	// is stopped, and the next frame runs.
	ArmorGraph  fresh;
	ArmorPuppet puppet;
	puppet.moving = true;
	puppet.started = true;
	now = t0;
	puppet.nextResync = now + MoveSync::RESYNC;
	Rebuilt(puppet);
	ArmorFrame(fresh, puppet, 312.0f, now += FRAME);
	ArmorFrame(fresh, puppet, 312.0f, now += FRAME);
	CHECK(fresh.moving);
	CHECK(fresh.gait == RUN);
	CHECK(fresh.idleStops == 1);
}

TEST("gait: a power armor graph the stops do not reach gets two in the time the still legs take to place the stand-in again")
{
	// Held by something else: the stops are refused, come less and less often (IdleRelease), and the stand-in is placed
	// again for its still legs (ArmorFrames::STILL_LIMIT) with two sent, never every frame.
	ArmorGraph  graph;
	ArmorPuppet puppet;
	auto        now = t0;
	Rebuilt(puppet);
	int frames = 0;
	for (auto t = std::chrono::milliseconds(0); t < ArmorFrames::STILL_LIMIT; t += FRAME, ++frames) {
		ArmorFrame(graph, puppet, 312.0f, now += FRAME, false);
	}
	CHECK(!graph.moving);
	CHECK(graph.idleStops == 2);
	CHECK(graph.refused >= frames);  // the start every frame, as before
	CHECK(frames >= 30);
}
