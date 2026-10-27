#pragma once

#include "Protocol.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Giving items to friends by dropping them. Our player drops an item; once it has come to rest it is reported
// (kReportDrop), and the server offers it back to us alone (kDropPlaced): our game swaps the item we dropped for a copy and
// says so (kClaimDrop kPlaced), and only then does the server tell the other games, each placing its own copy where it
// lies. So no friend can take an item that is still in our world. A copy can't be picked up as it is (it is shown only once
// its activation is blocked): pressing Take on one asks the server (kClaimDrop), which grants each item to whoever asks
// first (common/DropStore.h), and only then does our player pick it up, as their own take. Every other game removes its
// copy. So an item moves once: never into two inventories, never into none.
//
// Copies are never saved; they go before every save and load and are placed again while our player is near. What our
// world holds of the transfers (the drops whose item left it, the items it took in) goes into the co-save (record 'DROP')
// and to the server after every welcome and load (kDropRecord), so a reload on either side neither copies an item nor
// loses one.
//
// Not given: keys, notes, holotapes, books, bobbleheads, magazines (everyone collects their own), quest items, and an item
// whose base or mods exist in our game only. Those stay on our ground, in our game only. Main thread only, but for the
// event hooks.
namespace Drops
{
	// The engine's events (any thread): our player's inventory lost an item to nothing (a drop, or an item used up), with
	// the reference it became if the event names one; and our player activated a reference made at runtime.
	void OnLeftInventory(std::uint32_t a_base, std::int32_t a_count, std::uint32_t a_ref);
	void OnActivated(std::uint32_t a_ref);

	// The engine's event (any thread): our player equipped something of a_base (using up food or a stim is one too).
	void OnEquipped(std::uint32_t a_base);

	// Any thread: a_ref is one of our copies of a friend's drop (never reported as a workshop piece of ours).
	bool IsCopyRef(std::uint32_t a_ref);

	// Any thread: workshop mode scrapped (or stored) a_ref, from a_bench's settlement. True if it was one of our copies:
	// the item is claimed as our player's take, so it goes for everyone (they have its components), or, if someone else's
	// claim came first, it is theirs alone and what the scrap put in a_bench is taken back out.
	bool OnScrapped(std::uint32_t a_ref, std::uint32_t a_bench);

	// The engine's event (any thread): a_count of a_base came into a_container from nothing (a scrap's components in its
	// workbench, among others). Noted only while workshop mode is up.
	void OnAddedFromNowhere(std::uint32_t a_container, std::uint32_t a_base, std::int32_t a_count);

	// Dev only (the dropitem command): the next item our player loses to nothing is taken as dropped, as from the Pip-Boy
	// (the console's drop runs with no menu open).
	void DevDropNext();

	// Dev only (`drops loseplaced`): our next swap's kPlaced isn't sent, as one lost with the link (then `net drop`: the item
	// must come back to our world as we leave).
	void DevLoseNextPlaced();

	// Our player put something in a container that a friend's game won't see there (a weapon or armor, or anything in a
	// workbench): said once a session, with how to give it instead.
	void OnKeptInContainer();

	// Gifts' share (src/game/GiftBook.h: a weapon or armor given through a container is a drop too, its holder set). Our
	// record of the session's transfers (any thread); whether our game can make the item as it is; and one of it (count 1)
	// put in a_ref's inventory, made as our copies are (its mods, condition and name; main thread).
	void               RecordMade(std::uint64_t a_nonce);
	[[nodiscard]] bool HasMade(std::uint64_t a_nonce);
	void               RecordTook(std::uint32_t a_dropId);
	[[nodiscard]] bool HasTaken(std::uint32_t a_dropId);
	[[nodiscard]] bool CanPlace(const Protocol::DroppedItem& a_item);
	bool               AddOneTo(RE::TESObjectREFR* a_ref, const Protocol::DroppedItem& a_item);

	// Messages from the server (a gift's are Gifts', and never come here: Session).
	void OnPlaced(const Protocol::DroppedItem& a_drop);
	void OnGone(const Protocol::DropGone& a_gone);
	void OnList(const Protocol::DropList& a_list);

	// Every frame while welcomed.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// We were welcomed into a_session as a_playerId: the record of that session goes to the server.
	void OnWelcome(std::uint64_t a_session, std::uint32_t a_playerId);

	// A save loaded while we stay in the session: our world is the save's, and its record goes to the server.
	void SendRecord();

	// Before a save or a load: our copies go (never saved).
	void DiscardCopies();

	// A save was loaded (or a new game started): what we knew of our world described the one before it.
	void OnGameLoaded();

	// The co-save (on the save's or load's thread): our record, written with the save and read back with it.
	std::vector<std::uint8_t> SaveRecord();
	void                      LoadRecord(std::span<const std::uint8_t> a_bytes);
	void                      RevertRecord();  // the world's key too

	// Whose world our record speaks for (CharacterBinding::World): its key, in the co-save beside the record (on the save's
	// or load's thread), and after the load or new game is done (main thread; a_character: the loaded character's ID).
	std::vector<std::uint8_t> SaveWorld();
	void                      LoadWorld(std::span<const std::uint8_t> a_bytes);
	void                      OnWorldStarted(bool a_newGame, std::uint64_t a_character);

	// The session ended: copies deleted, everything but the record forgotten (it describes our world).
	void Reset();

	// For the dev commands: what lies in the session and our copies, in one line ("; " between entries).
	std::string List();

	// For the dev commands: each stack of a_base our player carries, with its mods, condition and name.
	std::string ItemMods(std::uint32_t a_base);

	std::string Describe();
}
