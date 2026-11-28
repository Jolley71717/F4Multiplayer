#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
		// Who the others hear it built by, and the key it is kept under: the player, or the builder id of their second
		// character's pieces (BuilderOf).
		std::uint32_t builder = 0;
	};

	// How many lost pieces are kept (Gone), by default.
	static constexpr std::size_t MAX_LOST = 2048;

	// Two characters of one PC share a player id (it is the PC's), and each character's world numbers what it builds on
	// its own: the second's piece can have the id a piece of the first's has. Kept under the player's id it took the
	// first's place, for everyone and for good. Such a piece is kept, and told to the others, as built by a builder id
	// of its own: one per (player, character) that ever needed it, which no player has (the high bit, a number, the
	// player's id). To the other games it is the piece of a player who isn't in the session, which they keep copies of
	// as of anyone's who left. Everything else stays under the player's id, so a PC with one character, and every piece
	// kept before, is told as it always was.
	static constexpr std::uint32_t BUILDER_BASE = 0x8000'0000;
	static constexpr std::uint32_t BUILDER_PLAYER_MASK = 0xFFFF;  // a player past this (never, in practice) gets none
	static constexpr std::uint32_t BUILDER_NUMBERS = 0x7FFF;      // per player

	[[nodiscard]] static constexpr bool IsBuilderId(std::uint32_t a_id) { return a_id >= BUILDER_BASE; }
	// The player whose pieces an id's are: a builder id's player, or the id itself.
	[[nodiscard]] static constexpr std::uint32_t PlayerOf(std::uint32_t a_id) { return IsBuilderId(a_id) ? a_id & BUILDER_PLAYER_MASK : a_id; }

	WorkshopStore(std::size_t a_maxBuilt, std::size_t a_maxScraps, std::size_t a_maxLost = MAX_LOST) :
		maxBuilt(a_maxBuilt), maxScraps(a_maxScraps), maxLost(a_maxLost)
	{}

	// The builder id of the character's pieces that are kept apart, 0 while none ever was.
	[[nodiscard]] std::uint32_t BuilderIdOf(std::uint32_t a_player, std::uint64_t a_character) const
	{
		const auto it = a_character != 0 ? builders.find({ a_player, a_character }) : builders.end();
		return it != builders.end() ? it->second : 0;
	}

	// Who a piece of the player's, in the world of a_character (0: not known), is kept and told as built by: the
	// character's builder id if the piece is kept apart, the player otherwise (kept or not).
	[[nodiscard]] std::uint32_t BuilderOf(std::uint32_t a_player, std::uint64_t a_character, std::uint32_t a_ref) const
	{
		if (const auto id = BuilderIdOf(a_player, a_character); id != 0 && (built.contains({ id, a_ref }) || lost.contains({ id, a_ref }))) {
			return id;
		}
		return a_player;
	}

	// A player's report (its playerId already set by the server, and its character: whose world it was, 0 not known yet).
	// A report of a piece kept as lost is its builder's world having it again (found, or built anew under that ID).
	// A piece placed under an id that a piece of another character of theirs has is kept apart from now on (its
	// outcome's builder); a scrap never takes out another character's piece.
	Outcome Report(const Protocol::WorkshopItem& a_report)
	{
		auto a_item = a_report;
		Key  key{ BuilderOf(a_report.playerId, a_report.character, a_report.refId), a_report.refId };
		if (OfAnotherCharacter(key, a_report.character)) {
			const auto id = a_item.op == Protocol::WorkshopOp::kPlaced ? MakeBuilderId(a_report.playerId, a_report.character) : 0;
			if (id != 0) {
				key.first = id;
			} else if (a_item.op == Protocol::WorkshopOp::kScrapped) {
				return { .builder = key.first };  // a piece of this world nobody knew about: the other world's stays
			}
			// (No builder id to be had: it takes the other's place, as it always did.)
		}
		a_item.playerId = key.first;
		if (a_item.op == Protocol::WorkshopOp::kPlaced) {
			if (!built.contains(key) && built.size() >= maxBuilt) {
				return { .full = true, .builder = key.first };
			}
			built[key] = a_item;
			lost.erase(key);
			return { .relay = true, .builder = key.first };
		}
		if (a_item.op != Protocol::WorkshopOp::kScrapped) {
			return { .builder = key.first };  // kLost is the server's own
		}
		lost.erase(key);
		// Already gone for everyone. The host reports the junk its save had scrapped each time it
		// hosts, and a saved session remembers the last report: forgetting it here would bring the
		// junk back for whoever joins next.
		if (scraps.contains(key)) {
			return { .builder = key.first };
		}
		if (!Protocol::IsShareableRef(a_item.refId)) {
			return { .relay = built.erase(key) > 0, .builder = key.first };  // something they built, or nothing anyone knew about
		}
		// A piece of the world itself: it's in every game, so it goes from everyone's world, theirs who
		// join later too. One its player had moved first was kept as built (the others moved theirs
		// there): the scrap replaces it, or a joiner would get the move and not the scrap.
		const bool wasMoved = built.erase(key) > 0;
		if (scraps.size() >= maxScraps) {
			return { .relay = wasMoved, .full = true, .builder = key.first };  // not kept, but the copy of a moved one still goes
		}
		scraps[key] = a_item;
		return { .relay = true, .builder = key.first };
	}

	// Whether the report would keep one more thing, rather than replace, drop or skip what is kept (a
	// player's share of the store counts only these).
	[[nodiscard]] bool Adds(const Protocol::WorkshopItem& a_item) const
	{
		const Key key{ BuilderOf(a_item.playerId, a_item.character, a_item.refId), a_item.refId };
		if (a_item.op == Protocol::WorkshopOp::kPlaced) {
			return !built.contains(key) || (OfAnotherCharacter(key, a_item.character) && CanMakeBuilderId(a_item.playerId, a_item.character));
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
		// Under the player's id, and under the character's builder id (the pieces kept apart): named to their game as the
		// player's, by the ids its world has them under.
		for (const auto builder : { a_player, BuilderIdOf(a_player, a_character) }) {
			if (builder == 0) {
				continue;
			}
			for (auto it = built.lower_bound({ builder, 0 }); it != built.end() && it->first.first == builder; ++it) {
				if (!Protocol::IsShareableRef(it->first.second) && it->second.character == a_character) {
					out.push_back(it->second);
					out.back().playerId = a_player;
				}
			}
			for (auto it = lost.lower_bound({ builder, 0 }); it != lost.end() && it->first.first == builder; ++it) {
				if (it->second.item.character == a_character) {
					out.push_back(it->second.item);
					out.back().playerId = a_player;
				}
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
		const auto it = built.find({ BuilderOf(a_item.playerId, a_character, a_item.refId), a_item.refId });
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

	// An item from the saved session, kept as it was. One kept apart (its playerId a builder id) says whose builder id
	// that is: the file holds no list of them, and one with no piece left is free again.
	void Restore(const Protocol::WorkshopItem& a_item)
	{
		const Key key{ a_item.playerId, a_item.refId };
		if (IsBuilderId(a_item.playerId) && a_item.character != 0) {
			const auto player = PlayerOf(a_item.playerId);
			builders.emplace(std::pair{ player, a_item.character }, a_item.playerId);  // the first to say so, if a file ever held two
			auto& next = nextBuilder[player];
			next = (std::max)(next, ((a_item.playerId - BUILDER_BASE) >> 16) + 1);
		}
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
	// Whether a piece is kept under the key, built or lost: a container change may name it (a built holder:
	// Protocol::ContainerChange::builder).
	[[nodiscard]] bool         Knows(const Key& a_key) const { return built.contains(a_key) || lost.contains(a_key); }

	// The builder a change at the built holder a_key (a piece kept under it, or one that was) is told to a_player's game, which
	// plays a_character's world (0: not known yet). Anyone else's piece: the key's own builder, as the piece's kWorkshopItem
	// was relayed and that game's copy is keyed. One of the player's own (their id, or a builder id of theirs): their own
	// player id when it is of the world they play (kept with that character, or with none known on either side), which their
	// game takes for its own piece; Protocol::BUILT_ELSEWHERE when it is another character's world's, which their game has
	// nothing of.
	[[nodiscard]] std::uint32_t BuilderFor(const Key& a_key, std::uint32_t a_player, std::uint64_t a_character) const
	{
		if (a_key.first == 0 || PlayerOf(a_key.first) != a_player) {
			return a_key.first;
		}
		const Protocol::WorkshopItem* kept = nullptr;
		if (const auto it = built.find(a_key); it != built.end()) {
			kept = &it->second;
		} else if (const auto gone = lost.find(a_key); gone != lost.end()) {
			kept = &gone->second.item;
		}
		const bool here = kept ? kept->character == 0 || a_character == 0 || kept->character == a_character :
		                         a_key.first == a_player || a_key.first == BuilderIdOf(a_player, a_character);
		return here ? a_player : Protocol::BUILT_ELSEWHERE;
	}
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
		builders.clear();
		nextBuilder.clear();
	}

private:
	struct Lost
	{
		Protocol::WorkshopItem item;   // kLost
		std::uint64_t          order;  // when it was lost (the longest lost goes first)
	};

	// Whether what is kept under the key (built, or lost) is a piece another character's world built: both characters
	// known and not the same. Never a piece of the world itself (the same reference in every world).
	[[nodiscard]] bool OfAnotherCharacter(const Key& a_key, std::uint64_t a_character) const
	{
		if (a_character == 0 || Protocol::IsShareableRef(a_key.second)) {
			return false;
		}
		const auto other = [&](const Protocol::WorkshopItem& a_kept) { return a_kept.character != 0 && a_kept.character != a_character; };
		if (const auto it = built.find(a_key); it != built.end()) {
			return other(it->second);
		}
		const auto it = lost.find(a_key);
		return it != lost.end() && other(it->second.item);
	}

	[[nodiscard]] bool CanMakeBuilderId(std::uint32_t a_player, std::uint64_t a_character) const
	{
		if (a_player == 0 || a_player > BUILDER_PLAYER_MASK || a_character == 0) {
			return false;
		}
		const auto next = nextBuilder.find(a_player);
		return builders.contains({ a_player, a_character }) || next == nextBuilder.end() || next->second <= BUILDER_NUMBERS;
	}

	// The character's builder id, made if it has none: 0 if none can be had.
	std::uint32_t MakeBuilderId(std::uint32_t a_player, std::uint64_t a_character)
	{
		if (!CanMakeBuilderId(a_player, a_character)) {
			return 0;
		}
		auto& id = builders[{ a_player, a_character }];
		if (id == 0) {
			id = BUILDER_BASE | (nextBuilder[a_player]++ << 16) | a_player;
		}
		return id;
	}

	std::size_t              maxBuilt;
	std::size_t              maxScraps;
	std::size_t              maxLost;
	Items                    built;   // what players built, by builder
	Items                    scraps;  // pieces of the world scrapped, by who scrapped them
	std::map<Key, Lost>      lost;    // what players built that their world said it lacked (Gone)
	std::uint64_t            lostOrder = 0;
	// The builder ids given so far: (player, character) to the id, and each player's next number.
	std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> builders;
	std::map<std::uint32_t, std::uint32_t>                           nextBuilder;
};
