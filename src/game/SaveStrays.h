#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// Which of the game's created references SaveGuard keeps out of a save (no game needed, so it is tested).
namespace SaveStrays
{
	// A created reference SaveGuard looked at: an actor, or anything else placed at runtime (a stand-in's
	// frame copy or light, or one of the game's own).
	struct Actor
	{
		bool inCell = false;
		bool hasBase = false;  // its base form still exists
		bool deleted = false;
		bool ours = false;     // a stand-in, frame copy or light this run placed, or one marked or placed as stand-ins are (see StandIn)
		bool anchored = true;  // the engine's save code finds a cell or a worldspace for it (see Doomed)
	};

	// One the engine cannot save: for a created reference with a record of changes, the save (and the
	// engine unloading it, as a demoted actor's cell goes) asks for its cell, or else its worldspace, and
	// reads that without a check. With neither, it crashes, whoever made the reference and whether or not it
	// is deleted. So it is a stray whoever made it: losing its record beats a save that cannot be written.
	constexpr bool Doomed(const Actor& a_actor)
	{
		return !a_actor.anchored;
	}

	// In no cell, and deleted or without its base. Whether it is a stray or the game's own depends on whose it
	// is (see Stray).
	constexpr bool Gone(const Actor& a_actor)
	{
		return !a_actor.inCell && (!a_actor.hasBase || a_actor.deleted);
	}

	// A created reference is a stray when it is doomed (see Doomed), or in no cell and its base is gone (a
	// stand-in an earlier version saved, on a face copy no save keeps) or ours and deleted. The rest are
	// left alone: the game keeps created actors in no cell (leveled ones whose cell unloaded, disabled ones a
	// quest holds back), and one the game or a script deleted keeps its record on purpose (its id stays
	// taken while the record exists, so a script still holding it never finds another form under that id).
	constexpr bool Stray(const Actor& a_actor)
	{
		return Doomed(a_actor) || (Gone(a_actor) && (!a_actor.hasBase || a_actor.ours));
	}

	// Form IDs the game hands out at runtime (FF......).
	constexpr bool IsCreatedId(std::uint32_t a_id)
	{
		return (a_id >> 24) == 0xFF;
	}

	// Every stand-in is put in the player's faction at this rank when it is placed (RemotePlayers::Spawn,
	// every version since 0.4.0, with the console's addtofaction). On a base that is not unique (every
	// stand-in's but a unique iPuppetBaseForm's) the engine keeps that change on the reference itself
	// (ExtraFactionChanges, under change flag 0x80000000) and saves and loads it with the reference's other
	// extra data, deleted or not. That is read from the game's code (1.11.240) and not yet seen in a game.
	// So it marks a stand-in an earlier version saved without its friend's look having to be known.
	inline constexpr std::uint32_t STAND_IN_FACTION = 0x0001C21C;  // PlayerFaction
	inline constexpr int           STAND_IN_RANK = 1;

	// Whether a faction change on a reference (the faction's form ID, the rank) is the stand-ins' one. One
	// of the game's created actors that is deleted, in no cell and carries it is unlikely (a player's
	// console addtofaction could give it one); it would lose only the record of an actor already gone.
	constexpr bool StandInFaction(std::uint32_t a_faction, int a_rank)
	{
		return a_faction == STAND_IN_FACTION && a_rank == STAND_IN_RANK;
	}

	// What tells whose a created actor is. The base it was placed from: a leveled actor's own base is a
	// copy the game makes for it; what was placed is in its leveled data (0 when that names nothing). And
	// whether the reference carries the stand-ins' faction change (see StandInFaction).
	struct Origin
	{
		std::uint32_t base = 0;
		bool          leveled = false;
		std::uint32_t leveledFrom = 0;
		bool          standInFaction = false;
	};

	constexpr std::uint32_t PlacedFrom(const Origin& a_origin)
	{
		return a_origin.leveled ? a_origin.leveledFrom : a_origin.base;
	}

	// Whether an actor not placed this run was placed as stand-ins are, by an earlier version or before
	// a load: from a base stand-ins are placed from (a_standInBase says which: the default settlers, the
	// ini's iPuppetBaseForm, the looks friends asked for this run), or from a base made at runtime (a face
	// copy). The game makes runtime bases of its own for leveled actors; those count by what was placed (a
	// leveled list or an NPC of the game's), so the game's leveled actors are not ours. A stand-in placed
	// straight on a friend's look is known this way only once that friend has joined this run.
	template <class F>
	constexpr bool FromStandInBase(const Origin& a_origin, F&& a_standInBase)
	{
		const auto from = PlacedFrom(a_origin);
		return from != 0 && (IsCreatedId(from) || a_standInBase(from));
	}

	// Whether an actor not placed this run is a stand-in all the same: it carries the stand-ins' faction
	// change (which needs no friend to have joined), or it was placed from a stand-in base.
	template <class F>
	constexpr bool StandIn(const Origin& a_origin, F&& a_standInBase)
	{
		return a_origin.standInFaction || FromStandInBase(a_origin, a_standInBase);
	}

	// The stand-ins, their frame copies and their lights placed this game run, by form ID, with the base each was
	// placed from. The game hands ids out again (after a load it counts on from the save's last one), so an id is
	// only ours while the form under it has the base ours had.
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

		// The base the id was placed from, while it is ours.
		[[nodiscard]] std::optional<std::uint32_t> BaseOf(std::uint32_t a_id) const
		{
			const auto it = bases.find(a_id);
			return it != bases.end() ? std::optional{ it->second } : std::nullopt;
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

	// When SaveGuard looks for strays: before a save, or right after a load.
	enum class When
	{
		kSave,
		kLoad,
	};

	enum class Verdict
	{
		kLeave,    // not a stray
		kExclude,  // its record is dropped
		kTell,     // only logged
	};

	// A stray is kept out of every save. Right after a load only ours are: one of the game's may be in the middle of
	// getting its cell back as the load settles, so it is logged then and left to the sweep before the next save.
	constexpr Verdict Judge(const Actor& a_actor, When a_when)
	{
		if (!Stray(a_actor)) {
			return Verdict::kLeave;
		}
		return a_when == When::kSave || a_actor.ours ? Verdict::kExclude : Verdict::kTell;
	}

	// References of ours the game took from under us (their handle stopped resolving) while their form may still be
	// there: kept out of saves for as long as it is, and is the one we placed (the game hands ids out again). The
	// oldest go past MAX; a load forgets them all (the ids name other forms then).
	class Forgotten
	{
	public:
		static constexpr std::size_t MAX = 64;

		void Add(std::uint32_t a_id, std::uint32_t a_base)
		{
			if (a_id == 0 || std::ranges::find(entries, std::pair{ a_id, a_base }) != entries.end()) {
				return;
			}
			if (entries.size() >= MAX) {
				entries.erase(entries.begin());
			}
			entries.emplace_back(a_id, a_base);
		}

		// Keeps the ones whose form is still there with the base it was placed from: a_baseNow(id) gives the base of
		// the form under the id now, or nothing when there is none. Calls a_kept(id) for each one kept.
		template <class F, class G>
		void Keep(F&& a_baseNow, G&& a_kept)
		{
			std::erase_if(entries, [&](const std::pair<std::uint32_t, std::uint32_t>& a_entry) {
				const std::optional<std::uint32_t> base = a_baseNow(a_entry.first);
				if (!base || *base != a_entry.second) {
					return true;
				}
				a_kept(a_entry.first);
				return false;
			});
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;  // id, base
	};
}
