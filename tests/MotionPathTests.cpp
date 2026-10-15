#include "Test.h"

#include "Protocol.h"
#include "game/MotionPath.h"

#include <cmath>
#include <deque>

// Where a puppet is put at the moment the engine updates it (src/game/MotionPath.h): our frame's work samples it and
// hands over the path ahead, and the update, which comes at another moment, puts it where the path is then.
namespace
{
	using namespace std::chrono_literals;
	using MotionPath::Clock;

	const auto start = Clock::time_point{} + 1h;

	// As NpcSync keeps a mirrored NPC's states.
	struct Received
	{
		Clock::time_point    received;
		Protocol::ActorState state;
	};

	Protocol::ActorState At(float a_x, std::uint32_t a_worldspace = 0x3C)
	{
		Protocol::ActorState state;
		state.worldspace = a_worldspace;
		state.x = a_x;
		state.heading = 1.0f;
		return state;
	}

	bool Near(float a_a, float a_b)
	{
		return std::abs(a_a - a_b) < 0.01f;
	}
}

TEST("motion path: an update between two points puts it between them, by the time")
{
	MotionPath::Path path;
	path.Add({ start, 0.0f, 0.0f, 0.0f, 0.0f });
	path.Add({ start + 100ms, 40.0f, 20.0f, 10.0f, 0.5f });
	path.Add({ start + 200ms, 80.0f, 20.0f, 10.0f, 0.5f });
	const auto quarter = path.At(start + 25ms);
	REQUIRE(quarter);
	CHECK(Near(quarter->x, 10.0f));
	CHECK(Near(quarter->y, 5.0f));
	CHECK(Near(quarter->z, 2.5f));
	CHECK(Near(quarter->heading, 0.125f));
	const auto later = path.At(start + 150ms);
	REQUIRE(later);
	CHECK(Near(later->x, 60.0f));
	CHECK(Near(later->y, 20.0f));
}

TEST("motion path: before it starts it is at its first point, and once it has run out at its last")
{
	MotionPath::Path path;
	CHECK(!path.At(start));
	path.Add({ start, 5.0f, 0.0f, 0.0f, 0.0f });
	path.Add({ start + 50ms, 25.0f, 0.0f, 0.0f, 0.0f });
	CHECK(Near(path.At(start - 3ms)->x, 5.0f));  // the update ran a moment before our work sampled it
	CHECK(Near(path.At(start + 80ms)->x, 25.0f));  // no newer state: it stands where the last one put it
}

TEST("motion path: a heading blended across north turns the short way")
{
	MotionPath::Path path;
	path.Add({ start, 0.0f, 0.0f, 0.0f, 6.2f });
	path.Add({ start + 100ms, 0.0f, 0.0f, 0.0f, 0.1f });
	const auto half = path.At(start + 50ms);
	REQUIRE(half);
	// From 6.2 to 0.1 is a turn of about +0.183, not -6.1.
	CHECK(std::abs(half->heading - (6.2f + 0.0916f)) < 0.01f);
}

TEST("motion path: a point no later than the last is left out, and a full path takes no more")
{
	MotionPath::Path path;
	CHECK(path.Add({ start, 0.0f, 0.0f, 0.0f, 0.0f }));
	CHECK(path.Add({ start, 9.0f, 0.0f, 0.0f, 0.0f }));
	CHECK(path.count == 1);
	for (int i = 1; i < static_cast<int>(MotionPath::POINTS); ++i) {
		CHECK(path.Add({ start + i * 10ms, static_cast<float>(i), 0.0f, 0.0f, 0.0f }));
	}
	CHECK(path.count == MotionPath::POINTS);
	CHECK(!path.Add({ start + 1s, 99.0f, 0.0f, 0.0f, 0.0f }));
	CHECK(Near(path.At(start + 1s)->x, static_cast<float>(MotionPath::POINTS - 1)));
}

TEST("motion path: a mirrored NPC's path goes on through the states its owner sent, each a delay after it came, and stops at a door")
{
	// Decoded as the owner's game sent them, every 50 ms; the mirror is drawn 100 ms behind.
	std::vector<Protocol::ActorState> sent;
	for (int i = 0; i < 6; ++i) {
		auto state = At(static_cast<float>(i) * 20.0f);
		state.refId = 0x1D162;
		sent.push_back(state);
	}
	sent[5] = At(500.0f, 0);  // through a door into an interior
	sent[5].cell = 0x1234;
	const auto decoded = Protocol::DecodeActorStates(Protocol::Encode(sent, Protocol::MessageType::kActorStatesRelay));
	REQUIRE(decoded);
	REQUIRE(decoded->size() == sent.size());
	std::deque<Received> snapshots;
	for (std::size_t i = 0; i < decoded->size(); ++i) {
		snapshots.push_back({ start + static_cast<int>(i) * 50ms, (*decoded)[i] });
	}

	// Our work samples at 175 ms, which draws the moment 75 ms: halfway from the second state to the third.
	const auto now = start + 175ms;
	const auto path = MotionPath::Ahead({ now, 30.0f, 0.0f, 0.0f, 1.0f }, 0, 0x3C, snapshots, 1, 100ms, &Received::received);
	// The second state was drawn already; the third to the fifth are ahead; the sixth is in another space.
	CHECK(path.count == 4);
	// An update a whole frame after our work (it ran early in the frame before) finds it 25 ms further on.
	const auto later = path.At(now + 25ms);
	REQUIRE(later);
	CHECK(Near(later->x, 40.0f));
	// Our work missing a frame of 62 ms: the update still finds it where it is at that moment.
	CHECK(Near(path.At(now + 62ms)->x, 54.8f));
	// Past the last state in its space it stands there.
	CHECK(Near(path.At(now + 1s)->x, 80.0f));
}
