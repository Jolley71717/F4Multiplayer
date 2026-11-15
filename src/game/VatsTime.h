#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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
//   The other way round, a friend whose game alone runs slowed sends states spaced on the wall clock with the speeds of
//   their slowed game: their stand-in, and the NPCs their game runs, have their animation stepped at the pace that game
//   runs them at (PlayerRate, WorldRate; game/StandInTime.h), so their legs keep to the ground they cover. For that a
//   report carries, with the multiplier, how the game's own player is paced in it (PlayerFactor: Jet slows the world
//   and the one who took it by amounts of their own).
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

	// How many times its world's pace a game's own player runs at, from the engine's two slowdowns (VATS::magicTimeSlowdown,
	// the world's, and VATS::playerMagicTimeSlowdown, the player's): while a slowdown is on, the engine steps the player by
	// the world's frame time times the player's over the world's. 1 with none on, or when either is not a usable number;
	// never more than Protocol::MAX_PLAYER_TIME_FACTOR.
	constexpr float PlayerFactor(float a_magic, float a_playerMagic)
	{
		constexpr float most = std::numeric_limits<float>::max();
		if (!(a_magic > 0.0f) || !(a_playerMagic > 0.0f) || a_magic > most || a_playerMagic > most) {
			return 1.0f;
		}
		const float factor = a_playerMagic / a_magic;
		if (!(factor > 0.0f)) {
			return 1.0f;
		}
		return factor < Protocol::MAX_PLAYER_TIME_FACTOR ? factor : Protocol::MAX_PLAYER_TIME_FACTOR;
	}

	// Our own game this frame.
	struct Own
	{
		bool  vats = false;        // in VATS: its menu, or a shot playing out
		float multiplier = 1.0f;   // the global time multiplier it runs at
		float playerFactor = 1.0f; // how many times that pace our own player runs at (PlayerFactor)

		[[nodiscard]] std::uint8_t Flags() const
		{
			return static_cast<std::uint8_t>((vats ? Protocol::kTimeVats : 0) | (multiplier < FULL ? Protocol::kTimeSlowed : 0));
		}

		// The player's factor as it is said: 1 in VATS (the pace our player keeps while a shot plays out is the VATS
		// camera's, not this) and in a game that is not slowed (a slowdown still set for a frame after the multiplier is
		// back says nothing of our player's pace), and never a number the server would refuse.
		[[nodiscard]] float Factor() const
		{
			if (vats || !(multiplier < FULL) || !std::isfinite(playerFactor) || !(playerFactor > 0.0f)) {
				return 1.0f;
			}
			return (std::min)(playerFactor, Protocol::MAX_PLAYER_TIME_FACTOR);
		}
	};

	// When our own goes out: at once when VATS comes up or goes down, or the game becomes slowed or not; a multiplier,
	// or our player's factor, that moves within that, when it has moved CHANGE, MIN_GAP apart at most. Said again after
	// each welcome, and every RESEND while welcomed (the server passes on only a change, so the same word again costs the
	// others nothing).
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
			const Protocol::TimeScale now{ 0, a_own.Flags(), multiplier, a_own.Factor() };
			const bool                due = !sent || sent->flags != now.flags ||
			                 ((std::abs(sent->multiplier - now.multiplier) >= CHANGE || std::abs(sent->playerFactor - now.playerFactor) >= CHANGE) && a_now - sentAt >= MIN_GAP) ||
			                 a_now - sentAt >= RESEND;
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

	// The rate friends' games run at, as known here, for the step a stand-in's animation is played by
	// (game/StandInTime.h): the group's slow motion while it is in effect (a_group, from Group: every game runs at it),
	// full speed else. It is the rate a friend's game runs at unless that friend's row says otherwise (PlayerRate, WorldRate).
	constexpr float FriendsRate(float a_group)
	{
		return a_group > 0.0f && a_group < 1.0f ? a_group : 1.0f;
	}

	// A friend's rate as a stand-in's step goes by it (game/StandInTime.h): the common one (a_common, FriendsRate) when it
	// is not a usable rate (a stopped game says nothing of pace) or within CHANGE of it (rates are said CHANGE apart, and
	// a step that close to the engine's own is left the engine's), and never past Protocol::MAX_TIME_MULTIPLIER.
	constexpr float Settled(float a_rate, float a_common)
	{
		if (!(a_rate > 0.0f) || a_rate > std::numeric_limits<float>::max()) {
			return a_common;
		}
		const float rate = a_rate < Protocol::MAX_TIME_MULTIPLIER ? a_rate : Protocol::MAX_TIME_MULTIPLIER;
		const float apart = rate > a_common ? rate - a_common : a_common - rate;
		return apart <= CHANGE * a_common ? a_common : rate;
	}

	// The rate the world of a friend's game runs at by their row, before it is settled: their multiplier, and in the
	// group's slow motion (a_group, from Group) the group's speed unless their game is slower still on its own (their
	// row may not say the group's speed yet, and Jet below it is left as it is).
	constexpr float RowWorld(const Protocol::TimeScale& a_row, float a_group)
	{
		const bool grouped = a_group > 0.0f && a_group < 1.0f;
		return grouped && !(a_row.multiplier < a_group) ? a_group : a_row.multiplier;
	}

	// The rate the world of a friend's game runs at, for the step the NPCs their game runs are played by here: game
	// seconds a real second. The common rate while that friend is in VATS: a shot playing out paces its target and
	// the rest by the VATS camera's rules, which no row says.
	constexpr float WorldRate(const Protocol::TimeScale& a_row, float a_group)
	{
		const float common = FriendsRate(a_group);
		return (a_row.flags & Protocol::kTimeVats) != 0 ? common : Settled(RowWorld(a_row, a_group), common);
	}

	// The rate a friend's own player runs at, for the step their stand-in is played by here: their world's rate times
	// their player's factor in it (Jet slows the one who took it by an amount of its own). The common rate in VATS, as
	// for WorldRate.
	constexpr float PlayerRate(const Protocol::TimeScale& a_row, float a_group)
	{
		const float common = FriendsRate(a_group);
		if ((a_row.flags & Protocol::kTimeVats) != 0) {
			return common;
		}
		const float factor = a_row.playerFactor > 0.0f && a_row.playerFactor <= Protocol::MAX_PLAYER_TIME_FACTOR ? a_row.playerFactor : 1.0f;
		return Settled(RowWorld(a_row, a_group) * factor, common);
	}

	// A friend's row, if they have said one.
	inline const Protocol::TimeScale* RowOf(const Friends& a_friends, std::uint32_t a_playerId)
	{
		const auto it = std::ranges::find(a_friends.Rows(), a_playerId, &Protocol::TimeScale::playerId);
		return it != a_friends.Rows().end() ? &*it : nullptr;
	}

	// PlayerRate and WorldRate for a friend by id: the common rate for one who has said nothing (yet).
	inline float PlayerRate(const Friends& a_friends, std::uint32_t a_playerId, float a_group)
	{
		const auto row = RowOf(a_friends, a_playerId);
		return row ? PlayerRate(*row, a_group) : FriendsRate(a_group);
	}

	inline float WorldRate(const Friends& a_friends, std::uint32_t a_playerId, float a_group)
	{
		const auto row = RowOf(a_friends, a_playerId);
		return row ? WorldRate(*row, a_group) : FriendsRate(a_group);
	}

	// Whether any friend's player or world runs at another rate than the common one: only then does a stand-in's step
	// go by its own friend's rate (Puppets looks no puppet up while every game runs alike).
	inline bool Differ(const Friends& a_friends, float a_group)
	{
		const float common = FriendsRate(a_group);
		return std::ranges::any_of(a_friends.Rows(), [&](const Protocol::TimeScale& a_row) { return PlayerRate(a_row, a_group) != common || WorldRate(a_row, a_group) != common; });
	}
}
