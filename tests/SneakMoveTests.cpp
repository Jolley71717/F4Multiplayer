#include "Test.h"

#include "game/Gait.h"
#include "game/MoveSync.h"

#include <cstring>

// A friend who sneaks and moves, driven frame by frame the way Puppets::DriveAnimation does (MoveSync::NamesOf, the
// sneak's gait events), against a model of the humanoid graph's two sneak locomotions as the game's files have them:
//  - weapon away (MTBehavior's MTSneakIdleLocomotion_SM): the sneak idle sets off only on "moveStartAnimated", a
//    wildcard that also restarts the set-off clip from the sneak's locomotion; its "moveStart" is held by a condition
//    that never holds; "moveStop" stops it;
//  - weapon drawn (WeaponBehavior's WPNSneakReady_SM): "moveStart" and "moveStop", walk or run by the start state
//    iSyncSneakWalkRun and, moving, "sneakWalkStart"/"sneakRunStart";
//  - "sneakStart"/"sneakStop" go between standing and sneaking, moving or not as iSyncIdleLocomotion says.
namespace
{
	using namespace std::chrono_literals;

	const auto     t0 = MoveSync::Clock::time_point{} + 1h;
	constexpr auto FRAME = 16ms;
	constexpr float SNEAK_WALK = 90.0f;
	constexpr float SNEAK_RUN = 250.0f;

	struct Graph
	{
		bool         drawn = false;
		bool         sneak = false;
		bool         moving = false;
		int          sneakGait = 0;      // the weapon-drawn sneak's: 0 walk, 1 run
		std::int32_t idleLocomotion = 0;  // iSyncIdleLocomotion
		std::int32_t sneakWalkRun = 0;    // iSyncSneakWalkRun
		int          setOffs = 0;         // set-off clips played (and restarted)

		bool Is(const char* a_event, const char* a_name) { return std::strcmp(a_event, a_name) == 0; }

		bool Notify(const char* a_event)
		{
			const bool sneakAway = sneak && !drawn;
			if (Is(a_event, "moveStartAnimated")) {
				if (!sneakAway) {
					return false;  // nobody's outside the weapon-away sneak
				}
				moving = true;  // from the idle, or again from the locomotion (a wildcard)
				++setOffs;
				return true;
			}
			if (Is(a_event, "moveStart")) {
				if (sneakAway || moving) {
					return false;
				}
				moving = true;
				sneakGait = sneakWalkRun;
				++setOffs;
				return true;
			}
			if (Is(a_event, "moveStop")) {
				if (!moving) {
					return false;
				}
				moving = false;
				return true;
			}
			if (Is(a_event, "sneakStart") || Is(a_event, "sneakStop")) {
				const bool to = Is(a_event, "sneakStart");
				if (sneak == to) {
					return false;
				}
				sneak = to;
				moving = idleLocomotion == 1;  // the other version, entered moving or standing by the variable
				sneakGait = sneakWalkRun;
				return true;
			}
			if (Is(a_event, "sneakWalkStart") || Is(a_event, "sneakRunStart")) {
				if (!(sneak && drawn && moving)) {
					return false;
				}
				sneakGait = Is(a_event, "sneakRunStart") ? 1 : 0;
				return true;
			}
			return false;  // the standing gaits' events: not modelled
		}
	};

	struct Puppet
	{
		bool                        moving = false;
		MoveSync::Clock::time_point nextResync{};
		Gait::Kind                  gait = Gait::Kind::kWalk;
		bool                        sneaking = false;  // the sneak last sent
	};

	// One DriveAnimation frame for a humanoid, in its order.
	void Frame(Graph& a_graph, Puppet& a_puppet, float a_speed, bool a_sneaking, MoveSync::Clock::time_point a_now)
	{
		const auto gait = Gait::Pick(a_speed, a_puppet.gait);
		a_graph.sneakWalkRun = Gait::WeaponState(gait);
		const auto step = MoveSync::Drive(a_puppet.moving, a_puppet.nextResync, a_speed, a_now, [&](MoveSync::Event a_event) {
			const auto names = MoveSync::NamesOf(a_event, a_sneaking);
			bool       taken = a_graph.Notify(names.first);
			if (names.second) {
				taken = a_graph.Notify(names.second) || taken;
			}
			return taken;
		});
		if (Gait::Send(step.moving, a_puppet.moving, gait, a_puppet.gait, step.resent != nullptr)) {
			a_graph.Notify(Gait::Event(gait));
			a_graph.Notify(Gait::WeaponEvent(gait));
			if (a_sneaking) {
				a_graph.Notify(Gait::SneakEvent(gait));
			}
		}
		a_graph.idleLocomotion = step.moving ? 1 : 0;
		if (a_sneaking != a_puppet.sneaking) {
			a_graph.Notify(a_sneaking ? "sneakStart" : "sneakStop");
		}
		a_puppet.moving = step.moving;
		a_puppet.nextResync = step.nextResync;
		a_puppet.gait = gait;
		a_puppet.sneaking = a_sneaking;
	}

	MoveSync::Clock::time_point Run(Graph& a_graph, Puppet& a_puppet, float a_speed, bool a_sneaking, MoveSync::Clock::time_point a_from, MoveSync::Clock::duration a_for)
	{
		auto t = a_from;
		for (; t < a_from + a_for; t += FRAME) {
			Frame(a_graph, a_puppet, a_speed, a_sneaking, t);
		}
		return t;
	}
}

