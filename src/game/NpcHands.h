#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// Who runs each NPC, as the server tells it (NpcSync::ApplyOwners), and how often that changes. Every change of hands
// restarts the NPC's AI in the game that takes it, from where that game's copy stood (a delay and a trip behind the
// game that had it), and makes a copy of the one that had it, which then jumps to where the new runner's stands: an
// NPC two players both shoot used to (each hit took it over, Server.cpp's cooldown between: now the runner's own hits
// claim it again and keep it, NpcSync::OnLocalInteraction), and one on the edge of a distance hand-off still does,
// stopping and setting off again at each change. Counted for `net status` (`hands=`) and the log: a change
// within QUICK of the last is a quick one. A release (nobody runs it) is remembered for QUICK, so the claim that follows
// a hand-off counts as the change it is. Pure, so it can be tested.
namespace NpcHands
{
	using Clock = std::chrono::steady_clock;

	constexpr auto        QUICK = std::chrono::seconds(10);  // twice the server's cooldown between takeovers
	constexpr std::size_t MAX_TRACKED = 4096;                // NPCs remembered: as many as one game may run

	enum class Change
	{
		kNone,     // the same runner as before, or nobody for one never run
		kFirst,    // its first runner in this session
		kChanged,  // another runner
		kQuick,    // another runner within QUICK of the last change
		kFreed,    // nobody runs it now
	};

	struct Turn
	{
		Change          change = Change::kNone;
		std::uint32_t   from = 0;  // who ran it before (0: nobody)
		Clock::duration since{};   // since the last change, for kChanged and kQuick
	};

	class Book
	{
	public:
		// The server named a_owner (0: nobody) for a_id at a_now.
		Turn Note(std::uint32_t a_id, std::uint32_t a_owner, Clock::time_point a_now)
		{
			const auto it = hands.find(a_id);
			if (it == hands.end()) {
				if (a_owner == 0 || hands.size() >= MAX_TRACKED) {
					return {};
				}
				hands.emplace(a_id, Hands{ a_owner, a_now });
				return { Change::kFirst };
			}
			auto& entry = it->second;
			if (entry.owner == a_owner) {
				return {};
			}
			Turn turn{ Change::kFreed, entry.owner, a_now - entry.at };
			if (a_owner != 0) {
				const bool quick = turn.since < QUICK;
				turn.change = quick ? Change::kQuick : Change::kChanged;
				++changes;
				quickOnes += quick;
			}
			entry = { a_owner, a_now };
			return turn;
		}

		// Drops releases long past: nothing of those is a change any more.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(hands, [&](const auto& a_entry) { return a_entry.second.owner == 0 && a_now - a_entry.second.at >= QUICK; });
		}

		void Clear()
		{
			hands.clear();
			changes = 0;
			quickOnes = 0;
		}

		[[nodiscard]] std::uint32_t Changes() const { return changes; }
		[[nodiscard]] std::uint32_t Quick() const { return quickOnes; }
		[[nodiscard]] std::size_t   Tracked() const { return hands.size(); }

	private:
		struct Hands
		{
			std::uint32_t     owner = 0;
			Clock::time_point at{};
		};

		std::unordered_map<std::uint32_t, Hands> hands;
		std::uint32_t                            changes = 0;
		std::uint32_t                            quickOnes = 0;
	};
}
