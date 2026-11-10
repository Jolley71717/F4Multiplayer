#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// What our game says and does about companions (protocol 64), kept free of the game so it can be tested
// (tests/CompanionRulesTests.cpp); Companions.cpp reads the Followers quest's aliases and runs the dismissal.
//
// Each look, the aliases' contents (Companion, DogmeatCompanion) are compared with what was reported: an NPC they hold
// now is reported recruited, one they let go of is reported let go, and a report the server hasn't answered is made
// again after REPORT_RETRY. The server's word on an NPC (CompanionOwner): ours, and it stays; another player's (here, or
// held for one who left) while our aliases hold it, and our recruit was refused: the game dismisses our copy (the
// Followers script's quiet dismissal), a moment after the word (the recruit's own scripts finish first), again every
// DISMISS_RETRY while the aliases still hold it, DISMISS_TRIES times, then leaves it (a dismissal that never takes is
// logged, and nothing else is tried: the game is as it was before this feature). A word that names a player for an NPC
// we never recruited is news of theirs (the log, `net status`).
namespace CompanionRules
{
	using Clock = std::chrono::steady_clock;
	using State = Protocol::CompanionState;

	constexpr auto REPORT_RETRY = std::chrono::seconds(5);
	constexpr auto DISMISS_DELAY = std::chrono::seconds(1);
	constexpr auto DISMISS_RETRY = std::chrono::seconds(10);
	constexpr int  DISMISS_TRIES = 3;

	// Which of the Followers quest's aliases holds the NPC: the script dismisses each its own way.
	enum class Slot : std::uint8_t
	{
		kCompanion,
		kDogmeat,
	};

	struct Held
	{
		std::uint32_t id = 0;
		Slot          slot = Slot::kCompanion;
	};

	// What the server's word on an NPC means here.
	enum class Verdict : std::uint8_t
	{
		kNone,     // nothing new
		kOurs,     // our companion, granted
		kRefused,  // our aliases hold it and it is another's: dismiss our copy (once per refusal; the notice goes with this)
		kTheirs,   // another player's (we don't hold it)
		kHeld,     // held for a player who left (we don't hold it)
		kFree,     // nobody's now
		kFull,     // the server keeps no more for us: nothing is done (the game's aliases can't reach it)
	};

	struct Word
	{
		Verdict       verdict = Verdict::kNone;
		std::uint32_t playerId = 0;  // the owner named, if any
		bool          held = false;  // ... who left (kRefused: the notice says so)
	};

	struct Dismissal
	{
		std::uint32_t id = 0;
		Slot          slot = Slot::kCompanion;
		int           attempt = 0;  // 1-based
	};

	class Book
	{
	public:
		// Each look: what the aliases hold now. The reports to send (recruited, or let go).
		[[nodiscard]] std::vector<Protocol::CompanionReport> Look(const std::vector<Held>& a_held, Clock::time_point a_now)
		{
			std::vector<Protocol::CompanionReport> reports;
			for (const auto& held : a_held) {
				auto& entry = recruited[held.id];
				entry.slot = held.slot;
				if (!entry.reported || (!entry.answered && a_now - entry.reportedAt >= REPORT_RETRY)) {
					entry.reported = true;
					entry.reportedAt = a_now;
					reports.push_back({ held.id, true });
				}
			}
			for (auto it = recruited.begin(); it != recruited.end();) {
				const bool still = std::ranges::any_of(a_held, [&](const Held& a_h) { return a_h.id == it->first; });
				if (still) {
					++it;
					continue;
				}
				if (it->second.refused && !it->second.gaveUp) {
					++dismissed;  // the dismissal took (or the player let them go themselves): either way our aliases are free of it
				}
				reports.push_back({ it->first, false });
				it = recruited.erase(it);
			}
			return reports;
		}