TEST("sneak move: the weapon-away sneak idle refuses a bare moveStart, so the friend's stand-in slid along crouched")
{
	Graph graph{ .sneak = true };
	CHECK(!graph.Notify("moveStart"));
	CHECK(!graph.moving);
	CHECK(graph.Notify("moveStartAnimated"));
	CHECK(graph.moving);
}

TEST("sneak move: a friend crouched with their weapon away who walks sets off in the sneak walk at once, once, and stops")
{
	Graph  graph;
	Puppet puppet;
	auto   t = Run(graph, puppet, 0.0f, true, t0, 1s);  // crouches standing
	REQUIRE(graph.sneak && !graph.moving);
	Frame(graph, puppet, SNEAK_WALK, true, t += FRAME);
	CHECK(graph.moving && puppet.moving);
	t = Run(graph, puppet, SNEAK_WALK, true, t, 3s);
	CHECK(graph.moving);
	CHECK(graph.setOffs == 1);  // told again every RESYNC with "moveStart", which the sneak's walk refuses
	t = Run(graph, puppet, 0.0f, true, t, 200ms);
	CHECK(!graph.moving && graph.sneak);
	// And off again.
	Run(graph, puppet, SNEAK_RUN, true, t, 200ms);
	CHECK(graph.moving);
	CHECK(graph.setOffs == 2);
}

TEST("sneak move: a friend crouched with their weapon drawn sets off on moveStart, in the walk or the run")
{
	Graph  graph{ .drawn = true };
	Puppet puppet;
	auto   t = Run(graph, puppet, 0.0f, true, t0, 500ms);
	REQUIRE(graph.sneak);
	t = Run(graph, puppet, SNEAK_WALK, true, t, 2s);
	CHECK(graph.moving && graph.sneakGait == 0);
	CHECK(graph.setOffs == 1);
	t = Run(graph, puppet, SNEAK_RUN, true, t, 1s);  // speeds up into the sneak's run
	CHECK(graph.moving && graph.sneakGait == 1);
	t = Run(graph, puppet, 0.0f, true, t, 200ms);
	CHECK(!graph.moving);
	Run(graph, puppet, SNEAK_RUN, true, t, 200ms);  // a run's start enters the run by the sync variable
	CHECK(graph.moving && graph.sneakGait == 1);
}

TEST("sneak move: crouching and standing up while moving keep the legs moving, either way, weapon away or drawn")
{
	for (const bool drawn : { false, true }) {
		Graph  graph{ .drawn = drawn };
		Puppet puppet;
		auto   t = Run(graph, puppet, 300.0f, false, t0, 500ms);
		REQUIRE(graph.moving && !graph.sneak);
		t = Run(graph, puppet, SNEAK_RUN, true, t, 1s);
		CHECK(graph.sneak && graph.moving);
		t = Run(graph, puppet, 300.0f, false, t, 1s);
		CHECK(!graph.sneak && graph.moving);
		Run(graph, puppet, 0.0f, false, t, 200ms);
		CHECK(!graph.moving);
	}
}

TEST("sneak move: crouching standing still, then setting off, then standing up while still")
{
	Graph  graph;
	Puppet puppet;
	auto   t = Run(graph, puppet, 0.0f, true, t0, 500ms);
	t = Run(graph, puppet, SNEAK_WALK, true, t, 500ms);
	CHECK(graph.moving);
	t = Run(graph, puppet, 0.0f, true, t, 500ms);
	t = Run(graph, puppet, 0.0f, false, t, 500ms);
	CHECK(!graph.sneak && !graph.moving);
	Run(graph, puppet, 120.0f, false, t, 500ms);  // a standing walk sets off on moveStart as before
	CHECK(graph.moving);
}

TEST("sneak move: the events each of MoveSync's asks for")
{
	const auto start = MoveSync::NamesOf(MoveSync::Event::kStart, false);
	CHECK(std::strcmp(start.first, "moveStart") == 0);
	CHECK(start.second == nullptr);
	const auto sneakStart = MoveSync::NamesOf(MoveSync::Event::kStart, true);
	CHECK(std::strcmp(sneakStart.first, "moveStartAnimated") == 0);
	REQUIRE(sneakStart.second != nullptr);
	CHECK(std::strcmp(sneakStart.second, "moveStart") == 0);
	for (const bool sneaking : { false, true }) {
		const auto still = MoveSync::NamesOf(MoveSync::Event::kStillMoving, sneaking);
		CHECK(std::strcmp(still.first, "moveStart") == 0);
		CHECK(still.second == nullptr);
		const auto stop = MoveSync::NamesOf(MoveSync::Event::kStop, sneaking);
		CHECK(std::strcmp(stop.first, "moveStop") == 0);
		CHECK(stop.second == nullptr);
	}
	CHECK(MoveSync::Starts(MoveSync::Event::kStart));
	CHECK(MoveSync::Starts(MoveSync::Event::kStillMoving));
	CHECK(!MoveSync::Starts(MoveSync::Event::kStop));
	CHECK(std::strcmp(Gait::SneakEvent(Gait::Kind::kWalk), "sneakWalkStart") == 0);
	CHECK(std::strcmp(Gait::SneakEvent(Gait::Kind::kRun), "sneakRunStart") == 0);
}
