// When a mirrored NPC's states are drawn (MirrorClock.h, as NpcSync::ApplyStates stamps its snapshots): on the owner's
// clock when the state says when it was sampled, by the arrivals evened out when it does not (an older build's), and
// afresh on a change of hands.

#include "Test.h"

#include "game/MirrorClock.h"
#include "game/Prediction.h"

#include <cmath>
#include <vector>

using namespace std::chrono_literals;

namespace
{
	using Clock = MirrorClock::Clock;

	float Ms(Clock::duration a_duration)
	{
		return std::chrono::duration<float, std::milli>(a_duration).count();
	}
}

TEST("mirror clock: an owner that halves its rate for a far NPC, then doubles it again, has its states stamped as far apart as it sampled them")
{
	// The owner samples every 50 ms, then every 100 ms (the NPC went past FAST_RANGE from every player, or its fight
	// ended), then every 50 ms again; each state takes 40 ms to come. Stamped by the owner's clock, every stamp is its
	// sampling gap after the last, and none sits more than a few ms off its arrival once the trip is known.
	MirrorClock::Stamps stamps;
	const auto          start = Clock::time_point{} + 10s;
	std::uint32_t       sentMs = 1000;
	auto                sentAt = start;
	auto                last = Clock::time_point{};
	std::vector<float>  gaps;
	std::vector<float>  offBeat;
	const auto          send = [&](int a_gap) {
		sentMs += static_cast<std::uint32_t>(a_gap);
		sentAt += std::chrono::milliseconds(a_gap);
		const auto arrived = sentAt + 40ms;
		const auto stamp = stamps.Next(arrived, sentMs);
		CHECK(stamps.SteppedBack() == Clock::duration::zero());
		if (last != Clock::time_point{}) {
			gaps.push_back(Ms(stamp - last));
			offBeat.push_back(Ms(stamp - arrived));
		}
		last = stamp;
	};
	for (int i = 0; i < 40; ++i) {
		send(50);
	}
	for (int i = 0; i < 40; ++i) {
		send(100);
	}
	for (int i = 0; i < 40; ++i) {
		send(50);
	}
	REQUIRE(gaps.size() == 119);
	for (std::size_t i = 0; i < gaps.size(); ++i) {
		const float sampled = i < 39 ? 50.0f : i < 79 ? 100.0f : 50.0f;
		CHECK(std::abs(gaps[i] - sampled) < 1.0f);
		CHECK(std::abs(offBeat[i]) < 3.0f);
	}
}

TEST("mirror clock: stamped by their arrivals alone, the same states drift up to 130 ms off the beat at each change of rate")
{
	// What the arrival clock makes of the run above, with no sentMs (the bug the owner's clock takes away): after the
	// rate halves the stamps run early (the copy stands at each state and steps on), after it doubles again late (the
	// copy falls that much further behind).
	MirrorClock::Stamps stamps;
	const auto          start = Clock::time_point{} + 10s;
	auto                arrived = start;
	float               earliest = 0.0f;
	float               latest = 0.0f;
	const auto          send = [&](int a_gap) {
		arrived += std::chrono::milliseconds(a_gap);
		const float off = Ms(stamps.Next(arrived, 0) - arrived);
		earliest = (std::min)(earliest, off);
		latest = (std::max)(latest, off);
	};
	for (int i = 0; i < 40; ++i) {
		send(50);
	}
	for (int i = 0; i < 40; ++i) {
		send(100);
	}
	CHECK(earliest < -100.0f);
	CHECK(latest < 5.0f);
	for (int i = 0; i < 40; ++i) {
		send(50);
	}
	CHECK(latest > 100.0f);
}

TEST("mirror clock: states read two at a time by a slow game are still spread out, on either clock")
{
	// A 16 fps game reads the network every 62 ms: the owner's 50 ms states come in ones and twos. On the owner's
	// clock they are 50 ms apart whatever frame read them; on the arrival clock they are spread out once it settles.
	for (const bool owners : { true, false }) {
		MirrorClock::Stamps stamps;
		const auto          start = Clock::time_point{} + 10s;
		std::vector<Clock::time_point> out;
		for (int sent = 0, frame = 1; sent < 80; ++frame) {
			const auto now = start + frame * 62ms;
			while (sent < 80 && start + sent * 50ms + 30ms <= now) {
				out.push_back(stamps.Next(now, owners ? static_cast<std::uint32_t>(1000 + sent * 50) : 0u));
				++sent;
			}
		}
		for (std::size_t i = 40; i < out.size(); ++i) {
			const float gap = Ms(out[i] - out[i - 1]);
			CHECK(owners ? std::abs(gap - 50.0f) < 2.5f : (gap > 30.0f && gap < 70.0f));
		}
	}
}

TEST("mirror clock: a change of hands starts the timeline over on the new owner's clock")
{
	// The first owner's clock reads 1000 ms and up; the second's 900 000 ms and up. Without a fresh start the second
	// owner's first state would be stamped a quarter of an hour on, and the copy would wait at the old states for it.
	MirrorClock::Stamps stamps;
	const auto          start = Clock::time_point{} + 10s;
	auto                last = Clock::time_point{};
	for (int i = 0; i < 20; ++i) {
		last = stamps.Next(start + i * 50ms, static_cast<std::uint32_t>(1000 + i * 50));
	}
	stamps.NewOwner();
	const auto handedOver = start + 20 * 50ms + 120ms;  // the new owner's first state, a trip after the old one's last
	const auto first = stamps.Next(handedOver, 900000);
	CHECK(first == handedOver);
	CHECK(first > last);
	const auto second = stamps.Next(handedOver + 50ms, 900050);
	CHECK(std::abs(Ms(second - first) - 50.0f) < 1.0f);
	// And the arrival clock starts over with it: a state with no sentMs after the change is stamped when it came.
	stamps.NewOwner();
	const auto plain = handedOver + 1s;
	CHECK(stamps.Next(plain, 0) == plain);
}

