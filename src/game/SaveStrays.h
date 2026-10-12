#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Which of the game's created actors SaveGuard keeps out of a save (no game needed, so it is tested).
namespace SaveStrays
{
	// A created actor SaveGuard looked at.
	struct Actor
	{
		bool inCell = false;
		bool hasBase = false;  // its base form still exists
		bool deleted = false;
		bool ours = false;     // a stand-in this run placed, or one on a base stand-ins are placed from
	};

	// A created actor in no cell is a stray when its base is gone (a stand-in an earlier version saved,
	// on a face copy no save keeps) or when it is ours and deleted. The rest are left alone: the game
	// keeps created actors in no cell (leveled ones whose cell unloaded, disabled ones a quest holds
	// back), and one the game or a script deleted keeps its record on purpose (its id stays taken while
	// the record exists, so a script still holding it never finds another form under that id).
	constexpr bool Stray(const Actor& a_actor)
	{
		return !a_actor.inCell && (!a_actor.hasBase || (a_actor.ours && a_actor.deleted));
	}

	// The stand-ins placed this game run, by form ID, with the base each was placed from. The game hands
	// ids out again (after a load it counts on from the save's last one), so an id is only ours while
	// the form under it has the base ours had.
	class Placed
	{
	public:
		void Add(std::uint32_t a_id, std::uint32_t a_base)
		{
			bases[a_id] = a_base;
			any = true;
		}

		[[nodiscard]] bool Ours(std::uint32_t a_id, std::uint32_t a_base) const
		{
			const auto it = bases.find(a_id);
			return it != bases.end() && it->second == a_base;
		}

		// Whether this run ever placed one (a load does not change that).
		[[nodiscard]] bool Any() const { return any; }

		// After a load: the stand-ins from before it are gone, but for the ones the save brought back.
		void KeepOnly(const std::vector<std::uint32_t>& a_ids)
		{
			std::erase_if(bases, [&](const auto& a_entry) { return std::ranges::find(a_ids, a_entry.first) == a_ids.end(); });
		}

		[[nodiscard]] std::size_t Size() const { return bases.size(); }

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> bases;
		bool                                             any = false;
	};

	// Whether the forms are searched for strays before a save. Only once this run placed a stand-in, or
	// when the last load brought strays back (a save an earlier version broke, healed by the next save):
	// for a game that never had a session, a save costs nothing.
	constexpr bool SearchBeforeSave(bool a_placedAny, bool a_loadBroughtStrays)
	{
		return a_placedAny || a_loadBroughtStrays;
	}
}
