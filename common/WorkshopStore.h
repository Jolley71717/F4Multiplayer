#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// What the server keeps of settlement building for whoever joins later: what each player built
// (placed or moved: the last report of each), and the pieces of the world itself (the junk a
// settlement starts with) someone scrapped. Each kind has its own cap, so the junk a host's save had
// scrapped, reported each time it hosts, can't take the room for what players build. And what each
// builder's world said it lacks (lost: WorkshopAudit), out of everyone's world but kept in case that world has it again.
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

	// How many lost pieces are kept (Gone), by default.
	static constexpr std::size_t MAX_LOST = 2048;

	WorkshopStore(std::size_t a_maxBuilt, std::size_t a_maxScraps, std::size_t a_maxLost = MAX_LOST) :
		maxBuilt(a_maxBuilt), maxScraps(a_maxScraps), maxLost(a_maxLost)
	{}

	// A player's report (its playerId already set by the server, and its character: whose world it was, 0 not known yet).
	// A report of a piece kept as lost is its builder's world having it again (found, or built anew under that ID).
	Outcome Report(const Protocol::WorkshopItem& a_item)
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (a_item.op == Protocol::WorkshopOp::kPlaced) {
			if (!built.contains(key) && built.size() >= maxBuilt) {
				return { .full = true };
			}
			built[key] = a_item;
			lost.erase(key);
			return { .relay = true };
		}
		if (a_item.op != Protocol::WorkshopOp::kScrapped) {
			return {};  // kLost is the server's own
		}
		lost.erase(key);
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
		if (a_item.op == Protocol::WorkshopOp::kPlaced) {
			return !built.contains(key);
		}
		return a_item.op == Protocol::WorkshopOp::kScrapped && Protocol::IsShareableRef(a_item.refId) && !scraps.contains(key) && !built.contains(key);
	}

	// What the player's game, playing a_character's world, is to look for in it (WorkshopAudit), after their welcome and
	// each load: what they built there that is kept (not a piece of the world they moved, kept as built too), and what that
	// world said it lacked (kLost: it comes back if found standing). Only a scrap from its builder takes a built piece out,
	// and a game reports no scrap of a piece it lacks: one its builder's game lost (a save from before it was built loaded)
	// would stay for everyone else, at every join, with nobody able to take it out. Only that character's: the player ID
	// is the PC's (Hello::identity), not the character's, and another character's world has none of them. Nothing for a
	// character not known (0), and nothing ever for a piece whose character isn't (reported by an older build, or before
	// the builder's record said whose world it was).
	[[nodiscard]] std::vector<Protocol::WorkshopItem> BuiltBy(std::uint32_t a_player, std::uint64_t a_character) const
	{
		std::vector<Protocol::WorkshopItem> out;
		if (a_character == 0) {
			return out;
		}
		for (auto it = built.lower_bound({ a_player, 0 }); it != built.end() && it->first.first == a_player; ++it) {
			if (!Protocol::IsShareableRef(it->first.second) && it->second.character == a_character) {
				out.push_back(it->second);
			}
		}
		for (auto it = lost.lower_bound({ a_player, 0 }); it != lost.end() && it->first.first == a_player; ++it) {
			if (it->second.item.character == a_character) {
				out.push_back(it->second.item);
			}
		}
		return out;
	}

	// The builder's game, playing a_character's world, says its world lacks the piece (a_item: its playerId set by the
	// server, the base its game looked for). Taken out if what is kept is still that (the same base: a piece its game built
	// since under the same reference ID, already reported, stays) and that world's (a_character, known), and the scrap to
	// pass on to the others. Never a piece of the world: neither a scrap, kept apart, nor one its player moved. Not
	// forgotten: kept as lost (MAX_LOST, the longest lost going first), so its builder's game finds it if it stands again.
	std::optional<Protocol::WorkshopItem> Gone(const Protocol::WorkshopItem& a_item, std::uint64_t a_character)
	{
		if (Protocol::IsShareableRef(a_item.refId) || a_character == 0) {
			return std::nullopt;
		}
		const auto it = built.find({ a_item.playerId, a_item.refId });
		if (it == built.end() || it->second.base != a_item.base || it->second.character != a_character) {
			return std::nullopt;
		}
		auto kept = it->second;
		built.erase(it);
		if (lost.size() >= maxLost && !lost.empty()) {
			lost.erase(std::ranges::min_element(lost, {}, [](const auto& a_entry) { return a_entry.second.order; }));
		}
		kept.op = Protocol::WorkshopOp::kLost;
		lost[{ kept.playerId, kept.refId }] = { kept, ++lostOrder };
		auto scrap = kept;
		scrap.op = Protocol::WorkshopOp::kScrapped;
		scrap.character = 0;
		return scrap;
	}

	// The player's game says whose world it plays now (a_character), for the pieces it reported before saying so
	// (a_refs: since its welcome or its load): theirs, unless a report since said otherwise.
	void Tag(std::uint32_t a_player, const std::vector<std::uint32_t>& a_refs, std::uint64_t a_character)
	{
		for (const auto ref : a_refs) {
			if (const auto it = built.find({ a_player, ref }); it != built.end() && it->second.character == 0) {
				it->second.character = a_character;
			}
		}
	}

	// An item from the saved session, kept as it was.
	void Restore(const Protocol::WorkshopItem& a_item)
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (a_item.op == Protocol::WorkshopOp::kLost) {
			if (!Protocol::IsShareableRef(a_item.refId) && (lost.contains(key) || lost.size() < maxLost)) {
				lost[key] = { a_item, ++lostOrder };
			}
			return;
		}
		(a_item.op == Protocol::WorkshopOp::kScrapped ? scraps : built)[key] = a_item;
	}

	[[nodiscard]] const Items& Built() const { return built; }
	[[nodiscard]] const Items& Scraps() const { return scraps; }
	[[nodiscard]] std::size_t  LostCount() const { return lost.size(); }
	[[nodiscard]] bool         IsLost(const Key& a_key) const { return lost.contains(a_key); }
	// What is sent to whoever joins and counts against the caps: the lost pieces are neither.
	[[nodiscard]] std::size_t  Size() const { return built.size() + scraps.size(); }

	// Built objects first, then scraps (what a joiner is sent).
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

	// All that is kept, for the session file: ForEach's, then the lost pieces (kLost), the longest lost first.
	template <class F>
	void ForEachKept(F&& a_visit) const
	{
		ForEach(a_visit);
		std::vector<const std::pair<const Key, Lost>*> order;
		for (const auto& entry : lost) {
			order.push_back(&entry);
		}
		std::ranges::sort(order, {}, [](const auto* a_entry) { return a_entry->second.order; });
		for (const auto* entry : order) {
			a_visit(entry->first, entry->second.item);
		}
	}

	void Clear()
	{
		built.clear();
		scraps.clear();
		lost.clear();
	}

private:
	struct Lost
	{
		Protocol::WorkshopItem item;   // kLost
		std::uint64_t          order;  // when it was lost (the longest lost goes first)
	};

	std::size_t              maxBuilt;
	std::size_t              maxScraps;
	std::size_t              maxLost;
	Items                    built;   // what players built, by builder
	Items                    scraps;  // pieces of the world scrapped, by who scrapped them
	std::map<Key, Lost>      lost;    // what players built that their world said it lacked (Gone)
	std::uint64_t            lostOrder = 0;
};
