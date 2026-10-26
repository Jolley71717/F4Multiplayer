#include "Test.h"

#include "Protocol.h"
#include "game/StandInSpace.h"
#include "game/StateBuffer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

// Where a friend's stand-in is drawn while they stand where we do. Their game samples them on Session's schedule, the
// server relays each state in a PlayerStates batch (encoded and decoded as on the wire), our game reads what has come
// once a frame, samples it from StateBuffer and puts the stand-in where StandInSpace::Place says, as RemotePlayers does.
namespace
{
	using namespace std::chrono_literals;
	using StateBuffer::Clock;

	constexpr float pi = std::numbers::pi_v<float>;
	constexpr std::uint32_t FRIEND_ID = 2;

	const auto start = Clock::time_point{} + 1h;

	// Where they are at a moment (seconds since the start): x, y, z, heading and speed.
	struct Pose
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float heading = 0.0f;
		float speed = 0.0f;
	};

	struct Drawn
	{
		float               seconds = 0.0f;  // since the start
		StandInSpace::Point friendAt;         // where their state put them
		StandInSpace::Point standIn;          // where their stand-in is drawn
	};

	// Every frame drawn, with us standing at a_us facing a_ourHeading (north unless said). Their game sends every 33 ms;
	// each batch takes 40 ms to come.
	std::vector<Drawn> Play(const std::function<Pose(float)>& a_friend, const StandInSpace::Point& a_us, Clock::duration a_length, float a_clear = StandInSpace::CLEAR, float a_ourHeading = 0.0f)
	{
		struct Packet
		{
			Clock::time_point         arrives;
			std::vector<std::uint8_t> bytes;
		};
		std::vector<Packet> packets;
		std::uint32_t       sequence = 0;
		for (auto now = start; now < start + a_length; now += 33ms) {
			const auto pose = a_friend(std::chrono::duration<float>(now - start).count());
			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.worldspace = 0x3C;
			state.x = pose.x;
			state.y = pose.y;
			state.z = pose.z;
			state.heading = pose.heading;
			state.speed = pose.speed;
			state.sentMs = Protocol::SentMs(now);
			packets.push_back({ now + 40ms, Protocol::Encode(Protocol::PlayerStates{ sequence, { { FRIEND_ID, state } } }) });
		}

		std::vector<Drawn>  drawn;
		StateBuffer::Buffer buffer;
		StandInSpace::Away  away;
		std::size_t         next = 0;
		for (auto now = start; now < start + a_length; now += 16ms) {
			while (next < packets.size() && packets[next].arrives <= now) {
				const auto batch = Protocol::DecodePlayerStates(packets[next++].bytes);
				REQUIRE(batch);
				for (const auto& entry : batch->players) {
					buffer.Push(entry.state, now);
				}
			}
			buffer.Frame(now);
			if (const auto sampled = buffer.Sample(now)) {
				const StandInSpace::Point at{ sampled->state.x, sampled->state.y, sampled->state.z };
				drawn.push_back({ std::chrono::duration<float>(now - start).count(), at, StandInSpace::Place(away, at, sampled->state.heading, a_us, a_ourHeading, a_clear) });
			}
		}
		return drawn;
	}

	float Ground(const StandInSpace::Point& a_from, const StandInSpace::Point& a_to)
	{
		return std::hypot(a_to.x - a_from.x, a_to.y - a_from.y);
	}

	bool Same(const StandInSpace::Point& a_one, const StandInSpace::Point& a_two)
	{
		return a_one.x == a_two.x && a_one.y == a_two.y && a_one.z == a_two.z;
	}
}

