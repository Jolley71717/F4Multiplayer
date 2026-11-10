#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Whose companion each NPC is: the server's book (protocol 64). Two games can recruit the same companion, Dogmeat
// first of all: each game's Followers quest fills its own alias. On the wire the companion is one NPC, so the first game
// to report recruiting them keeps them, and a second game's report is refused with the owner's name (that game
// dismisses its copy: src/game/Companions.cpp). A companion is free again when its owner's game reports letting them go
// (a dismissal in the game's own dialogue, or a save loaded without them), or when the owner leaves: a crashed or
// disconnected owner's companion is held for them for HOLD, so a rejoin (the same hello identity) finds them still
// theirs, and is free after. Nobody overrides an owner, the host included. Pure, so it is tested on its own and with
// the server (tests/CompanionBookTests.cpp, tests/ServerCompanionTests.cpp).
namespace CompanionBook
{
	using Clock = std::chrono::steady_clock;
	using State = Protocol::CompanionState;

	// How long a companion is kept for an owner who left without letting them go.
	constexpr auto HOLD = std::chrono::minutes(5);
	// Companions the book keeps for one player: the game's aliases hold two (a companion and Dogmeat); a game reporting
	// more is refused past this (kFull), so no client fills the book.
	constexpr std::size_t MAX_PER_PLAYER = 4;

	// A change of an NPC's owner, for everyone.
	using Change = Protocol::CompanionOwner;

	struct Holder
	{
		std::uint32_t     playerId = 0;
		std::uint64_t     identity = 0;  // their hello identity: a rejoin with it finds a held companion still theirs (0: none, freed on leave)
		bool              gone = false;  // they left; held until goneAt + HOLD
		Clock::time_point goneAt{};
	};

	class Book
	{
	public:
		// a_playerId's game reports its aliases hold a_refId. What the reporter is told (as a CompanionOwner: theirs,
		// another's, held for one who left, or full); a change for everyone else, if any, is appended to a_changes.
		[[nodiscard]] Change Recruit(std::uint32_t a_refId, std::uint32_t a_playerId, std::uint64_t a_identity, [[maybe_unused]] Clock::time_point a_now, std::vector<Change>& a_changes)
		{
			if (const auto it = holders.find(a_refId); it != holders.end()) {
				auto& holder = it->second;
				const bool theirs = holder.playerId == a_playerId || (holder.gone && a_identity != 0 && holder.identity == a_identity);
				if (!theirs) {
					return { a_refId, holder.playerId, holder.gone ? State::kHeld : State::kOwner };
				}
				if (holder.gone || holder.playerId != a_playerId) {
					// Theirs again (a rejoin under a new id, or the same id reporting again): everyone hears whose it is now.
					holder = { a_playerId, a_identity };
					a_changes.push_back({ a_refId, a_playerId, State::kOwner });
				}
				return { a_refId, a_playerId, State::kOwner };
			}
			if (CountOf(a_playerId) >= MAX_PER_PLAYER) {
				return { a_refId, 0, State::kFull };
			}
			holders[a_refId] = { a_playerId, a_identity };
			a_changes.push_back({ a_refId, a_playerId, State::kOwner });
			return { a_refId, a_playerId, State::kOwner };
		}

		// a_playerId's game reports its aliases no longer hold a_refId: free, if it was theirs (another's stays as it is).
		bool Dismiss(std::uint32_t a_refId, std::uint32_t a_playerId, std::vector<Change>& a_changes)
		{
			const auto it = holders.find(a_refId);
			if (it == holders.end() || it->second.playerId != a_playerId || it->second.gone) {
				return false;
			}
			holders.erase(it);
			a_changes.push_back({ a_refId, 0, State::kFree });
			return true;
		}

		// The player left: their companions are held for them (with an identity to come back under), or freed.
		void Leave(std::uint32_t a_playerId, Clock::time_point a_now, std::vector<Change>& a_changes)
		{
			for (auto it = holders.begin(); it != holders.end();) {
				auto& holder = it->second;
				if (holder.playerId != a_playerId || holder.gone) {
					++it;
					continue;
				}
				if (holder.identity == 0) {
					a_changes.push_back({ it->first, 0, State::kFree });
					it = holders.erase(it);
					continue;
				}
				holder.gone = true;
				holder.goneAt = a_now;
				a_changes.push_back({ it->first, holder.playerId, State::kHeld });
				++it;
			}
		}

		// A player welcomed with this identity: companions held for it are theirs again, under their new id.
		void Rejoin(std::uint64_t a_identity, std::uint32_t a_playerId, std::vector<Change>& a_changes)
		{
			if (a_identity == 0) {
				return;
			}
			for (auto& [refId, holder] : holders) {
				if (holder.gone && holder.identity == a_identity) {
					holder = { a_playerId, a_identity };
					a_changes.push_back({ refId, a_playerId, State::kOwner });
				}
			}
		}

		// Companions held for players who have been gone HOLD are free.
		void Expire(Clock::time_point a_now, std::vector<Change>& a_changes)
		{
			for (auto it = holders.begin(); it != holders.end();) {
				if (it->second.gone && a_now - it->second.goneAt >= HOLD) {
					a_changes.push_back({ it->first, 0, State::kFree });
					it = holders.erase(it);
				} else {
					++it;
				}
			}
		}

		// Whose a_refId is (null: nobody's).
		[[nodiscard]] const Holder* HolderOf(std::uint32_t a_refId) const
		{
			const auto it = holders.find(a_refId);
			return it != holders.end() ? &it->second : nullptr;
		}

		// Whether a_playerId, here now, owns a_refId.
		[[nodiscard]] bool Owns(std::uint32_t a_refId, std::uint32_t a_playerId) const
		{
			const auto holder = HolderOf(a_refId);
			return holder && !holder->gone && holder->playerId == a_playerId;
		}

		// Every companion's owner now (a joiner's list), in no particular order.
		[[nodiscard]] std::vector<Change> All() const
		{
			std::vector<Change> out;
			for (const auto& [refId, holder] : holders) {
				out.push_back({ refId, holder.playerId, holder.gone ? State::kHeld : State::kOwner });
			}
			return out;
		}

		[[nodiscard]] std::size_t CountOf(std::uint32_t a_playerId) const
		{
			return static_cast<std::size_t>(std::ranges::count_if(holders, [&](const auto& a_entry) { return a_entry.second.playerId == a_playerId && !a_entry.second.gone; }));
		}

		[[nodiscard]] std::size_t Size() const { return holders.size(); }

		void Clear() { holders.clear(); }

	private:
		std::unordered_map<std::uint32_t, Holder> holders;
	};
}
