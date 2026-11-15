#include "Test.h"

#include "game/HolsterWatch.h"

#include <cmath>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using HolsterWatch::Do;

namespace
{
	const auto     T0 = HolsterWatch::Clock::time_point{} + 1h;
	constexpr auto FRAME = 16ms;

	using Said = std::vector<std::pair<Do, HolsterWatch::Clock::duration>>;

	// A thigh at one point of its swing, swayed a little.
	HolsterWatch::Leg Pose(float a_swing, float a_sway = 0.0f)
	{
		HolsterWatch::Leg leg;
		leg.m[0] = a_swing;
		leg.m[4] = a_sway;
		return leg;
	}

	// A walk's thigh a_at into it.
	HolsterWatch::Leg Walking(HolsterWatch::Clock::duration a_at)
	{
		return Pose(0.3f * std::sin(6.2831853f * std::chrono::duration<float>(a_at).count()));
	}

	// A thigh that stands.
	HolsterWatch::Leg Standing(HolsterWatch::Clock::duration)
	{
		return Pose(0.2f);
	}

	// Frames from a_from for a_for of a stand-in that should walk, its thigh as a_leg(time since T0) gives it; what the
	// watch said (kNothing left out) is added to a_said with when, since T0.
	template <class LegAt>
	HolsterWatch::Clock::time_point Frames(HolsterWatch::Watch& a_watch, Said& a_said, HolsterWatch::Clock::time_point a_from, HolsterWatch::Clock::duration a_for, LegAt&& a_leg)
	{
		auto t = a_from;
		for (; t < a_from + a_for; t += FRAME) {
			const auto leg = a_leg(t - T0);
			if (const auto what = a_watch.Update(true, &leg, t); what != Do::kNothing) {
				a_said.emplace_back(what, t - T0);
			}
		}
		return t;
	}
}

TEST("holster watch: starts as a sneaking humanoid's weapon reaches its holster, moving or not")
{
	CHECK(HolsterWatch::Starts(true, false, true));
	CHECK(!HolsterWatch::Starts(true, true, true));    // away already (or not known)
	CHECK(!HolsterWatch::Starts(true, false, false));  // still on its way
	CHECK(!HolsterWatch::Starts(true, true, false));   // a draw
	CHECK(!HolsterWatch::Starts(false, false, true));  // upright, or no humanoid
}

TEST("holster watch: a watch not started says nothing")
{
	HolsterWatch::Watch watch;
	Said                said;
	Frames(watch, said, T0, 3s, Standing);
	CHECK(said.empty());
	CHECK(!watch.Armed());
	CHECK(watch.Tries() == 0);
}

TEST("holster watch: legs that stand while it should walk are set off again every STILL, MAX_TRIES times, then let be")
{
	HolsterWatch::Watch watch;
	Said                said;
	watch.Arm(T0);
	const auto t = Frames(watch, said, T0, 5s, Standing);
	REQUIRE(said.size() == HolsterWatch::MAX_TRIES + 1u);
	for (std::size_t i = 0; i < HolsterWatch::MAX_TRIES; ++i) {
		const auto nth = static_cast<int>(i) + 1;
		CHECK(said[i].first == Do::kSetOff);
		CHECK(said[i].second >= nth * HolsterWatch::STILL);
		CHECK(said[i].second <= nth * (HolsterWatch::STILL + FRAME));
	}
	CHECK(said.back().first == Do::kGaveUp);
	CHECK(!watch.Armed());
	CHECK(watch.Tries() == HolsterWatch::MAX_TRIES);
	// Let be: nothing more however long it stands.
	Frames(watch, said, t, 5s, Standing);
	CHECK(said.size() == HolsterWatch::MAX_TRIES + 1u);
}

TEST("holster watch: legs that walk are never set off, and nothing is said of them")
{
	HolsterWatch::Watch watch;
	Said                said;
	watch.Arm(T0);
	Frames(watch, said, T0, HolsterWatch::WATCH + 1s, Walking);
	CHECK(said.empty());
	CHECK(watch.Tries() == 0);
	CHECK(!watch.Armed());  // over, WATCH after the holster
}