		// The server's word (kCompanionOwner): what it means for us.
		[[nodiscard]] Word OnOwner(const Protocol::CompanionOwner& a_word, std::uint32_t a_localId, Clock::time_point a_now)
		{
			if (a_word.state == State::kFree) {
				owners.erase(a_word.refId);
			} else if (a_word.state != State::kFull) {
				owners[a_word.refId] = { a_word.playerId, a_word.state == State::kHeld };
			}
			const auto it = recruited.find(a_word.refId);
			Entry*     entry = it != recruited.end() ? &it->second : nullptr;
			if (entry) {
				entry->answered = true;
			}
			switch (a_word.state) {
			case State::kFull:
				if (entry) {
					entry->gaveUp = true;
				}
				return { Verdict::kFull };
			case State::kFree:
				return { Verdict::kFree };
			case State::kOwner:
			case State::kHeld:
				break;
			}
			const bool held = a_word.state == State::kHeld;
			if (a_word.playerId == a_localId && !held) {
				if (entry && !entry->ours) {
					entry->ours = true;
					entry->refused = false;
					++ours;
				}
				return { Verdict::kOurs, a_localId };
			}
			if (entry && !entry->refused && !entry->gaveUp) {
				entry->refused = true;
				entry->ours = false;
				entry->tries = 0;
				entry->nextDismissAt = a_now + DISMISS_DELAY;
				++refusals;
				return { Verdict::kRefused, a_word.playerId, held };
			}
			return { held ? Verdict::kHeld : Verdict::kTheirs, a_word.playerId, held };
		}

		// A dismissal due now, if any (the caller checks the player is out of the NPC's dialogue first, and calls
		// Dismissing once the call went).
		[[nodiscard]] std::optional<Dismissal> DueDismissal(Clock::time_point a_now) const
		{
			for (const auto& [id, entry] : recruited) {
				if (entry.refused && !entry.gaveUp && entry.tries < DISMISS_TRIES && a_now >= entry.nextDismissAt) {
					return Dismissal{ id, entry.slot, entry.tries + 1 };
				}
			}
			return std::nullopt;
		}

		void Dismissing(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto it = recruited.find(a_id);
			if (it == recruited.end()) {
				return;
			}
			++it->second.tries;
			it->second.nextDismissAt = a_now + DISMISS_RETRY;
			++dismissals;
		}

		// NPCs every dismissal of which went and the aliases still hold, DISMISS_RETRY after the last: given up on, each
		// said once.
		[[nodiscard]] std::vector<std::uint32_t> GivenUp(Clock::time_point a_now)
		{
			std::vector<std::uint32_t> out;
			for (auto& [id, entry] : recruited) {
				if (entry.refused && !entry.gaveUp && entry.tries >= DISMISS_TRIES && a_now >= entry.nextDismissAt) {
					entry.gaveUp = true;
					++gaveUp;
					out.push_back(id);
				}
			}
			return out;
		}

		// Whose the server last said a_id is (nothing: nobody's, or never said).
		[[nodiscard]] std::optional<std::uint32_t> OwnerOf(std::uint32_t a_id) const
		{
			const auto it = owners.find(a_id);
			return it != owners.end() ? std::optional{ it->second.playerId } : std::nullopt;
		}

		[[nodiscard]] bool IsOurs(std::uint32_t a_id) const
		{
			const auto it = recruited.find(a_id);
			return it != recruited.end() && it->second.ours;
		}

		// A save was loaded: what the aliases hold is reported again (the server answers as before; a refused one the save
		// still holds is dismissed again).
		void Reload() { recruited.clear(); }

		// The session ended.
		void Reset()
		{
			recruited.clear();
			owners.clear();
		}

		[[nodiscard]] std::size_t Recruited() const { return recruited.size(); }
		[[nodiscard]] std::size_t Known() const { return owners.size(); }

		[[nodiscard]] std::string Describe() const
		{
			return std::format("companions: held={} known={} ours={} refused={} dismissals={} dismissed={} gaveUp={}", recruited.size(), owners.size(), ours, refusals, dismissals, dismissed, gaveUp);
		}

	private:
		struct Entry
		{
			Slot              slot = Slot::kCompanion;
			bool              reported = false;
			bool              answered = false;
			bool              ours = false;
			bool              refused = false;
			bool              gaveUp = false;
			int               tries = 0;
			Clock::time_point reportedAt{};
			Clock::time_point nextDismissAt{};
		};

		struct Owner
		{
			std::uint32_t playerId = 0;
			bool          held = false;
		};

		std::unordered_map<std::uint32_t, Entry> recruited;  // what our aliases hold, as reported
		std::unordered_map<std::uint32_t, Owner> owners;     // whose the server says each NPC is
		std::uint32_t                            ours = 0;
		std::uint32_t                            refusals = 0;
		std::uint32_t                            dismissals = 0;  // dismissal calls made
		std::uint32_t                            dismissed = 0;   // ... after which the aliases let the NPC go
		std::uint32_t                            gaveUp = 0;
	};
}