TEST("stand-in space: a friend who moves to our very spot is drawn a body's room to our right, not through us")
{
	// We face north. They stand 600 units off facing east, then a moveto puts them on our spot (as onto our stand-in
	// while we were down).
	const StandInSpace::Point us{ 1000.0f, 2000.0f, 50.0f };
	const auto drawn = Play([&](float a_seconds) {
		return a_seconds < 2.0f ? Pose{ us.x - 600.0f, us.y, us.z, pi / 2.0f } : Pose{ us.x, us.y, us.z, pi / 2.0f };
	},
		us, 5s);
	REQUIRE(drawn.size() > 200);

	int onUs = 0;
	for (const auto& frame : drawn) {
		CHECK(Ground(us, frame.standIn) >= StandInSpace::CLEAR - 0.01f);
		onUs += Ground(us, frame.friendAt) < 1.0f;
		if (Ground(us, frame.friendAt) >= StandInSpace::CLEAR) {
			CHECK(Same(frame.friendAt, frame.standIn));  // anywhere else they are drawn where they are
		}
	}
	CHECK(onUs > 100);

	// Standing on our spot it holds still, a step to our right (east), at their height: it does not jitter about us.
	const auto& last = drawn.back();
	CHECK(std::abs(last.standIn.x - (us.x + StandInSpace::CLEAR)) < 0.01f);
	CHECK(std::abs(last.standIn.y - us.y) < 0.01f);
	CHECK(last.standIn.z == us.z);
	for (std::size_t i = drawn.size() - 60; i < drawn.size(); ++i) {
		CHECK(Same(drawn[i].standIn, last.standIn));
	}
}

TEST("stand-in space: a friend on our very spot who looks about does not take their stand-in round us")
{
	// We face north. A moveto onto our spot facing west, then they turn a full circle where they stand, and walk off north.
	const StandInSpace::Point us{ -400.0f, 250.0f, 10.0f };
	const auto drawn = Play([&](float a_seconds) {
		if (a_seconds < 1.0f) {
			return Pose{ us.x + 500.0f, us.y, us.z, -pi / 2.0f };
		}
		if (a_seconds < 4.5f) {
			return Pose{ us.x, us.y, us.z, -pi / 2.0f + (std::max)(a_seconds - 1.5f, 0.0f) * 2.0f * pi / 3.0f };
		}
		return Pose{ us.x, us.y + 200.0f * (a_seconds - 4.5f), us.z, 0.0f, 200.0f };
	},
		us, 6s);
	REQUIRE(drawn.size() > 300);

	// While they turn on our spot it stays a step east of us, to our right as they came, not the way they face.
	int turning = 0;
	for (const auto& frame : drawn) {
		CHECK(Ground(us, frame.standIn) >= StandInSpace::CLEAR - 0.01f);
		if (frame.seconds > 1.2f && frame.seconds < 4.5f) {
			++turning;
			CHECK(std::abs(frame.standIn.x - (us.x + StandInSpace::CLEAR)) < 0.01f);
			CHECK(std::abs(frame.standIn.y - us.y) < 0.01f);
		}
	}
	CHECK(turning > 150);
	// Walked off, they are drawn on their own path again.
	CHECK(Same(drawn.back().friendAt, drawn.back().standIn));

	// Back on our spot as we face west, the step is north, our right then: what is kept is where this arrival put it,
	// not the last one.
	StandInSpace::Away away;
	const auto south = StandInSpace::Place(away, us, pi / 2.0f, us, pi / 2.0f);  // we face east
	CHECK(south.y < us.y);
	CHECK(Same(StandInSpace::Place(away, { us.x, us.y + 300.0f, us.z }, 0.0f, us, pi / 2.0f), { us.x, us.y + 300.0f, us.z }));
	const auto north = StandInSpace::Place(away, us, pi / 2.0f, us, -pi / 2.0f);
	CHECK(std::abs(north.x - us.x) < 0.01f);
	CHECK(north.y > us.y);
	// Turning on the spot after does not take it round us either.
	CHECK(Same(StandInSpace::Place(away, us, pi / 2.0f, us, pi), north));
}