TEST("holster watch: an idle's slow sway is standing still")
{
	// What the gliding stand-in's thigh did on the rig: about 0.04 a second, one way.
	HolsterWatch::Watch watch;
	Said                said;
	watch.Arm(T0);
	Frames(watch, said, T0, 1s, [](HolsterWatch::Clock::duration a_at) { return Pose(0.2f, 0.04f * std::chrono::duration<float>(a_at).count()); });
	REQUIRE(said.size() == 1);
	CHECK(said[0].first == Do::kSetOff);
	CHECK(said[0].second <= HolsterWatch::STILL + FRAME);
}

TEST("holster watch: says once that the legs move again after a set-off, and again after the next")
{
	HolsterWatch::Watch watch;
	Said                said;
	watch.Arm(T0);
	auto t = Frames(watch, said, T0, 700ms, Standing);
	REQUIRE(said.size() == 1);
	CHECK(said[0].first == Do::kSetOff);
	t = Frames(watch, said, t, 2s, Walking);
	REQUIRE(said.size() == 2);
	CHECK(said[1].first == Do::kWalks);
	CHECK(watch.Tries() == 1);
	// They stop dead again (the set-off's clip ended in the idle): a second set-off, and its own "moves again".
	const auto stopped = Walking(t - T0);
	t = Frames(watch, said, t, 700ms, [&](HolsterWatch::Clock::duration) { return stopped; });
	REQUIRE(said.size() == 3);
	CHECK(said[2].first == Do::kSetOff);
	CHECK(watch.Tries() == 2);
	Frames(watch, said, t, 1s, Walking);
	REQUIRE(said.size() == 4);
	CHECK(said[3].first == Do::kWalks);
}

TEST("holster watch: nothing is counted while it stands or its leg can't be read, and the count starts over after")
{
	for (const bool unreadable : { false, true }) {
		HolsterWatch::Watch watch;
		Said                said;
		watch.Arm(T0);
		auto t = Frames(watch, said, T0, 500ms, Standing);
		// One frame of a stand-in told to stand (or in the air), or with no skeleton to read.
		const auto leg = Pose(0.2f);
		CHECK(watch.Update(unreadable, unreadable ? nullptr : &leg, t) == Do::kNothing);
		CHECK(watch.Still() == HolsterWatch::Clock::duration{});
		t += FRAME;
		t = Frames(watch, said, t, 500ms, Standing);
		CHECK(said.empty());  // a second in all, not STILL in one piece
		Frames(watch, said, t, 200ms, Standing);
		REQUIRE(said.size() == 1);
		CHECK(said[0].first == Do::kSetOff);
	}
}

TEST("holster watch: a hitch counts as one long frame, not as legs that stood through it")
{
	HolsterWatch::Watch watch;
	watch.Arm(T0);
	const auto leg = Pose(0.2f);
	// Frames a second apart (a load, a stall): each counts as LONGEST_FRAME.
	const int needed = static_cast<int>(HolsterWatch::STILL / HolsterWatch::LONGEST_FRAME);
	CHECK(watch.Update(true, &leg, T0) == Do::kNothing);
	for (int i = 1; i < needed; ++i) {
		CHECK(watch.Update(true, &leg, T0 + i * 1s) == Do::kNothing);
	}
	CHECK(watch.Update(true, &leg, T0 + needed * 1s) == Do::kSetOff);
}

TEST("holster watch: ends WATCH after the holster, and starts over at the next")
{
	HolsterWatch::Watch watch;
	Said                said;
	watch.Arm(T0);
	// Walks until just before the end, then stands: too late to be set off.
	auto t = Frames(watch, said, T0, HolsterWatch::WATCH - 300ms, Walking);
	CHECK(watch.Armed());
	t = Frames(watch, said, t, 2s, Standing);
	CHECK(said.empty());
	CHECK(!watch.Armed());
	// The next holster: watched again from nothing. (Frames gives its times since T0; only what was said matters here.)
	watch.Arm(t);
	CHECK(watch.Armed());
	CHECK(watch.Tries() == 0);
	Said again;
	Frames(watch, again, t, 700ms, Standing);
	REQUIRE(again.size() == 1);
	CHECK(again[0].first == Do::kSetOff);
	watch.Clear();
	CHECK(!watch.Armed());
	Frames(watch, again, t + 700ms, 2s, Standing);
	CHECK(again.size() == 1);
}
