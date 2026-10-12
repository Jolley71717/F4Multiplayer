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

		void Frame(Graph& a_graph, float a_speed, MoveSync::Clock::time_point a_now)
		{
			const auto step = MoveSync::Drive(moving, nextResync, a_speed, a_now, a_graph);
			moving = step.moving;
			nextResync = step.nextResync;
			resent += step.resent != nullptr;
		}

		// Frames from a_from for a_for at a_speed; returns the end time.
		MoveSync::Clock::time_point Run(Graph& a_graph, float a_speed, MoveSync::Clock::time_point a_from, MoveSync::Clock::duration a_for)
		{
			auto t = a_from;
			for (; t < a_from + a_for; t += FRAME) {
				Frame(a_graph, a_speed, t);
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

TEST("move sync: a puppet walking while its owner's copy runs is stopped and started again")
{
	// It walks (graph) while we think it stands, so every start is refused and Speed stays 0: a walk at
	// a walk's pace while its owner's copy ran. A stand-in back in its walk, or a mirror its own AI had
	// walking when mirroring began (every puppet starts out standing, as far as we know). A stop it takes
	// shows that, and a start puts it right, on the first frame.
	Graph  graph{ .walking = true };
	Puppet puppet;
	puppet.Frame(graph, 300.0f, t0);
	CHECK(puppet.moving && graph.walking);
	CHECK(graph.stops == 1 && graph.starts == 2);
	CHECK(puppet.resent == 1);
}

TEST("move sync: a refused start is tested with a stop no more than every RESYNC")
{
	// Mid-jump, or held by an idle (a dog sitting, which IdleRelease deals with): it takes neither, so the
	// test changes nothing.
	Graph  graph{ .jumping = true };
	Puppet puppet;
	puppet.Run(graph, 300.0f, t0, 2s);
	CHECK(!puppet.moving && !graph.walking);
	CHECK(graph.starts >= 100);                   // asked every frame
	CHECK(graph.stops >= 4 && graph.stops <= 5);  // tested every 500 ms
	CHECK(puppet.resent == 0);
}

TEST("move sync: a resend that puts the graph right says what it sent")
{
	Graph      graph{ .walking = true };
	const auto step = MoveSync::Drive(false, t0, 0.0f, t0, graph);
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
