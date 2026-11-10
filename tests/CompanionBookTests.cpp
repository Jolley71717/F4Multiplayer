// The server's book of whose companion each NPC is (common/CompanionBook.h): the first to recruit keeps them, a
// dismissal frees, a leave holds for a rejoin then frees, and nobody ever holds a companion another holds.

#include "Test.h"

#include "CompanionBook.h"

#include <algorithm>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <unordered_map>

using namespace std::chrono_literals;
using CompanionBook::Book;
using CompanionBook::Change;
using State = Protocol::CompanionState;

namespace
{
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t PIPER = 0x0002F1F8;
	constexpr std::uint32_t NICK = 0x00002F25;

	const auto T0 = CompanionBook::Clock::time_point{} + 1h;

	// What a_changes say of a_ref last (nothing: no change named it).
	std::optional<Change> LastOf(const std::vector<Change>& a_changes, std::uint32_t a_ref)
	{
		const auto it = std::ranges::find_if(a_changes | std::views::reverse, [&](const Change& a_change) { return a_change.refId == a_ref; });
		return it != a_changes.rend() ? std::optional{ *it } : std::nullopt;
	}
}

TEST("companion book: the first to recruit keeps the companion; the second is told whose it is")
{
	Book                book;
	std::vector<Change> changes;
	const auto          first = book.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	CHECK(first.state == State::kOwner);
	CHECK(first.playerId == 1);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].refId == DOGMEAT && changes[0].playerId == 1 && changes[0].state == State::kOwner);

	changes.clear();
	const auto second = book.Recruit(DOGMEAT, 2, 0x22, T0 + 1s, changes);
	CHECK(second.state == State::kOwner);
	CHECK(second.playerId == 1);  // theirs, not ours
	CHECK(changes.empty());       // nothing changed for anyone
	CHECK(book.Owns(DOGMEAT, 1));
	CHECK(!book.Owns(DOGMEAT, 2));

	// The owner reporting again (a load, a repeat) is still the owner, and nobody hears of it.
	changes.clear();
	CHECK(book.Recruit(DOGMEAT, 1, 0x11, T0 + 2s, changes).playerId == 1);
	CHECK(changes.empty());
	CHECK(book.Size() == 1);
}

TEST("companion book: a dismissal frees the companion only from its owner")
{
	Book                book;
	std::vector<Change> changes;
	(void)book.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	changes.clear();
	CHECK(!book.Dismiss(DOGMEAT, 2, changes));  // not theirs to let go
	CHECK(changes.empty());
	CHECK(book.Owns(DOGMEAT, 1));
	CHECK(book.Dismiss(DOGMEAT, 1, changes));
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].state == State::kFree && changes[0].playerId == 0);
	CHECK(!book.HolderOf(DOGMEAT));
	CHECK(!book.Dismiss(DOGMEAT, 1, changes));  // nothing to let go twice

	// Free, the next to recruit gets them.
	changes.clear();
	CHECK(book.Recruit(DOGMEAT, 2, 0x22, T0 + 5s, changes).playerId == 2);
	CHECK(book.Owns(DOGMEAT, 2));
}

TEST("companion book: an owner who leaves has the companion held for a rejoin, and it is free after the hold")
{
	Book                book;
	std::vector<Change> changes;
	(void)book.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	changes.clear();
	book.Leave(1, T0 + 10s, changes);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].state == State::kHeld && changes[0].playerId == 1);
	CHECK(!book.Owns(DOGMEAT, 1));  // not here
	REQUIRE(book.HolderOf(DOGMEAT));
	CHECK(book.HolderOf(DOGMEAT)->gone);

	// Another player's recruit while held: refused, told it is held for player 1.
	changes.clear();
	const auto answer = book.Recruit(DOGMEAT, 2, 0x22, T0 + 20s, changes);
	CHECK(answer.state == State::kHeld && answer.playerId == 1);
	CHECK(changes.empty());

	// Just before the hold is up: still held; at it: free, and everyone hears.
	book.Expire(T0 + 10s + CompanionBook::HOLD - 1ms, changes);
	CHECK(changes.empty());
	CHECK(book.HolderOf(DOGMEAT));
	book.Expire(T0 + 10s + CompanionBook::HOLD, changes);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].state == State::kFree);
	CHECK(!book.HolderOf(DOGMEAT));
	CHECK(book.Recruit(DOGMEAT, 2, 0x22, T0 + 20min, changes).playerId == 2);
}

