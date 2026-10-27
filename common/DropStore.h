#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

// The server's book of items players gave each other by dropping them (Protocol::DroppedItem): what lies in the session
// for anyone to take, and what was taken by whom. The server is what decides who gets an item: a take is granted to the
// first player who asks while it lies there, and to nobody after, so two players pressing Take at once can't both have
// it. Each game only ever takes an item once its grant comes (src/game/Drops.h).
//
// What a game's save holds is the other half: a save from before a player dropped something has it in their inventory
// again, and a save from before they took something lacks it. Each game says what its world holds after its welcome
// and after every load (Protocol::DropRecord), and Reconcile makes the book agree with it: what that world holds
// (an item it took, or its own drop it never swapped out) is that player's, lying nowhere; what the book had them hold
// but their world lacks lies there again. An owner's drop back in their world is kept as theirs, so a later load of a
// save after the drop lays it down again. The one case left is an item two worlds hold, as when someone else took a drop
// and its owner loads a save from before dropping it: both have it then (counted, dupRisk).
//
// Only the saves of one character hold what that character's world gave and took: each record says whose world it is
// (DropRecord::character), and each drop keeps the characters whose worlds it left and went into. A record of another
// character's world (a new character, a coc one from the main menu, another character's save) is no save from before
// those: it lays nothing that character took down again and takes back nothing it gave, so a player who starts over
// doesn't get back what their older character took, while that character's own older save still does. A character not
// known (0: an older build's record, a file from before them) matches any, as before.
//
// A new drop is an offer at first, to its dropper's game alone: it lies there for the others only once that game says it
// has swapped the item it dropped for a copy (Confirm), so nobody can take an item that is still in its dropper's world.
// An offer is never taken or listed, and goes when its dropper picked the item up again (Keep), or leaves (DropOffers): a
// loose drop's then, as its dropper's game gives the item back to its world as it leaves. A gift's (a drop put in a
// container, holder set: src/game/GiftBook.h) waits for its giver's next record instead, and is saved with the session:
// their game gives nothing back on a link that went, so only its record says whether the item left its world.
//
// Kept free of the server's loop so it can be tested; saved with the session (SessionFile).
class DropStore
{
public:
	struct Limits
	{
		std::size_t lying = 1024;                           // items lying in the session at once
		std::size_t taken = Protocol::MAX_DROP_RECORD;      // takes remembered (for a taker's reload); the oldest go first
		std::size_t perOwner = 256;                         // items one player's game may have lying at once
	};

	enum class Outcome : std::uint8_t
	{
		kAdded,
		kRepeat,     // the same drop reported again (its nonce): the one kept is the answer
		kFull,       // the session has Limits::lying
		kOwnerFull,  // this player has Limits::perOwner lying
		kInvalid,    // no nonce
	};

	struct Added
	{
		Outcome                               outcome = Outcome::kInvalid;
		std::optional<Protocol::DroppedItem> drop;  // as kept: kAdded and kRepeat
	};

	// What a game's record changed (Reconcile).
	struct Reconciled
	{
		std::vector<Protocol::DropGone>    withdrawn;  // lying no more: in this player's world (takerId 0: their own, back with them)
		std::vector<Protocol::DroppedItem> returned;   // not with their taker: lying there again
		std::vector<Protocol::DroppedItem> confirmed;  // their offers their world swapped out after all: lying there now
		std::vector<Protocol::DropGone>    unoffered;  // their offers their world still has: gone (for them alone to hear)
		std::size_t                        dupRisk = 0;  // their world holds one someone else holds too
		bool                               changed = false;  // the book changed (to be saved)
	};

	DropStore() = default;
	explicit DropStore(const Limits& a_limits) :
		limits(a_limits)
	{}

	// Whether a record of a_record's character's world speaks for what a_kept's did (see the class).
	[[nodiscard]] static constexpr bool SameCharacter(std::uint64_t a_kept, std::uint64_t a_record)
	{
		return a_kept == 0 || a_record == 0 || a_kept == a_record;
	}

	// a_player's game plays a_character's world now (their last record says so, answered yet or not): their drops and takes
	// from here on are that world's.
	void SetCharacter(std::uint32_t a_player, std::uint64_t a_character) { characters[a_player] = a_character; }

	[[nodiscard]] std::uint64_t CharacterOf(std::uint32_t a_player) const
	{
		const auto it = characters.find(a_player);
		return it == characters.end() ? 0 : it->second;
	}

	// A player's drop (a_item as their game reported it; its ids are set here): an offer to them until they Confirm it.
	Added Add(std::uint32_t a_owner, Protocol::DroppedItem a_item)
	{
		if (a_item.nonce == 0) {
			return {};
		}
		for (const auto* book : { &offers, &lying }) {
			for (const auto& [id, drop] : *book) {
				if (drop.ownerId == a_owner && drop.nonce == a_item.nonce) {
					return { Outcome::kRepeat, drop };
				}
			}
		}
		for (const auto& [id, drop] : taken) {
			if (drop.ownerId == a_owner && drop.nonce == a_item.nonce) {
				return { Outcome::kRepeat, std::nullopt };  // taken already: nothing to answer
			}
		}
		if (lying.size() + offers.size() >= limits.lying) {
			return { Outcome::kFull, std::nullopt };
		}
		if (LyingOf(a_owner) >= limits.perOwner) {
			return { Outcome::kOwnerFull, std::nullopt };
		}
		a_item.dropId = nextId++;
		a_item.ownerId = a_owner;
		a_item.takerId = 0;
		a_item.ownerCharacter = CharacterOf(a_owner);
		a_item.takerCharacter = 0;
		offers.emplace(a_item.dropId, a_item);
		return { Outcome::kAdded, a_item };
	}

