#pragma once

#include "Protocol.h"
#include "game/AimPoint.h"
#include "game/MotionPath.h"
#include "game/Prediction.h"
#include "game/StandInHold.h"

#include <algorithm>
#include <array>
#include <atomic>
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
	// toward how far past the newest state we have found ourselves lately, plus slack: that covers the gap
	// between their states, the trip, and the wait until our game next reads the network, which is a whole
	// frame in a slow game. A friend whose states stop for longer than LEAD_LIMIT (a loading screen) is not
	// counted. MIN_DELAY is the least, as before.
	//
	// Lately is the last three seconds, whatever our frame rate: the furthest lead of each LEAD_SLOT, for the last
	// LEADS of them. Kept per frame, all but the furthest of the last 64 frames were four seconds in a 16 fps game
	// but under half a second at 144: a state held up on the way was forgotten long before the next, the delay
	// eased back down, and the next one froze the stand-in again. A frame after a stall of our own game (longer
	// than MAX_DELAY) is not counted: nothing was drawn meanwhile, so nothing stood still.
	//
	// Not the furthest lead of them all, though: a high quantile of the slots. The furthest took one late state in three
	// seconds, or one long frame of the sender's game, as what every state needed, and on a clean link drew the friend
	// 150 ms behind where about 70 covered all but a few slots. A state later than the delay covers is carried on past
	// (EXTRAPOLATE); a run of them (a link that got worse) is more than the quantile leaves out, so the delay grows for it.
	constexpr float LEAD_QUANTILE = 0.95f;
	// What Wanted takes: LEAD_QUANTILE, or another to measure against it (netsim lead); 1 is the furthest, as before.
	inline std::atomic<float> leadQuantile{ LEAD_QUANTILE };
	constexpr auto  MIN_DELAY = 70ms;
	constexpr auto  MAX_DELAY = 250ms;
	constexpr float DELAY_SLACK = 0.01f;  // seconds
	constexpr float DELAY_SLEW = 0.05f;   // seconds per second, so the drawn time never jumps: it runs at most 5% fast or slow
	constexpr float SLEW_FRAME_LIMIT = 0.1f;  // seconds: a longer frame (a stall) eases it no further
	constexpr auto  LEAD_LIMIT = 500ms;
	constexpr auto  LEAD_SLOT = 50ms;
	constexpr std::size_t LEADS = 64;  // slots: 3.2 seconds
	constexpr auto  STALE_AFTER = 5s;
	constexpr std::size_t MAX_SNAPSHOTS = 32;

	// Past the newest state (the next is late on the way: a relay route that hiccups), a friend who was moving goes on the
	// way they were going for up to EXTRAPOLATE, instead of standing until it comes and then going on from there. Stood,
	// every late state over Steam's relays was a stop in their run, which a host saw as a jittery friend. When the late
	// state comes, what that guess got wrong (they turned or stopped meanwhile) is eased out over CORRECTION seconds rather
	// than jumped (see Correction). Past EXTRAPOLATE it stands as before, and the drawn time waits there for the next.
	constexpr auto  EXTRAPOLATE = 100ms;
	constexpr float EXTRAPOLATE_SPEED = 1200.0f;  // units/second: faster than any run between two states is a jump, not carried on
	constexpr float EXTRAPOLATE_FROM = 10.0f;     // units/second: the newest state says they stand (MoveSync::STOP): not carried on
	constexpr float CORRECTION = 0.1f;            // seconds: the time constant a correction is eased out with
	constexpr float SNAP_DISTANCE = 256.0f;       // units: a correction further than this is a jump (setpos, a fast stride): taken at once

	struct Snapshot
	{
		Clock::time_point     stamp;  // when it was sampled, on our clock (see Prediction::SenderClock)
		Protocol::PlayerState state;
	};

	struct Sampled
	{
		Protocol::PlayerState state;
		float                 motionAngle = 0.0f;  // world heading of travel, radians
		bool                  held = false;        // past the newest state, and past going on from it: it stands
		bool                  ahead = false;       // past the newest state, going on the way they went (EXTRAPOLATE)
		float                 heldSpeed = 0.0f;    // when held: the speed the newest state said (state.speed is stopped once states dry up)
		// The pitch it aims at, degrees up (+) or down (-): state.pitch blended between the two states it is drawn
		// between, as its place and heading are (AimPoint::BlendPitch), so its gun sweeps rather than steps.
		float                 pitch = 0.0f;

		// The friend's own speed in the state it was put by: moving still while their next state is late, though it is
		// held, and drawn, standing.
		[[nodiscard]] float OwnSpeed() const { return held ? heldSpeed : state.speed; }
		// Where it is from now on, for the engine's update of the stand-in, which may come before or after our next frame's
		// work (MotionPath.h): here now, then each newer state a delay after its stamp.
		MotionPath::Path      path;
	};

	inline float LerpAngle(float a_from, float a_to, float a_t)
	{
		return a_from + StandInHold::TurnBetween(a_from, a_to) * a_t;
	}

	// What the stand-in is drawn off from its states by, while a jump is eased out. A late state the stand-in was carried
	// on past (EXTRAPOLATE), or stamps the sender's clock moved back (SenderClock::steppedBack), change where the states put
	// it at the moment it is drawn: it would jump there. Instead the difference is kept here at the moment it came and shrinks
	// to nothing with CORRECTION as its time constant; what is drawn goes on from where it was.
	struct Correction
	{
		float             x = 0.0f;
		float             y = 0.0f;
		float             z = 0.0f;
		Clock::time_point at{};  // when it was that much

		// How much of it is left at a_now (1 at `at`, falling off from there; all of it before).
		[[nodiscard]] float Left(Clock::time_point a_now) const
		{
			if (x == 0.0f && y == 0.0f && z == 0.0f) {
				return 0.0f;
			}
			const float since = std::chrono::duration<float>(a_now - at).count();
			return since <= 0.0f ? 1.0f : std::exp(-since / CORRECTION);
		}
	};

	struct Buffer
	{
		std::deque<Snapshot>     snapshots;
		Prediction::SenderClock  sender;
		Prediction::ArrivalClock arrivals;  // for states that don't say when they were sent
		Clock::time_point        arrived{};  // when the last state came
		float                    delay = std::chrono::duration<float>(MIN_DELAY).count();  // seconds behind now it is drawn
		std::array<float, LEADS> leads{};  // the furthest our frames went past the newest state the last one handed over, each recent slot, seconds
		std::size_t              leadCount = 0;
		std::size_t              leadNext = 0;
		Clock::time_point        slotFrom{};  // when the newest slot began
		bool                     waiting = false;    // the drawn time waits at the newest state for the next (see Frame)
		float                    beforeWait = 0.0f;  // the delay when it began to
		Clock::time_point        handedTo{};  // the newest state's stamp when the last frame sampled: where the path it handed over ends
		Clock::time_point        framed{};    // when Frame last ran
		Correction               correction;  // a jump being eased out (Correct)

		// Takes a state that came at a_now; false (and nothing kept) when it is no newer than the newest held.
		bool Push(const Protocol::PlayerState& a_state, Clock::time_point a_now)
		{
			if (!snapshots.empty() && a_state.sequence <= snapshots.back().state.sequence) {
				return false;
			}
			// Where it is drawn now, before this state changes that (see Correction).
			const auto before = Sample(a_now);
			const auto steps = snapshots.empty() ? 1u : a_state.sequence - snapshots.back().state.sequence;
			const auto stamp = a_state.sentMs ? sender.Next(a_now, a_state.sentMs) : arrivals.Next(a_now, steps);
			if (a_state.sentMs && sender.steppedBack > Clock::duration::zero()) {
				// The first state came late, so the ones kept were stamped too late as well: they move back with it.
				for (auto& snapshot : snapshots) {
					snapshot.stamp -= sender.steppedBack;
				}
				if (handedTo != Clock::time_point{}) {
					handedTo -= sender.steppedBack;
				}
			}
			snapshots.push_back({ stamp, a_state });
			arrived = a_now;
			while (snapshots.size() > MAX_SNAPSHOTS) {
				snapshots.pop_front();
			}
			Correct(before, a_now);
			return true;
		}

		// A state just came: where the states now draw it at a_now against a_before, where they drew it a moment ago (with
		// what was left of the correction then, which Sample adds to both). The difference is kept, to be eased out from
		// a_now on. In another space, or further than SNAP_DISTANCE, there is nothing to ease: it goes there. Nor after a
		// stall of our own game (no frame for longer than SLEW_FRAME_LIMIT): nothing was drawn meanwhile, so nothing would
		// be seen to jump, and eased it would trail behind for a while after.
		void Correct(const std::optional<Sampled>& a_before, Clock::time_point a_now)
		{
			const bool drawing = framed != Clock::time_point{} && a_now - framed <= std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(SLEW_FRAME_LIMIT));
			const auto after = a_before && drawing ? Sample(a_now) : std::nullopt;
			if (!after || a_before->state.cell != after->state.cell || a_before->state.worldspace != after->state.worldspace) {
				correction = {};
				return;
			}
			const Correction next{ a_before->state.x - after->state.x + correction.x * correction.Left(a_now), a_before->state.y - after->state.y + correction.y * correction.Left(a_now),
				a_before->state.z - after->state.z + correction.z * correction.Left(a_now), a_now };
			const bool none = std::abs(next.x) < 0.01f && std::abs(next.y) < 0.01f && std::abs(next.z) < 0.01f;
			correction = none || next.x * next.x + next.y * next.y + next.z * next.z > SNAP_DISTANCE * SNAP_DISTANCE ? Correction{} : next;
		}

		// The way the newest state was going (units a second), from the one before it: none when they stand, the two are in
		// different spaces or too far apart in time to say, or it is faster than anyone runs (a jump).
		[[nodiscard]] std::optional<std::array<float, 3>> Velocity() const
		{
			if (snapshots.size() < 2) {
				return std::nullopt;
			}
			const auto& a = snapshots[snapshots.size() - 2];
			const auto& b = snapshots.back();
			const float span = std::chrono::duration<float>(b.stamp - a.stamp).count();
			if (b.state.speed < EXTRAPOLATE_FROM || a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace || span <= 0.0f || span > std::chrono::duration<float>(MAX_DELAY).count()) {
				return std::nullopt;
			}
			// In the air the fall is not a line: only the way across is carried on.
			const bool                 inAir = (b.state.flags & Protocol::kInAir) != 0;
			const std::array<float, 3> v{ (b.state.x - a.state.x) / span, (b.state.y - a.state.y) / span, inAir ? 0.0f : (b.state.z - a.state.z) / span };
			if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > EXTRAPOLATE_SPEED * EXTRAPOLATE_SPEED) {
				return std::nullopt;
			}
			return v;
		}

		// How far behind now it should be drawn, before easing: the recent leads' quantile (LEAD_QUANTILE), plus slack.
		// With few slots yet, that is the furthest of them.
		float Wanted() const
		{
			auto        sorted = leads;
			const auto  end = sorted.begin() + static_cast<std::ptrdiff_t>(leadCount);
			const float q = std::clamp(leadQuantile.load(std::memory_order_relaxed), 0.0f, 1.0f);
			const auto  rank = static_cast<std::size_t>(std::ceil(q * static_cast<float>(leadCount)));
			const auto  at = sorted.begin() + static_cast<std::ptrdiff_t>(std::clamp<std::size_t>(rank, 1, leadCount) - 1);
			std::nth_element(sorted.begin(), at, end);
			return std::clamp(*at + DELAY_SLACK, std::chrono::duration<float>(MIN_DELAY).count(), std::chrono::duration<float>(MAX_DELAY).count());
		}

		// Notes a frame's lead (seconds) in the slot for a_now: the slots for the time since the last are opened empty.
		void NoteLead(Clock::time_point a_now, float a_lead)
		{
			if (leadCount == 0) {
				slotFrom = a_now;
				leads[0] = a_lead;
				leadNext = leadCount = 1;
				return;
			}
			for (std::size_t i = 0; i < LEADS && a_now - slotFrom >= LEAD_SLOT; ++i) {
				slotFrom += LEAD_SLOT;
				leads[leadNext] = 0.0f;
				leadNext = (leadNext + 1) % LEADS;
				leadCount = (std::min)(leadCount + 1, LEADS);
			}
			if (a_now - slotFrom >= LEAD_SLOT) {
				slotFrom = a_now;  // longer ago than all the slots: every one was opened empty
			}
			auto& slot = leads[(leadNext + LEADS - 1) % LEADS];
			slot = (std::max)(slot, a_lead);
		}

		// Once a frame, before Sample: drops states too old to be drawn again, notes how far past the newest
		// one the frames since the last were, and eases the delay toward what that asks for. Between our frames the
		// engine draws the stand-in on the path the last one handed over (MotionPath.h), which ends at the newest state
		// it had: that is how far past it they went, up to now, when our work comes late in a frame or misses one.
		void Frame(Clock::time_point a_now)
		{
			while (snapshots.size() > 2 && a_now - snapshots[1].stamp > MIN_DELAY + 1s) {
				snapshots.pop_front();
			}
			if (snapshots.empty()) {
				return;
			}
			const auto newest = snapshots.back().stamp;
			const auto handed = handedTo != Clock::time_point{} ? (std::min)(handedTo, newest) : newest;
			handedTo = newest;
			const float frame = framed != Clock::time_point{} ? std::chrono::duration<float>(a_now - framed).count() : 0.0f;
			framed = a_now;
			if (const auto lead = a_now - handed; lead < LEAD_LIMIT && frame <= std::chrono::duration<float>(MAX_DELAY).count()) {
				NoteLead(a_now, std::chrono::duration<float>(lead).count());
			}
			// By time, not by frame: 2 ms a frame moved a 60 fps game's stand-in 12% fast or slow while the delay changed.
			const float seconds = std::clamp(frame, 0.0f, SLEW_FRAME_LIMIT);
			if (leadCount) {
				delay += std::clamp(Wanted() - delay, -DELAY_SLEW * seconds, DELAY_SLEW * seconds);
			}
			// Their next state is late: the stand-in stands at the newest one, and the drawn time waits there with it
			// instead of running on past it. Run on, the next state to come was drawn from where the run had got to,
			// and the stand-in froze and then leapt ahead by as long as it had stood; waited, it goes on from where it
			// stood, and the delay eases back down afterwards. It waits MAX_DELAY at most. A silence past LEAD_LIMIT
			// (a loading screen) is no late state: the delay goes back to what it was, and the leads its start left are
			// forgotten, so a friend back from one is not drawn further behind for seconds after.
			// A friend who was moving goes on past it first, for up to EXTRAPOLATE (see there), and waits only after that.
			const float reached = std::chrono::duration<float>(a_now - newest).count();
			const float ahead = Velocity() ? std::chrono::duration<float>(EXTRAPOLATE).count() : 0.0f;
			if (reached <= delay + ahead) {
				waiting = false;
			} else if (a_now - newest <= LEAD_LIMIT) {
				if (!waiting) {
					waiting = true;
					beforeWait = delay;
				}
				delay = (std::min)(reached - ahead, std::chrono::duration<float>(MAX_DELAY).count());
			} else if (waiting) {
				waiting = false;
				delay = beforeWait;
				leadCount = leadNext = 0;
			}
		}

		// Interpolated state at (now - delay), or nullopt if we have nothing recent: carried on past the newest state for a
		// moment (EXTRAPOLATE), and off its states by what is left of a correction (Correction). Its path ends going on the
		// way the newest state went, for the engine's updates between our frames.
		std::optional<Sampled> Sample(Clock::time_point a_now) const
		{
			auto out = Raw(a_now);
			if (!out) {
				return out;
			}
			const auto& newest = snapshots.back();
			const auto  behind = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(delay));
			const auto& path = out->path;
			// The path ran to the newest state, in its space (Ahead ends it at a door): it goes on from there.
			const bool toNewest = out->state.cell == newest.state.cell && out->state.worldspace == newest.state.worldspace && path.count > 0 &&
			                      (out->held || out->ahead || path.points[path.count - 1].at == newest.stamp + behind);
			if (const auto v = toNewest ? Velocity() : std::nullopt) {
				const float seconds = std::chrono::duration<float>(EXTRAPOLATE).count();
				out->path.Add({ newest.stamp + behind + EXTRAPOLATE, newest.state.x + (*v)[0] * seconds, newest.state.y + (*v)[1] * seconds, newest.state.z + (*v)[2] * seconds, newest.state.heading });
			}
			if (const float left = correction.Left(a_now); left > 0.0f) {
				out->state.x += correction.x * left;
				out->state.y += correction.y * left;
				out->state.z += correction.z * left;
				for (std::uint8_t i = 0; i < out->path.count; ++i) {
					auto&       point = out->path.points[i];
					const float k = correction.Left(point.at);
					point.x += correction.x * k;
					point.y += correction.y * k;
					point.z += correction.z * k;
				}
			}
			return out;
		}

	private:
		std::optional<Sampled> Raw(Clock::time_point a_now) const
		{
			const auto& snaps = snapshots;
			if (snaps.empty() || a_now - snaps.back().stamp > STALE_AFTER) {
				return std::nullopt;
			}

			const auto behind = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(delay));
			const auto renderTime = a_now - behind;
			// What it is drawn as now (aimed at a_pitch), and the path on from there through the states from a_next.
			const auto sampled = [&](const Protocol::PlayerState& a_state, float a_motionAngle, std::size_t a_next, float a_pitch) {
				Sampled out{ a_state, a_motionAngle };
				out.pitch = a_pitch;
				const auto ahead = MotionPath::Ahead({ a_now, a_state.x, a_state.y, a_state.z, a_state.heading }, a_state.cell, a_state.worldspace, snaps, a_next, behind, &Snapshot::stamp);
				// From the state before too, when there is one in this space: the engine places the stand-in at the time of
				// its frame (FrameClock.h), which can be a little before this moment, and there it is on the way from that
				// state, not standing where this moment has it.
				if (a_next > 0 && a_next <= snaps.size() && snaps[a_next - 1].state.cell == a_state.cell && snaps[a_next - 1].state.worldspace == a_state.worldspace) {
					const auto& before = snaps[a_next - 1];
					out.path.Add({ before.stamp + behind, before.state.x, before.state.y, before.state.z, before.state.heading });
				}
				for (std::uint8_t i = 0; i < ahead.count && out.path.Add(ahead.points[i]); ++i) {
				}
				return out;
			};
			if (renderTime <= snaps.front().stamp) {
				return sampled(snaps.front().state, snaps.front().state.heading, 0, static_cast<float>(snaps.front().state.pitch));
			}
			// Frame waits at the newest state, or a moment past it, rather than running on: there, it is held. (The delay is
			// kept in float seconds, so the drawn time comes back a few nanoseconds either side of it.)
			const bool atNewest = renderTime + std::chrono::microseconds(1) >= snaps.back().stamp;

			for (std::size_t i = 1; i < snaps.size() && !atNewest; ++i) {
				const auto& a = snaps[i - 1];
				const auto& b = snaps[i];
				if (renderTime > b.stamp) {
					continue;
				}

				// Don't blend across a cell or worldspace change (doors, fast travel).
				if (a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace) {
					return sampled(b.state, b.state.heading, i, static_cast<float>(b.state.pitch));
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
				return sampled(out, motionAngle, i, AimPoint::BlendPitch(static_cast<float>(a.state.pitch), static_cast<float>(b.state.pitch), t));
			}

			// Past the newest snapshot: on the way it went for up to EXTRAPOLATE, then held there; and the animation stops if
			// updates have dried up.
			const auto& newest = snaps.back();
			auto        held = newest.state;
			const auto  past = (std::max)(renderTime - newest.stamp, Clock::duration::zero());
			const auto  v = Velocity();
			if (v) {
				const float seconds = std::chrono::duration<float>((std::min)(past, std::chrono::duration_cast<Clock::duration>(EXTRAPOLATE))).count();
				held.x += (*v)[0] * seconds;
				held.y += (*v)[1] * seconds;
				held.z += (*v)[2] * seconds;
			}
			if (a_now - newest.stamp > MIN_DELAY + 250ms) {
				held.speed = 0.0f;
			}
			const float motionAngle = v && ((*v)[0] != 0.0f || (*v)[1] != 0.0f) ? std::atan2((*v)[0], (*v)[1]) : held.heading;
			auto        out = sampled(held, motionAngle, snaps.size(), static_cast<float>(held.pitch));
			// Frame waits at EXTRAPOLATE past it once that is over: there it is held, though the float delay brings the drawn time
			// back a few nanoseconds short of it (as at the newest state, above), which way depending on the clock's value.
			out.ahead = v && past + std::chrono::microseconds(1) < EXTRAPOLATE;
			out.held = !out.ahead;
			out.heldSpeed = newest.state.speed;
			return out;
		}
	};
}
