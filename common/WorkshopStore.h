#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

// What the server keeps of settlement building for whoever joins later: what each player built
// (placed or moved: the last report of each), and the pieces of the world itself (the junk a
// settlement starts with) someone scrapped. Each kind has its own cap, so the junk a host's save had
// scrapped, reported each time it hosts, can't take the room for what players build.
class WorkshopStore
{
public:
	using Key = std::pair<std::uint32_t, std::uint32_t>;  // (player, their reference ID)
	using Items = std::map<Key, Protocol::WorkshopItem>;

	// What became of a report.
	struct Outcome
	{
		bool relay = false;  // the others hear of it
		bool full = false;   // its kind's cap is reached: it isn't kept
	};

	WorkshopStore(std::size_t a_maxBuilt, std::size_t a_maxScraps) :
		maxBuilt(a_maxBuilt), maxScraps(a_maxScraps)
	{}

	// A player's report (its playerId already set by the server).
	Outcome Report(const Protocol::WorkshopItem& a_item)
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (a_item.op != Protocol::WorkshopOp::kScrapped) {
			if (!built.contains(key) && built.size() >= maxBuilt) {
				return { .full = true };
			}
			built[key] = a_item;
			return { .relay = true };
		}
		// Already gone for everyone. The host reports the junk its save had scrapped each time it
		// hosts, and a saved session remembers the last report: forgetting it here would bring the
		// junk back for whoever joins next.
		if (scraps.contains(key)) {
			return {};
		}
		if (!Protocol::IsShareableRef(a_item.refId)) {
			return { .relay = built.erase(key) > 0 };  // something they built, or nothing anyone knew about
		}
		// A piece of the world itself: it's in every game, so it goes from everyone's world, theirs who
		// join later too. One its player had moved first was kept as built (the others moved theirs
		// there): the scrap replaces it, or a joiner would get the move and not the scrap.
		const bool wasMoved = built.erase(key) > 0;
		if (scraps.size() >= maxScraps) {
			return { .relay = wasMoved, .full = true };  // not kept, but the copy of a moved one still goes
		}
		scraps[key] = a_item;
		return { .relay = true };
	}

	// Whether the report would keep one more thing, rather than replace, drop or skip what is kept (a
	// player's share of the store counts only these).
	[[nodiscard]] bool Adds(const Protocol::WorkshopItem& a_item) const
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (a_item.op != Protocol::WorkshopOp::kScrapped) {
			return !built.contains(key);
		}
		return Protocol::IsShareableRef(a_item.refId) && !scraps.contains(key) && !built.contains(key);
	}

	// An item from the saved session, kept as it was.
	void Restore(const Protocol::WorkshopItem& a_item)
	{
		(a_item.op == Protocol::WorkshopOp::kScrapped ? scraps : built)[{ a_item.playerId, a_item.refId }] = a_item;
	}

	[[nodiscard]] const Items& Built() const { return built; }
	[[nodiscard]] const Items& Scraps() const { return scraps; }
	[[nodiscard]] std::size_t  Size() const { return built.size() + scraps.size(); }

	// Built objects first, then scraps.
	template <class F>
	void ForEach(F&& a_visit) const
	{
		for (const auto& [key, item] : built) {
			a_visit(key, item);
		}
		for (const auto& [key, item] : scraps) {
			a_visit(key, item);
		}
	}

	void Clear()
	{
		built.clear();
		scraps.clear();
	}

private:
	std::size_t maxBuilt;
	std::size_t maxScraps;
	Items       built;   // what players built, by builder
	Items       scraps;  // pieces of the world scrapped, by who scrapped them
};