	// a_owner's game swapped the item it dropped for a copy: the offer lies there for anyone now (to be told to the others).
	std::optional<Protocol::DroppedItem> Confirm(std::uint32_t a_owner, std::uint32_t a_dropId)
	{
		const auto it = offers.find(a_dropId);
		if (it == offers.end() || it->second.ownerId != a_owner) {
			return std::nullopt;
		}
		auto drop = std::move(it->second);
		offers.erase(it);
		lying[drop.dropId] = drop;
		return drop;
	}

	// a_owner's player picked the item up again before the offer came: it never lay there.
	bool Keep(std::uint32_t a_owner, std::uint32_t a_dropId)
	{
		const auto it = offers.find(a_dropId);
		if (it == offers.end() || it->second.ownerId != a_owner) {
			return false;
		}
		offers.erase(it);
		return true;
	}

	// a_owner left: their loose drops' offers go (their game gives those items back to its world as it leaves). Their gifts'
	// offers stay for their next record to settle (Reconcile): their game may have taken the item out of its world before
	// the link went, and gives nothing back.
	std::size_t DropOffers(std::uint32_t a_owner)
	{
		return static_cast<std::size_t>(std::erase_if(offers, [&](const auto& a_entry) { return a_entry.second.ownerId == a_owner && a_entry.second.holder == 0; }));
	}

	// a_player's game asks to take the item: granted while it lies there, to whoever asks first (never an offer).
	std::optional<Protocol::DropGone> Take(std::uint32_t a_player, std::uint32_t a_dropId)
	{
		const auto it = lying.find(a_dropId);
		if (it == lying.end() || a_player == 0) {
			return std::nullopt;
		}
		auto drop = std::move(it->second);
		lying.erase(it);
		drop.takerId = a_player;
		drop.takerCharacter = CharacterOf(a_player);
		Remember(std::move(drop));
		return Protocol::DropGone{ a_dropId, a_player };
	}

	// a_player's game could not finish a take granted to it: the item lies there again.
	std::optional<Protocol::DroppedItem> GiveBack(std::uint32_t a_player, std::uint32_t a_dropId)
	{
		const auto it = taken.find(a_dropId);
		if (it == taken.end() || it->second.takerId != a_player) {
			return std::nullopt;
		}
		return Lay(it);
	}

	// What a_player's world holds (their game's DropRecord) against the book, in session a_session (see the class).
	Reconciled Reconcile(std::uint32_t a_player, const Protocol::DropRecord& a_record, std::uint64_t a_session)
	{
		Reconciled out;
		// A record of another session (or none) is of a save from before this one: it holds none of its transfers.
		const bool        ours = a_record.sessionId == a_session && a_session != 0;
		const std::unordered_set<std::uint64_t> made = ours ? std::unordered_set<std::uint64_t>(a_record.made.begin(), a_record.made.end()) : std::unordered_set<std::uint64_t>{};
		const std::unordered_set<std::uint32_t> took = ours ? std::unordered_set<std::uint32_t>(a_record.taken.begin(), a_record.taken.end()) : std::unordered_set<std::uint32_t>{};
		// Whose world it is: their drops and takes from now on are its (see the class).
		const auto character = a_record.character;
		SetCharacter(a_player, character);
		// Whether a drop of theirs (an offer, or one lying) is this world's: one another character of theirs gave isn't.
		const auto ownWorld = [&](const Protocol::DroppedItem& a_drop) { return a_drop.ownerId == a_player && SameCharacter(a_drop.ownerCharacter, character); };
		// Whether the player's world holds the item: one they took, or their own that never left it.
		const auto holds = [&](const Protocol::DroppedItem& a_drop) {
			return took.contains(a_drop.dropId) || (ownWorld(a_drop) && !made.contains(a_drop.nonce));
		};
		// Their offers: one their world swapped out (a confirm lost with a load) lies there now; one it still has goes. One
		// another character's world made waits for that world's record.
		for (auto it = offers.begin(); it != offers.end();) {
			if (!ownWorld(it->second)) {
				++it;
				continue;
			}
			auto drop = std::move(it->second);
			it = offers.erase(it);
			if (made.contains(drop.nonce)) {
				lying[drop.dropId] = drop;
				out.confirmed.push_back(std::move(drop));
			} else {
				out.unoffered.push_back({ drop.dropId, 0 });
			}
		}
		for (auto it = lying.begin(); it != lying.end();) {
			if (!holds(it->second)) {
				++it;
				continue;
			}
			// In their world, so it lies nowhere: their own is back with them (gone for everyone, no taker), and one they
			// took is theirs (as everyone heard when they took it).
			auto drop = std::move(it->second);
			it = lying.erase(it);
			const bool own = !took.contains(drop.dropId);
			drop.takerId = a_player;
			drop.takerCharacter = character;
			out.withdrawn.push_back({ drop.dropId, own ? 0 : a_player });
			Remember(std::move(drop));
		}
		for (auto it = taken.begin(); it != taken.end();) {
			const auto& drop = it->second;
			// Not in their world: it lies there again for anyone. Only if this world is the one that took it: another
			// character's of theirs (a new one) never had it, and the one that did still holds it in its saves.
			if (drop.takerId == a_player && SameCharacter(drop.takerCharacter, character) && !holds(drop)) {
				out.returned.push_back(*Lay(it++));
				continue;
			}
			if (drop.takerId != a_player && holds(drop)) {
				++out.dupRisk;  // in their world and in the holder's too
			}
			++it;
		}
		out.changed = !out.withdrawn.empty() || !out.returned.empty() || !out.confirmed.empty();
		return out;
	}

