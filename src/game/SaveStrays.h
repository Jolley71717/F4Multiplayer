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
		bool ours = false;     // a stand-in this run placed, or one placed as stand-ins are (see FromStandInBase)
	};

	// In no cell, and deleted or without its base. Whether it is a stray or the game's own depends on whose it
	// is (see Stray).
	constexpr bool Gone(const Actor& a_actor)
	{
		return !a_actor.inCell && (!a_actor.hasBase || a_actor.deleted);
	}

	// A created actor in no cell is a stray when its base is gone (a stand-in an earlier version saved,
	// on a face copy no save keeps) or when it is ours and deleted. The rest are left alone: the game
	// keeps created actors in no cell (leveled ones whose cell unloaded, disabled ones a quest holds
	// back), and one the game or a script deleted keeps its record on purpose (its id stays taken while
	// the record exists, so a script still holding it never finds another form under that id).
	constexpr bool Stray(const Actor& a_actor)
	{
		return Gone(a_actor) && (!a_actor.hasBase || a_actor.ours);
	}

	// Form IDs the game hands out at runtime (FF......).
	constexpr bool IsCreatedId(std::uint32_t a_id)
	{
		return (a_id >> 24) == 0xFF;
	}

	// The base a created actor was placed from. A leveled actor's own base is a copy the game makes for
	// it; what was placed is in its leveled data (0 when that names nothing).
	struct Origin
	{
		std::uint32_t base = 0;
		bool          leveled = false;
		std::uint32_t leveledFrom = 0;
	};

	constexpr std::uint32_t PlacedFrom(const Origin& a_origin)
	{
		return a_origin.leveled ? a_origin.leveledFrom : a_origin.base;
	}

	// Whether an actor not placed this run was placed as stand-ins are, by an earlier version or before
	// a load: from a base stand-ins are placed from (a_standInBase says which: the default settlers, the
	// ini's iPuppetBaseForm and iMyAppearance, the looks friends asked for), or from a base made at
	// runtime (a face copy). The game makes runtime bases of its own for leveled actors; those count by
	// what was placed (a leveled list or an NPC of the game's), so the game's leveled actors are not ours.
	template <class F>
	constexpr bool FromStandInBase(const Origin& a_origin, F&& a_standInBase)
	{
		const auto from = PlacedFrom(a_origin);
		return from != 0 && (IsCreatedId(from) || a_standInBase(from));
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
	// when the last load brought back gone actors (strays from a save an earlier version broke, healed by
	// the next save, or ones that turn out to be strays once a friend's look is known): for a game that
	// never had a session, a save costs nothing.
	constexpr bool SearchBeforeSave(bool a_placedAny, bool a_loadBroughtGone)
	{
		return a_placedAny || a_loadBroughtGone;
	}
}