TEST("stand-in space: a friend who comes onto our marker facing our way is not drawn in the way we walk")
{
	// Both of us joined onto one marker, facing east (hidq): their stand-in went a step ahead of us, where AutoMove goes.
	const StandInSpace::Point us{ 2500.0f, -700.0f, 120.0f };
	const auto drawn = Play([&](float a_seconds) {
		return a_seconds < 1.0f ? Pose{ us.x, us.y + 3000.0f, us.z, pi / 2.0f } : Pose{ us.x, us.y, us.z, pi / 2.0f };
	},
		us, 3s, StandInSpace::CLEAR, pi / 2.0f);
	REQUIRE(drawn.size() > 100);
	int onUs = 0;
	for (const auto& frame : drawn) {
		if (Ground(us, frame.friendAt) < StandInSpace::SAME) {
			++onUs;
			CHECK(frame.standIn.x - us.x < 0.01f);  // never ahead of us
			CHECK(std::abs(frame.standIn.x - us.x) < 0.01f);
			CHECK(std::abs(frame.standIn.y - (us.y - StandInSpace::CLEAR)) < StandInSpace::SAME);  // to our right: south (from their spot)
		}
	}
	CHECK(onUs > 100);
}

TEST("stand-in space: a stand-in put by us at one place is not put there again when we meet on a spot at another")
{
	// On one marker at A; both of us leave for B unseen (no frame with them off our spot), and arrive on one marker there.
	const StandInSpace::Point a{ 1000.0f, 1000.0f, 0.0f };
	const StandInSpace::Point b{ -30000.0f, 52000.0f, 900.0f };
	StandInSpace::Away        away;
	StandInSpace::Place(away, a, 0.0f, a, 0.0f);
	const auto atB = StandInSpace::Place(away, b, 0.0f, b, 0.0f);
	CHECK(std::abs(Ground(b, atB) - StandInSpace::CLEAR) < 0.01f);
	CHECK(std::abs(atB.x - (b.x + StandInSpace::CLEAR)) < 0.01f);  // to our right there
	CHECK(atB.z == b.z);

	// Into power armor on the spot: the room is widened, on the same side.
	const auto armor = StandInSpace::Place(away, b, 0.0f, b, 0.0f, StandInSpace::CLEAR_ARMOR);
	CHECK(std::abs(Ground(b, armor) - StandInSpace::CLEAR_ARMOR) < 0.01f);
	CHECK(armor.x > b.x);
	// ... and held there after.
	CHECK(Same(StandInSpace::Place(away, b, 0.0f, b, pi, StandInSpace::CLEAR_ARMOR), armor));
}

TEST("stand-in space: as we walk off a friend's spot, their stand-in stays where it was put, not a step ahead of us")
{
	// They stand on our marker facing east, as we do; we walk east off it a little each frame. Kept a step from our
	// feet, it went along ahead of us, and a body there stopped us from ever getting off the spot (hidq).
	const StandInSpace::Point at{ -100.0f, 400.0f, 0.0f };
	StandInSpace::Away        away;
	const auto                first = StandInSpace::Place(away, at, pi / 2.0f, at, pi / 2.0f);
	CHECK(first.x - at.x < 0.01f);
	for (int step = 1; step <= 40; ++step) {
		const StandInSpace::Point us{ at.x + 0.5f * static_cast<float>(step), at.y, at.z };
		const auto                standIn = StandInSpace::Place(away, at, pi / 2.0f, us, pi / 2.0f);
		CHECK(standIn.x - us.x < 0.01f);  // never ahead of us
		if (Ground(us, at) < StandInSpace::SAME) {
			CHECK(Same(standIn, first));  // on the spot, it does not move with us
		} else {
			CHECK(Ground(us, standIn) >= StandInSpace::CLEAR - 0.01f);  // off it, behind us, out of our body
		}
	}
}