	// What lies in the session now, in the order it was dropped.
	[[nodiscard]] std::vector<Protocol::DroppedItem> Lying() const
	{
		std::vector<Protocol::DroppedItem> out;
		out.reserve(lying.size());
		for (const auto& [id, drop] : lying) {
			out.push_back(drop);
		}
		return out;
	}

	// Everything kept, for the session file: what lies there, what was taken (takerId set), and the gifts' offers (takerId
	// Protocol::DROP_OFFERED).
	[[nodiscard]] std::vector<Protocol::DroppedItem> All() const
	{
		auto out = Lying();
		for (const auto& [id, drop] : offers) {
			if (drop.holder != 0) {
				auto offer = drop;
				offer.takerId = Protocol::DROP_OFFERED;
				out.push_back(std::move(offer));
			}
		}
		for (const auto& [id, drop] : taken) {
			out.push_back(drop);
		}
		return out;
	}

	// An entry of the saved session.
	void Restore(const Protocol::DroppedItem& a_drop)
	{
		if (a_drop.dropId == 0) {
			return;
		}
		nextId = (std::max)(nextId, a_drop.dropId + 1);
		if (a_drop.takerId == Protocol::DROP_OFFERED) {
			// A gift's offer: its giver's next record settles it (a loose drop's is never saved).
			if (a_drop.holder != 0 && a_drop.ownerId != 0 && lying.size() + offers.size() < limits.lying) {
				auto offer = a_drop;
				offer.takerId = 0;
				offers[offer.dropId] = std::move(offer);
			}
		} else if (a_drop.takerId != 0) {
			Remember(a_drop);
		} else if (lying.size() < limits.lying) {
			lying[a_drop.dropId] = a_drop;
		}
	}

	[[nodiscard]] const Protocol::DroppedItem* Find(std::uint32_t a_dropId) const
	{
		if (const auto it = lying.find(a_dropId); it != lying.end()) {
			return &it->second;
		}
		return nullptr;
	}

	// Who took it (0: nobody remembered as taking it).
	[[nodiscard]] std::uint32_t TakerOf(std::uint32_t a_dropId) const
	{
		const auto it = taken.find(a_dropId);
		return it == taken.end() ? 0 : it->second.takerId;
	}

	[[nodiscard]] std::size_t LyingCount() const { return lying.size(); }
	[[nodiscard]] std::size_t TakenCount() const { return taken.size(); }
	[[nodiscard]] std::size_t OfferCount() const { return offers.size(); }

	// What a_owner has lying there or offered.
	[[nodiscard]] std::size_t LyingOf(std::uint32_t a_owner) const
	{
		const auto of = [&](const auto& a_entry) { return a_entry.second.ownerId == a_owner; };
		return static_cast<std::size_t>(std::ranges::count_if(lying, of) + std::ranges::count_if(offers, of));
	}

	void Clear()
	{
		offers.clear();
		lying.clear();
		taken.clear();
		characters.clear();
		nextId = 1;
	}

private:
	using Book = std::map<std::uint32_t, Protocol::DroppedItem>;

	// A taken item lies there again (under its own id: every game's record names it so).
	std::optional<Protocol::DroppedItem> Lay(Book::iterator a_taken)
	{
		auto drop = std::move(a_taken->second);
		taken.erase(a_taken);
		drop.takerId = 0;
		drop.takerCharacter = 0;
		lying[drop.dropId] = drop;
		return drop;
	}

	void Remember(Protocol::DroppedItem a_drop)
	{
		while (!taken.empty() && taken.size() >= limits.taken) {
			taken.erase(taken.begin());  // the oldest: its taker's reload no longer brings it back
		}
		taken[a_drop.dropId] = std::move(a_drop);
	}

	Limits        limits;
	Book          offers;  // not confirmed by their dropper yet: lying nowhere
	Book          lying;
	Book          taken;
	std::uint32_t nextId = 1;
	std::map<std::uint32_t, std::uint64_t> characters;  // each player's world now (SetCharacter); not saved: every game says it after its welcome
};
