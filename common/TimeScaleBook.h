#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// The server's table of who is in VATS and how fast each game runs (kReportTimeScale in, kTimeScales out), with the
// host's group VATS speed. Each game reports its own on each change; the table goes to everyone else on each change and
// to a joiner after its welcome. Free of the network, so it is tested.
namespace TimeScaleBook
{
	// The host's group VATS speed as the wire carries it: 0 (off) for anything that would not slow a game (off, not a
	// number, 1 or more); else at least Protocol::MIN_GROUP_VATS_SPEED, so a friend's game is never all but stopped.
	inline float GroupSpeed(bool a_on, float a_speed)
	{
		if (!a_on || !std::isfinite(a_speed) || a_speed >= 1.0f) {
			return 0.0f;
		}
		return (std::max)(a_speed, Protocol::MIN_GROUP_VATS_SPEED);
	}

	class Book
	{
	public:
		// A player's report (their game's flags and multiplier): false when it changes nothing, or the table is full.
		bool Report(std::uint32_t a_playerId, std::uint8_t a_flags, float a_multiplier)
		{
			const Protocol::TimeScale row{ a_playerId, a_flags, a_multiplier };
			const auto                it = std::ranges::find(players, a_playerId, &Protocol::TimeScale::playerId);
			if (it != players.end()) {
				if (*it == row) {
					return false;
				}
				*it = row;
				return true;
			}
			if (players.size() >= Protocol::MAX_TIME_SCALES) {
				return false;
			}
			players.push_back(row);
			return true;
		}

		// A player left: true if they had said anything (the others are told the table without them).
		bool Leave(std::uint32_t a_playerId) { return std::erase_if(players, [&](const Protocol::TimeScale& a_row) { return a_row.playerId == a_playerId; }) != 0; }

		void Clear() { players.clear(); }

		[[nodiscard]] Protocol::TimeScales Table(float a_groupSpeed) const { return { a_groupSpeed, players }; }

		// Players in VATS now (for the log).
		[[nodiscard]] std::size_t InVats() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(players, [](const Protocol::TimeScale& a_row) { return (a_row.flags & Protocol::kTimeVats) != 0; }));
		}

	private:
		std::vector<Protocol::TimeScale> players;  // in the order they first reported
	};
}
