#pragma once

#include "game/DeferredDelete.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// The references WorkshopSync buried this session (a friend's copy the host scrapped, a wire of ours taken off, a piece of ours
// whose build the host answered): switched off, marked deleted and their record of changes dropped. That alone did not keep
// them buried: a deleted copy still in its cell when the cell unloaded had a fresh record written for it by the unload, and
// the cell's next load made it again as a live piece (a friend's generator the host scrapped, back and powered in the friend's
// game, and in their save, after a trip away). SaveGuard keeps the record out of saves; this book finds one that came back all
// the same and has it buried again. Free of the game, so it is unit tested; WorkshopSync reads the game for it (Facts).
namespace BuriedPieces
{
	using Clock = std::chrono::steady_clock;

	inline constexpr std::size_t MAX = 1024;         // a settlement's worth of copies at a leave: the oldest go unwatched
	inline constexpr float       SAME_SPOT = 8.0f;   // game units: one made again from its record stands where it was buried
	inline constexpr std::uint8_t MAX_BURIALS = 3;   // buried again at most this often; then left to SaveGuard (kept out of saves)

	// What the game has under a buried id now, as WorkshopSync reads it.
	struct Facts
	{
		bool          present = false;    // a reference under the id
		bool          hasRecord = false;  // the engine's list of changes holds a record for the id (asked only with none present)
		std::uint32_t base = 0;           // the reference's base
		bool          deleted = false;    // marked for deletion (still buried)
		bool          ours = false;       // a piece in use: a copy we keep, one waiting in the graveyard, or a workshop object of this game
		float         fromSpot = 0.0f;    // from where it was buried
		bool          has3D = false;
	};

	enum class Verdict
	{
		kKeep,       // still buried (or gone with its record still there, which its cell may bring back): looked at again
		kWait,       // back alive, its 3D still loading: buried again once that is safe (DeferredDelete::Safe)
		kBuryAgain,  // back alive: bury it again
		kGiveUp,     // back alive again and again: said once and left to SaveGuard, which keeps it out of every save
		kForget,     // the id is free (no form and no record), or names something else now: not ours to touch
	};

	struct Entry
	{
		std::uint32_t                    id = 0;
		std::uint32_t                    base = 0;
		std::array<float, 3>             at{};
		std::optional<Clock::time_point> aliveSince;  // first found back alive (not deleted), for its 3D's grace
		std::uint8_t                     burials = 0;  // buried again this session
	};

	// A_sinceAlive: how long it has been found back alive (zero the first time).
	constexpr Verdict Judge(const Entry& a_entry, const Facts& a_facts, Clock::duration a_sinceAlive)
	{
		if (!a_facts.present) {
			// The engine frees the id only once no form and no record are under it; until then its cell could bring it back.
			return a_facts.hasRecord ? Verdict::kKeep : Verdict::kForget;
		}
		if (a_facts.base != a_entry.base) {
			return Verdict::kForget;  // the id names another form now
		}
		if (a_facts.deleted) {
			return Verdict::kKeep;
		}
		if (a_facts.ours || a_facts.fromSpot > SAME_SPOT) {
			// One of the same base under the id that is a piece in use (the id handed out again to a copy or a build of ours), or
			// one standing elsewhere: not the buried one back from its record.
			return Verdict::kForget;
		}
		if (a_entry.burials >= MAX_BURIALS) {
			return Verdict::kGiveUp;
		}
		return DeferredDelete::Safe(a_facts.has3D, a_sinceAlive) ? Verdict::kBuryAgain : Verdict::kWait;
	}

	class Book
	{
	public:
		// A reference was buried (again): watched from now on. True the first time this session (to be said once).
		bool Add(std::uint32_t a_id, std::uint32_t a_base, std::array<float, 3> a_at)
		{
			if (a_id == 0) {
				return false;
			}
			const auto it = std::ranges::find(entries, a_id, &Entry::id);
			if (it != entries.end()) {
				it->base = a_base;
				it->at = a_at;
				it->aliveSince.reset();
				return false;
			}
			if (entries.size() >= MAX) {
				entries.erase(entries.begin());
			}
			entries.push_back({ a_id, a_base, a_at, std::nullopt, 0 });
			return true;
		}

		// Each entry judged as the game has it now (a_look(entry) gives its Facts). Forgotten and given-up ones leave the book;
		// those to bury again are counted as buried again here (WorkshopSync's Bury then calls Add, which keeps the count).
		// What was found back alive, with its verdict (kWait, kBuryAgain, kGiveUp), for WorkshopSync to act on and say. Each one
		// forgotten is told to a_forgot(entry) (SaveGuard lets go of it too: the id is not a buried one of ours any more).
		template <class Look, class Forgot>
		std::vector<std::pair<Entry, Verdict>> Check(Look&& a_look, Clock::time_point a_now, Forgot&& a_forgot)
		{
			std::vector<std::pair<Entry, Verdict>> found;
			std::vector<Entry>                     kept;
			kept.reserve(entries.size());
			for (auto& entry : entries) {
				const Facts facts = a_look(std::as_const(entry));
				const bool  alive = facts.present && facts.base == entry.base && !facts.deleted;
				if (!alive) {
					entry.aliveSince.reset();
				} else if (!entry.aliveSince) {
					entry.aliveSince = a_now;
				}
				const auto verdict = Judge(entry, facts, alive ? a_now - *entry.aliveSince : Clock::duration{});
				switch (verdict) {
				case Verdict::kKeep:
					kept.push_back(entry);
					break;
				case Verdict::kForget:
					a_forgot(std::as_const(entry));
					break;
				case Verdict::kWait:
					found.emplace_back(entry, verdict);
					kept.push_back(entry);
					break;
				case Verdict::kBuryAgain:
					++entry.burials;
					entry.aliveSince.reset();
					found.emplace_back(entry, verdict);
					kept.push_back(entry);
					break;
				case Verdict::kGiveUp:
					found.emplace_back(entry, verdict);
					break;
				}
			}
			entries = std::move(kept);
			return found;
		}

		template <class Look>
		std::vector<std::pair<Entry, Verdict>> Check(Look&& a_look, Clock::time_point a_now)
		{
			return Check(a_look, a_now, [](const Entry&) {});
		}

		[[nodiscard]] bool        Contains(std::uint32_t a_id) const { return std::ranges::find(entries, a_id, &Entry::id) != entries.end(); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }
		void                      Clear() { entries.clear(); }

	private:
		std::vector<Entry> entries;  // oldest first
	};
}
