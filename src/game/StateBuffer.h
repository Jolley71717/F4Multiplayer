#pragma once

#include "Protocol.h"
#include "game/Prediction.h"
#include "game/StandInHold.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

// A friend's states as they come, and where their stand-in is drawn between them. Kept free of the game so
// it can be tested.
namespace StateBuffer
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// Stand-ins are drawn a little in the past, so there are two states to blend between. How far is eased
	// toward how far past the newest state we have found ourselves lately (all but the furthest few frames
	// of the last LEADS), plus slack: that covers the gap between their states, the trip, and the wait until
	// our game next reads the network, which is a whole frame in a slow game. A friend whose states stop
	// for longer than LEAD_LIMIT (a loading screen) is not counted. MIN_DELAY is the least, as before.
	constexpr auto  MIN_DELAY = 70ms;
	constexpr auto  MAX_DELAY = 250ms;
	constexpr float DELAY_SLACK = 0.01f;  // seconds
	constexpr float DELAY_SLEW = 0.002f;  // seconds per frame, so the drawn time never jumps
	constexpr auto  LEAD_LIMIT = 500ms;
	constexpr std::size_t LEADS = 64;
	constexpr std::size_t LEADS_LEFT_OUT = 64;  // one in this many: the furthest frames, which may stand still
	constexpr auto  STALE_AFTER = 5s;
	constexpr std::size_t MAX_SNAPSHOTS = 32;

	struct Snapshot
	{
		Clock::time_point     stamp;  // when it was sampled, on our clock (see Prediction::SenderClock)
		Protocol::PlayerState state;
	};

	struct Sampled
	{
		Protocol::PlayerState state;
		float                 motionAngle = 0.0f;  // world heading of travel, radians
		bool                  held = false;        // past the newest state: it stands where that one put it
		float                 heldSpeed = 0.0f;    // when held: the speed the newest state said (state.speed is stopped once states dry up)

		// The friend's own speed in the state it was put by: moving still while their next state is late, though it is
		// held, and drawn, standing.
		[[nodiscard]] float OwnSpeed() const { return held ? heldSpeed : state.speed; }
	};

	inline float LerpAngle(float a_from, float a_to, float a_t)
	{
		return a_from + StandInHold::TurnBetween(a_from, a_to) * a_t;
	}

	struct Buffer
	{
		std::deque<Snapshot>     snapshots;
		Prediction::SenderClock  sender;
		Prediction::ArrivalClock arrivals;  // for states that don't say when they were sent
		Clock::time_point        arrived{};  // when the last state came
		float                    delay = std::chrono::duration<float>(MIN_DELAY).count();  // seconds behind now it is drawn
		std::array<float, LEADS> leads{};  // how far past the newest state's stamp each recent frame was, seconds
		std::size_t              leadCount = 0;
		std::size_t              leadNext = 0;

		// Takes a state that came at a_now; false (and nothing kept) when it is no newer than the newest held.
		bool Push(const Protocol::PlayerState& a_state, Clock::time_point a_now)
		{
			if (!snapshots.empty() && a_state.sequence <= snapshots.back().state.sequence) {
				return false;
			}
			const auto steps = snapshots.empty() ? 1u : a_state.sequence - snapshots.back().state.sequence;
			const auto stamp = a_state.sentMs ? sender.Next(a_now, a_state.sentMs) : arrivals.Next(a_now, steps);
			if (a_state.sentMs && sender.steppedBack > Clock::duration::zero()) {
				// The first state came late, so the ones kept were stamped too late as well: they move back with it.
				for (auto& snapshot : snapshots) {
					snapshot.stamp -= sender.steppedBack;
				}
			}
			snapshots.push_back({ stamp, a_state });
			arrived = a_now;
			while (snapshots.size() > MAX_SNAPSHOTS) {
				snapshots.pop_front();
			}
			return true;
		}

		// How far behind now it should be drawn, before easing: all but the furthest of the recent leads, plus slack.
		float Wanted() const
		{
			auto       sorted = leads;
			const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(leadCount);
			const auto at = sorted.begin() + static_cast<std::ptrdiff_t>(leadCount / LEADS_LEFT_OUT);
			std::nth_element(sorted.begin(), at, end, std::greater<>{});
			return std::clamp(*at + DELAY_SLACK, std::chrono::duration<float>(MIN_DELAY).count(), std::chrono::duration<float>(MAX_DELAY).count());
		}

		// Once a frame, before Sample: drops states too old to be drawn again, notes how far past the newest
		// one now is, and eases the delay toward what that asks for.
		void Frame(Clock::time_point a_now)
		{
			while (snapshots.size() > 2 && a_now - snapshots[1].stamp > MIN_DELAY + 1s) {
				snapshots.pop_front();
			}
			if (snapshots.empty()) {
				return;
			}
			if (const auto lead = a_now - snapshots.back().stamp; lead < LEAD_LIMIT) {
				leads[leadNext] = std::chrono::duration<float>(lead).count();
				leadNext = (leadNext + 1) % LEADS;
				leadCount = (std::min)(leadCount + 1, LEADS);
			}
			if (leadCount) {
				delay += std::clamp(Wanted() - delay, -DELAY_SLEW, DELAY_SLEW);
			}
		}

		// Interpolated state at (now - delay), or nullopt if we have nothing recent.
		std::optional<Sampled> Sample(Clock::time_point a_now) const
		{
			const auto& snaps = snapshots;
			if (snaps.empty() || a_now - snaps.back().stamp > STALE_AFTER) {
				return std::nullopt;
			}

			const auto renderTime = a_now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(delay));
			if (renderTime <= snaps.front().stamp) {
				return Sampled{ snaps.front().state, snaps.front().state.heading };
			}

			for (std::size_t i = 1; i < snaps.size(); ++i) {
				const auto& a = snaps[i - 1];
				const auto& b = snaps[i];
				if (renderTime > b.stamp) {
					continue;
				}

				// Don't blend across a cell or worldspace change (doors, fast travel).
				if (a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace) {
					return Sampled{ b.state, b.state.heading };
				}

				const float span = std::chrono::duration<float>(b.stamp - a.stamp).count();
				const float t = span > 0.0f ? std::chrono::duration<float>(renderTime - a.stamp).count() / span : 1.0f;

				auto out = b.state;
				out.x = std::lerp(a.state.x, b.state.x, t);
				out.y = std::lerp(a.state.y, b.state.y, t);
				out.z = std::lerp(a.state.z, b.state.z, t);
				out.heading = LerpAngle(a.state.heading, b.state.heading, t);

				const float dx = b.state.x - a.state.x;
				const float dy = b.state.y - a.state.y;
				const float motionAngle = dx * dx + dy * dy > 1.0f ? std::atan2(dx, dy) : out.heading;
				return Sampled{ out, motionAngle };
			}

			// Past the newest snapshot: hold position rather than guessing ahead, and stop
			// animating if updates have dried up.
			auto held = snaps.back().state;
			if (a_now - snaps.back().stamp > MIN_DELAY + 250ms) {
				held.speed = 0.0f;
			}
			return Sampled{ held, held.heading, true, snaps.back().state.speed };
		}
	};
}