TEST("stand-in space: a friend who runs right through us is drawn round us on their side, and on their own path elsewhere")
{
	// Along x through our spot at a run, a little to one side.
	const StandInSpace::Point us{ 0.0f, 0.0f, 0.0f };
	constexpr float           RUN = 400.0f;
	const auto drawn = Play([&](float a_seconds) { return Pose{ -800.0f + RUN * a_seconds, 5.0f, 0.0f, pi / 2.0f, RUN }; }, us, 4s);
	REQUIRE(drawn.size() > 200);

	int moved = 0;
	for (const auto& frame : drawn) {
		CHECK(Ground(us, frame.standIn) >= StandInSpace::CLEAR - 0.01f);
		const bool clear = Ground(us, frame.friendAt) >= StandInSpace::CLEAR;
		moved += !clear;
		if (clear) {
			CHECK(Same(frame.friendAt, frame.standIn));
		}
		// Going round us it stays on their side of us: straight out from us toward them.
		const float along = (frame.standIn.x - us.x) * (frame.friendAt.x - us.x) + (frame.standIn.y - us.y) * (frame.friendAt.y - us.y);
		CHECK(along > 0.0f);
		CHECK(frame.standIn.z == frame.friendAt.z);
	}
	CHECK(moved > 5);  // about 0.2 s of frames go round us
	CHECK(moved < 30);
}

TEST("stand-in space: a friend on the floor above or below us is drawn where they are")
{
	const StandInSpace::Point us{ 300.0f, 300.0f, 0.0f };
	for (const float z : { StandInSpace::TALL + 10.0f, -(StandInSpace::TALL + 10.0f) }) {
		const auto drawn = Play([&](float) { return Pose{ us.x + 3.0f, us.y, z, 0.0f }; }, us, 2s);
		REQUIRE(!drawn.empty());
		for (const auto& frame : drawn) {
			CHECK(Same(frame.friendAt, frame.standIn));
		}
	}
	// On a step just above us they are on our floor: moved.
	const auto drawn = Play([&](float) { return Pose{ us.x + 3.0f, us.y, 30.0f, 0.0f }; }, us, 2s);
	REQUIRE(!drawn.empty());
	CHECK(std::abs(Ground(us, drawn.back().standIn) - StandInSpace::CLEAR) < 0.01f);
	CHECK(drawn.back().standIn.z == 30.0f);
}

TEST("stand-in space: in power armor the room is wider")
{
	const StandInSpace::Point us{ 0.0f, 0.0f, 0.0f };
	const auto drawn = Play([&](float) { return Pose{ 0.0f, 45.0f, 0.0f, 0.0f }; }, us, 2s, StandInSpace::CLEAR_ARMOR);
	REQUIRE(!drawn.empty());
	CHECK(std::abs(drawn.back().standIn.y - StandInSpace::CLEAR_ARMOR) < 0.01f);
	CHECK(drawn.back().standIn.x == 0.0f);
	// Out of it, 45 units is room enough.
	CHECK(Same(StandInSpace::Clear({ 0.0f, 45.0f, 0.0f }, 0.0f, us), { 0.0f, 45.0f, 0.0f }));
}

TEST("stand-in space: a spot or heading that is not a number does not make one")
{
	const StandInSpace::Point us{ 10.0f, 20.0f, 0.0f };
	// On our spot with no heading to go by: out along +y.
	const auto out = StandInSpace::Clear(us, std::nanf(""), us);
	CHECK(out.x == us.x);
	CHECK(std::abs(out.y - (us.y + StandInSpace::CLEAR)) < 0.01f);
	// With no heading of ours to go by, a step along theirs (east).
	StandInSpace::Away away;
	const auto         theirs = StandInSpace::Place(away, us, pi / 2.0f, us, std::nanf(""));
	CHECK(std::abs(theirs.x - (us.x + StandInSpace::CLEAR)) < 0.01f);
	CHECK(std::abs(theirs.y - us.y) < 0.01f);
	// A spot that is not a number is left as it came (the state check drops those before they are drawn).
	const auto bad = StandInSpace::Clear({ std::nanf(""), 20.0f, 0.0f }, 0.0f, us);
	CHECK(std::isnan(bad.x));
}