TEST("mirror clock: an owner whose first state came late has the states kept before stepped back with it")
{
	// The first state took 200 ms (the joiner's game hitched over the welcome), the rest 40 ms: once the quickest trip
	// is seen the stamps step back, and SteppedBack says by how much, so NpcSync moves the snapshots it kept.
	MirrorClock::Stamps stamps;
	const auto          start = Clock::time_point{} + 10s;
	stamps.Next(start + 200ms, 1000);
	CHECK(stamps.SteppedBack() == Clock::duration::zero());
	stamps.Next(start + 50ms + 40ms, 1050);
	CHECK(Ms(stamps.SteppedBack()) > 100.0f);
}

TEST("mirror clock: a state on another clock that overtook the owners' message starts the timeline over, instead of landing hours off")
{
	// The host ran the raider for five seconds (its clock 1000 ms and up, every trip known), a friend's hit took it over,
	// and the friend's first state (their clock three hours on) came 50 ms after the host's last, before the owners'
	// message that would have started the clock over (NewOwner). The old clock would have stamped it three hours on,
	// and the copy would have stood at the host's last state until it was dropped. Stamped when it came instead, and
	// the next on the friend's clock 50 ms after.
	MirrorClock::Stamps stamps;
	const auto          start = Clock::time_point{} + 10s;
	auto                last = Clock::time_point{};
	for (int i = 0; i < 100; ++i) {
		last = stamps.Next(start + i * 50ms + 40ms, static_cast<std::uint32_t>(1000 + i * 50));
	}
	const auto     took = start + 100 * 50ms + 40ms;
	const uint32_t theirs = 1000 + 3 * 60 * 60 * 1000;
	const auto     first = stamps.Next(took, theirs);
	CHECK(first == took);
	CHECK(first > last);
	CHECK(stamps.SteppedBack() == Clock::duration::zero());  // nothing kept had run past now
	const auto second = stamps.Next(took + 50ms, theirs + 50);
	CHECK(std::abs(Ms(second - first) - 50.0f) < 1.0f);
	CHECK(stamps.SteppedBack() == Clock::duration::zero());

	// A new runner whose clock reads lower was never a problem: that step counts as no time, and the stamp moves on by
	// a millisecond (Prediction::SenderClock).
	const auto lower = stamps.Next(took + 100ms, 500);
	CHECK(lower == second + 1ms);
	CHECK(stamps.SteppedBack() == Clock::duration::zero());
}

TEST("mirror clock: stamps that ran more than a second past their arrivals step back behind the state that starts over")
{
	// Every trip took 1.5 s for four seconds (a choked route: the owner's clock is laid that late on ours), then the
	// queue drains and the states held up come a millisecond apart: the stamps keep the late layout and run further
	// past each arrival, which the shift would take a minute to follow down. The first state more than FAR_AHEAD
	// ahead starts the timeline over when it came, and SteppedBack says how far the stamps given before it step back,
	// so NpcSync keeps its snapshots in the order they were given; the warming clock then lays the rest of the burst
	// on its arrivals. No stamp ever sits more than FAR_AHEAD past its arrival, and none is given out of order.
	MirrorClock::Stamps            stamps;
	const auto                     start = Clock::time_point{} + 10s;
	std::vector<Clock::time_point> given;
	int                            restarts = 0;
	auto                           restartedAt = Clock::time_point{};
	std::size_t                    restarted = 0;
	const auto                     send = [&](Clock::time_point a_arrived, std::uint32_t a_sentMs) {
		const auto stamp = stamps.Next(a_arrived, a_sentMs);
		const auto before = given.empty() ? Clock::time_point{} : given.back();
		for (auto& earlier : given) {
			earlier -= stamps.SteppedBack();  // as NpcSync::ApplyStates moves the snapshots it kept
		}
		if (stamp == a_arrived && stamps.SteppedBack() > 500ms) {
			++restarts;
			restartedAt = stamp;
			restarted = given.size();
			CHECK(stamps.SteppedBack() == before - a_arrived + 1ms);
		}
		CHECK(stamp - a_arrived <= MirrorClock::FAR_AHEAD);
		CHECK(given.empty() || given.back() < stamp);
		given.push_back(stamp);
	};
	for (int i = 0; i < 80; ++i) {
		send(start + i * 50ms + 1500ms, static_cast<std::uint32_t>(1000 + i * 50));
	}
	const auto drained = start + 79 * 50ms + 1500ms;
	for (int i = 80; i < 130; ++i) {
		send(drained + (i - 79) * 1ms, static_cast<std::uint32_t>(1000 + i * 50));
	}
	CHECK(restarts == 1);
	REQUIRE(given.size() == 130);
	CHECK(restarted == 100);                                       // the first more than FAR_AHEAD past its arrival
	CHECK(restartedAt == drained + 21ms);                          // started over when it came
	CHECK(std::abs(Ms(given[99] - given[98]) - 50.0f) < 1.0f);  // the last on the old layout, moved back together
	// The rest of the burst is laid on its arrivals by the warming clock (SenderClock's own step back), each step
	// moving the stamps before it back too: given above, in order and none more than FAR_AHEAD past its arrival.
}
