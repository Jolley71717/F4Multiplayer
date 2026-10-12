#include "Test.h"

#include "game/MoveSync.h"

#include <cstring>

// Whether a puppet's graph is told to start or stop moving, against a graph that, like the game's
// locomotion graph, takes "moveStart" only while standing and "moveStop" only while moving, and neither
// in the middle of a jump.
namespace
{
	using namespace std::chrono_literals;

	const auto     t0 = MoveSync::Clock::time_point{} + 1h;
	constexpr auto FRAME = 16ms;

	struct Graph
	{
		bool walking = false;  // in its locomotion state
		bool jumping = false;  // takes neither event
		int  starts = 0;       // events sent
		int  stops = 0;

		bool operator()(MoveSync::Event a_event)
		{
			const bool start = a_event == MoveSync::Event::kStart;
			++(start ? starts : stops);
			if (jumping || walking == start) {
				return false;
			}
			walking = start;
			return true;
		}
	};

	struct Puppet
	{
		bool                        moving = false;
		MoveSync::Clock::time_point nextResync{};
		int                         resent = 0;  // resends the graph took

		void Frame(Graph& a_graph, float a_speed, MoveSync::Clock::time_point a_now, bool a_probe = true)
		{
			const auto step = MoveSync::Drive(moving, nextResync, a_speed, a_probe, a_now, a_graph);
			moving = step.moving;
			nextResync = step.nextResync;
			resent += step.resent != nullptr;
		}

		// Frames from a_from for a_for at a_speed; returns the end time.
		MoveSync::Clock::time_point Run(Graph& a_graph, float a_speed, MoveSync::Clock::time_point a_from, MoveSync::Clock::duration a_for, bool a_probe = true)
		{
			auto t = a_from;
			for (; t < a_from + a_for; t += FRAME) {
				Frame(a_graph, a_speed, t, a_probe);
			}
			return t;
		}
	};
}

TEST("move sync: a puppet starts above START and stops below STOP")
{
	Graph  graph;
	Puppet puppet;
	auto   t = puppet.Run(graph, 0.0f, t0, 100ms);
	CHECK(!graph.walking && !puppet.moving);
	t = puppet.Run(graph, MoveSync::START + 1.0f, t, 100ms);
	CHECK(graph.walking && puppet.moving);
	t = puppet.Run(graph, (MoveSync::START + MoveSync::STOP) / 2.0f, t, 100ms);  // slowing, not stopped
	CHECK(graph.walking && puppet.moving);
	puppet.Run(graph, 0.0f, t, 100ms);
	CHECK(!graph.walking && !puppet.moving);
	CHECK(puppet.resent == 0);
}

TEST("move sync: a graph found walking while its owner's copy stands is stopped within RESYNC")
{
	// Back from a jump or a stagger in its locomotion state, and no stop was ever sent again: the friend
	// stood still and their stand-in walked on the spot.
	Graph  graph{ .walking = true };
	Puppet puppet{ .nextResync = t0 + 1s };
	auto   t = puppet.Run(graph, 0.0f, t0, 1s);
	CHECK(graph.walking);  // not yet due
	t = puppet.Run(graph, 0.0f, t, MoveSync::RESYNC + FRAME);
	CHECK(!graph.walking && !puppet.moving);
	CHECK(puppet.resent == 1);
}

TEST("move sync: a stop refused mid-jump is sent again after the landing")
{
	Graph  graph;
	Puppet puppet;
	auto   t = puppet.Run(graph, 300.0f, t0, 100ms);
	REQUIRE(graph.walking);
	graph.jumping = true;  // the friend jumps and lets go of the stick in the air
	t = puppet.Run(graph, 0.0f, t, 300ms);
	CHECK(!puppet.moving && graph.walking);  // told to stop, refused
	graph.jumping = false;                   // landed, still walking
	t = puppet.Run(graph, 0.0f, t, MoveSync::RESYNC + FRAME);
	CHECK(!graph.walking);
	CHECK(puppet.resent == 1);
}

TEST("move sync: a graph in step is told again only every RESYNC, and refuses")
{
	Graph  graph;
	Puppet puppet;
	puppet.Run(graph, 0.0f, t0, 2s);
	CHECK(graph.starts == 0);
	CHECK(graph.stops >= 4 && graph.stops <= 5);  // 2 s of standing: about one every 500 ms
	CHECK(puppet.resent == 0);
}

TEST("move sync: a stand-in walking while its friend runs is stopped and started again")
{
	// It walks (graph) while we think it stands, so every start is refused and Speed stays 0: a walk at
	// a walk's pace while its friend ran. A stop it takes shows that, and a start puts it right.
	Graph  graph{ .walking = true };
	Puppet puppet;
	puppet.Frame(graph, 300.0f, t0);
	CHECK(puppet.moving && graph.walking);
	CHECK(graph.stops == 1 && graph.starts == 2);
	CHECK(puppet.resent == 1);
}

TEST("move sync: a refused start is tested with a stop no more than every RESYNC")
{
	Graph  graph{ .jumping = true };  // takes nothing for a while
	Puppet puppet;
	puppet.Run(graph, 300.0f, t0, 2s);
	CHECK(!puppet.moving);
	CHECK(graph.starts >= 100);                   // asked every frame
	CHECK(graph.stops >= 4 && graph.stops <= 5);  // tested every 500 ms
	CHECK(puppet.resent == 0);
}

TEST("move sync: a mirrored NPC's refused start is not tested with a stop")
{
	// Its refusal is an idle holding it (a dog sitting), which IdleRelease deals with.
	Graph  graph{ .jumping = true };
	Puppet puppet;
	puppet.Run(graph, 300.0f, t0, 2s, false);
	CHECK(graph.stops == 0);
	CHECK(!puppet.moving);
}

TEST("move sync: a resend that puts the graph right says what it sent")
{
	Graph      graph{ .walking = true };
	const auto step = MoveSync::Drive(false, t0, 0.0f, true, t0, graph);
	REQUIRE(step.resent != nullptr);
	CHECK(std::strcmp(step.resent, "moveStop") == 0);
	CHECK(step.nextResync == t0 + MoveSync::RESYNC);
}

TEST("move sync: something that keeps happening is logged the first few times, then every 50th")
{
	CHECK(MoveSync::Loggable(1));
	CHECK(MoveSync::Loggable(5));
	CHECK(!MoveSync::Loggable(6));
	CHECK(!MoveSync::Loggable(49));
	CHECK(MoveSync::Loggable(50));
	CHECK(!MoveSync::Loggable(51));
	CHECK(MoveSync::Loggable(100));
}
