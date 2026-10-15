#include "Test.h"

#include "Protocol.h"
#include "game/StandInSpace.h"
#include "game/StateBuffer.h"

#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

// Where a friend's stand-in is drawn while they stand where we do. Their game samples them on Session's schedule, the
// server relays each state in a PlayerStates batch (encoded and decoded as on the wire), our game reads what has come
// once a frame, samples it from StateBuffer and puts the stand-in where StandInSpace says, as RemotePlayers does.
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

	// Every frame drawn, with us standing at a_us. Their game sends every 33 ms; each batch takes 40 ms to come.
	std::vector<Drawn> Play(const std::function<Pose(float)>& a_friend, const StandInSpace::Point& a_us, Clock::duration a_length, float a_clear = StandInSpace::CLEAR)
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
				drawn.push_back({ std::chrono::duration<float>(now - start).count(), at, StandInSpace::Clear(at, sampled->state.heading, a_us, a_clear) });
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

TEST("stand-in space: a friend who moves to our very spot is drawn a body's room in front of themselves, not through us")
{
	// They stand 600 units off facing east, then a moveto puts them on our spot (as onto our stand-in while we were down).
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

	// Standing on our spot it holds still, a step ahead of them (east), at their height: it does not jitter about us.
	const auto& last = drawn.back();
	CHECK(std::abs(last.standIn.x - (us.x + StandInSpace::CLEAR)) < 0.01f);
	CHECK(std::abs(last.standIn.y - us.y) < 0.01f);
	CHECK(last.standIn.z == us.z);
	for (std::size_t i = drawn.size() - 60; i < drawn.size(); ++i) {
		CHECK(Same(drawn[i].standIn, last.standIn));
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
	// A spot that is not a number is left as it came (the state check drops those before they are drawn).
	const auto bad = StandInSpace::Clear({ std::nanf(""), 20.0f, 0.0f }, 0.0f, us);
	CHECK(std::isnan(bad.x));
}
