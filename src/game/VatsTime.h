#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

// VATS and the time scale in a session (game/RealTime.cpp does the game's side; game/RealTimeVats.h sets the
// multiplier). Each game says whether its player is in VATS and how fast it runs (kReportTimeScale); the server sends
// everyone the table, with the host's group VATS speed (kTimeScales; common/TimeScaleBook.h).
//
// - By default (bRealTimeVats) VATS does not slow the game at all, and the group speed is off: nothing here changes a
//   game, it only says who is in VATS (net status, the log).
// - A game that runs slowed on its own (Jet, or VATS with bRealTimeVats off) keeps that to itself: friends' stand-ins and
//   the NPCs their games run go on at full speed in it, on the wall clock, as they really move (StateBuffer, MirrorClock,
//   EngineClock all keep to the wall clock and the engine's real frame time, so no buffer grows and nothing snaps when
//   the multiplier changes back).
// - With the host's group VATS speed on (bGroupVatsSlowdown, fGroupVatsSpeed), while anyone is in VATS every game, theirs
//   included, runs at that speed: everyone sees the same slow motion, and since every game runs at the same rate, the
//   stand-ins and NPCs, which keep to the wall clock, are slowed just as much as all else.
//
// Free of the game, so it is tested.
namespace VatsTime
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	constexpr float FULL = 0.999f;     // a multiplier under this is a slowed game
	constexpr float CHANGE = 0.02f;    // a multiplier this far from the one said last is said again
	constexpr auto  MIN_GAP = 100ms;   // a multiplier that changes (Jet wearing off by steps) is said at most this often
	constexpr auto  RESEND = 3s;       // said again this often though nothing changed: a report the server's flood cap
	                                   // dropped ("out of VATS") would else hold every game at the group's speed

	// Our own game this frame.
	struct Own
	{
		bool  vats = false;        // in VATS: its menu, or a shot playing out
		float multiplier = 1.0f;   // the global time multiplier it runs at

		[[nodiscard]] std::uint8_t Flags() const
		{
			return static_cast<std::uint8_t>((vats ? Protocol::kTimeVats : 0) | (multiplier < FULL ? Protocol::kTimeSlowed : 0));
		}
	};

	// When our own goes out: at once when VATS comes up or goes down, or the game becomes slowed or not; a multiplier
	// that moves within that, when it has moved CHANGE, MIN_GAP apart at most. Said again after each welcome, and every
	// RESEND while welcomed (the server passes on only a change, so the same word again costs the others nothing).
	class Reporter
	{
	public:
		// The state to send now, if any.
		std::optional<Protocol::TimeScale> Frame(Clock::time_point a_now, bool a_welcomed, const Own& a_own)
		{
			if (!a_welcomed) {
				sent.reset();
				return std::nullopt;
			}
			const float multiplier = std::isfinite(a_own.multiplier) ? std::clamp(a_own.multiplier, 0.0f, Protocol::MAX_TIME_MULTIPLIER) : 1.0f;
			const Protocol::TimeScale now{ 0, a_own.Flags(), multiplier };
			const bool                due = !sent || sent->flags != now.flags ||
			                 (std::abs(sent->multiplier - now.multiplier) >= CHANGE && a_now - sentAt >= MIN_GAP) || a_now - sentAt >= RESEND;
			if (!due) {
				return std::nullopt;
			}
			sent = now;
			sentAt = a_now;
			++reports;
			return now;
		}

		[[nodiscard]] std::uint64_t Reports() const { return reports; }

	private:
		std::optional<Protocol::TimeScale> sent;
		Clock::time_point                  sentAt{};
		std::uint64_t                      reports = 0;
	};

	// One friend's row changed (for the log).
	struct Change
	{
		std::uint32_t playerId = 0;
		bool          vats = false;      // in VATS now
		bool          wasVats = false;
		float         multiplier = 1.0f;
		bool          left = false;      // gone from the table
	};

	// The friends' rows and the host's group speed, from the last kTimeScales. Our own row is ours to know: it is left out.
	class Friends
	{
	public:
		std::vector<Change> Set(const Protocol::TimeScales& a_table, std::uint32_t a_self)
		{
			std::vector<Change> changes;
			std::vector<Protocol::TimeScale> next;
			for (const auto& row : a_table.players) {
				if (row.playerId == a_self || std::ranges::find(next, row.playerId, &Protocol::TimeScale::playerId) != next.end()) {
					continue;
				}
				next.push_back(row);
				const auto before = std::ranges::find(rows, row.playerId, &Protocol::TimeScale::playerId);
				const bool wasVats = before != rows.end() && (before->flags & Protocol::kTimeVats) != 0;
				const bool vats = (row.flags & Protocol::kTimeVats) != 0;
				if (before == rows.end() || *before != row) {
					changes.push_back({ row.playerId, vats, wasVats, row.multiplier, false });
				}
			}
			for (const auto& row : rows) {
				if (std::ranges::find(next, row.playerId, &Protocol::TimeScale::playerId) == next.end()) {
					changes.push_back({ row.playerId, false, (row.flags & Protocol::kTimeVats) != 0, 1.0f, true });
				}
			}
			rows = std::move(next);
			groupSpeed = a_table.groupSpeed;
			return changes;
		}

		void Clear()
		{
			rows.clear();
			groupSpeed = 0.0f;
		}

		[[nodiscard]] float GroupSpeed() const { return groupSpeed; }

		[[nodiscard]] std::size_t InVats() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(rows, [](const Protocol::TimeScale& a_row) { return (a_row.flags & Protocol::kTimeVats) != 0; }));
		}

		[[nodiscard]] const std::vector<Protocol::TimeScale>& Rows() const { return rows; }

	private:
		std::vector<Protocol::TimeScale> rows;
		float                            groupSpeed = 0.0f;
	};

	// The group's slow motion in effect: the host's speed while it is on and anyone in the session, us too, is in VATS;
	// 0 else. Our own VATS counts at once, before the server's table says so.
	constexpr float Group(float a_groupSpeed, bool a_ownVats, std::size_t a_friendsInVats)
	{
		return a_groupSpeed > 0.0f && a_groupSpeed < 1.0f && (a_ownVats || a_friendsInVats > 0) ? a_groupSpeed : 0.0f;
	}
}