TEST("companion book: a rejoin under the same identity finds a held companion still theirs, under the new id")
{
	Book                book;
	std::vector<Change> changes;
	(void)book.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	(void)book.Recruit(PIPER, 1, 0x11, T0, changes);
	book.Leave(1, T0 + 10s, changes);
	changes.clear();
	// At the welcome: both theirs again, under id 5.
	book.Rejoin(0x11, 5, changes);
	REQUIRE(changes.size() == 2);
	CHECK(std::ranges::all_of(changes, [](const Change& a_change) { return a_change.playerId == 5 && a_change.state == State::kOwner; }));
	CHECK(book.Owns(DOGMEAT, 5) && book.Owns(PIPER, 5));
	CHECK(!book.Owns(DOGMEAT, 1));
	// Their game's report after the load: theirs, nothing new for anyone.
	changes.clear();
	CHECK(book.Recruit(DOGMEAT, 5, 0x11, T0 + 1min, changes).playerId == 5);
	CHECK(changes.empty());
	// Nothing expires for a player who is back.
	book.Expire(T0 + 1h, changes);
	CHECK(changes.empty());
	CHECK(book.Size() == 2);

	// Another identity's rejoin finds nothing.
	Book other;
	(void)other.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	other.Leave(1, T0, changes);
	changes.clear();
	other.Rejoin(0x22, 6, changes);
	CHECK(changes.empty());
	CHECK(other.HolderOf(DOGMEAT)->gone);
}

TEST("companion book: a report under a held identity, before the welcome's rejoin, is theirs too")
{
	// The server rejoins at the welcome; a report that reaches the book first (a test, a race) is the same identity's.
	Book                book;
	std::vector<Change> changes;
	(void)book.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	book.Leave(1, T0, changes);
	changes.clear();
	const auto answer = book.Recruit(DOGMEAT, 9, 0x11, T0 + 1s, changes);
	CHECK(answer.state == State::kOwner && answer.playerId == 9);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].playerId == 9 && changes[0].state == State::kOwner);
	CHECK(book.Owns(DOGMEAT, 9));
}

TEST("companion book: an owner with no identity to come back under frees the companion as they leave")
{
	Book                book;
	std::vector<Change> changes;
	(void)book.Recruit(DOGMEAT, 1, 0, T0, changes);
	changes.clear();
	book.Leave(1, T0 + 10s, changes);
	REQUIRE(changes.size() == 1);
	CHECK(changes[0].state == State::kFree);
	CHECK(!book.HolderOf(DOGMEAT));
	book.Rejoin(0, 5, changes);  // no identity rejoins nothing
	CHECK(changes.size() == 1);
}

TEST("companion book: a player keeps at most MAX_PER_PLAYER companions; past it the report is refused as full")
{
	Book                book;
	std::vector<Change> changes;
	for (std::uint32_t i = 0; i < CompanionBook::MAX_PER_PLAYER; ++i) {
		CHECK(book.Recruit(0x00010000 + i, 1, 0x11, T0, changes).state == State::kOwner);
	}
	const auto full = book.Recruit(0x00020000, 1, 0x11, T0, changes);
	CHECK(full.state == State::kFull && full.playerId == 0);
	CHECK(book.Size() == CompanionBook::MAX_PER_PLAYER);
	// Another player is not full for it.
	CHECK(book.Recruit(0x00020000, 2, 0x22, T0, changes).playerId == 2);
	// Letting one go makes room.
	CHECK(book.Dismiss(0x00010000, 1, changes));
	CHECK(book.Recruit(0x00030000, 1, 0x11, T0, changes).state == State::kOwner);
}

TEST("companion book: a joiner's list names every companion's owner, held ones as held")
{
	Book                book;
	std::vector<Change> changes;
	(void)book.Recruit(DOGMEAT, 1, 0x11, T0, changes);
	(void)book.Recruit(PIPER, 2, 0x22, T0, changes);
	book.Leave(2, T0, changes);
	const auto all = book.All();
	REQUIRE(all.size() == 2);
	const auto dog = LastOf(all, DOGMEAT);
	const auto piper = LastOf(all, PIPER);
	REQUIRE(dog && piper);
	CHECK(dog->playerId == 1 && dog->state == State::kOwner);
	CHECK(piper->playerId == 2 && piper->state == State::kHeld);
	book.Clear();
	CHECK(book.All().empty());
}

TEST("companion book: with five players recruiting, dismissing, leaving and rejoining at random, nobody ever holds another's companion and every answer tells the truth")
{
	// A model of the book: each companion's owner (and whether gone, since when), checked after every step.
	std::mt19937 rng{ 20261110 };
	for (int round = 0; round < 40; ++round) {
		Book                book;
		std::vector<Change> changes;
		struct Model
		{
			std::uint32_t                   playerId = 0;
			std::uint64_t                   identity = 0;
			bool                            gone = false;
			CompanionBook::Clock::time_point goneAt{};
		};
		std::unordered_map<std::uint32_t, Model> model;
		std::unordered_map<std::uint32_t, bool>  here;      // player id -> in the session
		std::unordered_map<std::uint32_t, std::uint64_t> identityOf;
		std::uint32_t nextId = 1;
		const std::uint64_t identities[] = { 0x11, 0x22, 0x33, 0x44, 0 };  // the fifth has none
		std::unordered_map<std::uint64_t, std::uint32_t> idOfIdentity;
		for (std::size_t i = 0; i < 5; ++i) {
			const auto id = nextId++;
			here[id] = true;
			identityOf[id] = identities[i];
			if (identities[i] != 0) {
				idOfIdentity[identities[i]] = id;
			}
		}
		const std::uint32_t npcs[] = { DOGMEAT, PIPER, NICK };
		auto now = T0;
		for (int step = 0; step < 200; ++step) {
			now += std::chrono::seconds(rng() % 90);
			changes.clear();
			std::vector<std::uint32_t> present;
			for (const auto& [id, in] : here) {
				if (in) {
					present.push_back(id);
				}
			}
			std::ranges::sort(present);
			const auto npc = npcs[rng() % 3];
			switch (rng() % 5) {
			case 0:
			case 1:
				if (!present.empty()) {
					const auto player = present[rng() % present.size()];
					const auto identity = identityOf[player];
					const auto answer = book.Recruit(npc, player, identity, now, changes);
					const auto it = model.find(npc);
					if (it == model.end()) {
						model[npc] = { player, identity };
						CHECK(answer.state == State::kOwner && answer.playerId == player);
					} else if (it->second.playerId == player || (it->second.gone && identity != 0 && it->second.identity == identity)) {
						it->second = { player, identity };
						CHECK(answer.state == State::kOwner && answer.playerId == player);
					} else {
						CHECK(answer.playerId == it->second.playerId);
						CHECK(answer.state == (it->second.gone ? State::kHeld : State::kOwner));
					}
				}
				break;
			case 2:
				if (!present.empty()) {
					const auto player = present[rng() % present.size()];
					const bool freed = book.Dismiss(npc, player, changes);
					const auto it = model.find(npc);
					const bool owns = it != model.end() && it->second.playerId == player && !it->second.gone;
					CHECK(freed == owns);
					if (owns) {
						model.erase(it);
					}
				}
				break;
			case 3:
				if (present.size() > 1) {
					const auto player = present[rng() % present.size()];
					book.Leave(player, now, changes);
					here[player] = false;
					for (auto it = model.begin(); it != model.end();) {
						if (it->second.playerId == player && !it->second.gone) {
							if (it->second.identity == 0) {
								it = model.erase(it);
								continue;
							}
							it->second.gone = true;
							it->second.goneAt = now;
						}
						++it;
					}
				}
				break;
			case 4:
				{
					// Someone gone comes back under a new id (their identity's), or a fresh player arrives.
					std::vector<std::uint32_t> gone;
					for (const auto& [id, in] : here) {
						if (!in) {
							gone.push_back(id);
						}
					}
					std::ranges::sort(gone);
					if (!gone.empty()) {
						const auto old = gone[rng() % gone.size()];
						const auto identity = identityOf[old];
						const auto id = nextId++;
						here[id] = true;
						identityOf[id] = identity;
						book.Rejoin(identity, id, changes);
						for (auto& [ref, holder] : model) {
							if (holder.gone && identity != 0 && holder.identity == identity) {
								holder = { id, identity };
							}
						}
					}
				}
				break;
			}
			book.Expire(now, changes);
			for (auto it = model.begin(); it != model.end();) {
				if (it->second.gone && now - it->second.goneAt >= CompanionBook::HOLD) {
					it = model.erase(it);
				} else {
					++it;
				}
			}
			// The book is the model: same owners, same holds. Nobody holds what another holds (one holder per NPC by
			// construction), and no player holds one for a player who is here.
			CHECK(book.Size() == model.size());
			for (const auto& [ref, holder] : model) {
				const auto real = book.HolderOf(ref);
				REQUIRE(real);
				CHECK(real->playerId == holder.playerId);
				CHECK(real->gone == holder.gone);
				CHECK(real->identity == holder.identity);
				CHECK(here[holder.playerId] != holder.gone);
			}
			// The last change said of an NPC this step is the book's state after it.
			std::set<std::uint32_t> named;
			for (const auto& change : changes) {
				named.insert(change.refId);
			}
			for (const auto ref : named) {
				const auto last = LastOf(changes, ref);
				const auto real = book.HolderOf(ref);
				REQUIRE(last);
				if (last->state == State::kFree) {
					CHECK(!real);
				} else {
					REQUIRE(real);
					CHECK(real->playerId == last->playerId);
					CHECK(real->gone == (last->state == State::kHeld));
				}
			}
		}
	}
}
