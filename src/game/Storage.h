#pragma once

#include "Protocol.h"

#include "game/StorageView.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Shared storage in the engine (the host's bSharedStorage; the rules are StorageView.h and the server's StorageBook.h):
// a home's or a settlement's container has one true copy, the host's game's.
//
// - The host's game lists each storage container near its player that isn't a workbench (a scan every few seconds),
//   once since the session began or its world last loaded, which claims it for the session. A friend's take its
//   container held less of than the server knew goes back as a shortfall (LootSync), and the container is listed again.
// - A friend's game, as its container menu shows a container the host's game keeps: the shared things its copy held
//   (ammo, aid, junk: LootScope's) are set aside, out of reach, the server is asked for the true copy, and its contents
//   come into the copy. Their takes and puts go out as any loot (WorldSync), checked by the server. As the menu closes
//   (or before a save or a load, or as the session ends) the session's things leave the copy and what was set aside
//   comes back. A move the server refused is undone: a take given back, a put returned (OwnGear's kStorageUndo, the
//   one change made to our player, only ever through StorageView::Client).
// - A load or a rejoin: once its catch-up has come, our game settles (OnCaughtUp): the server undoes in the true copy
//   our moves there that the loaded world lacks, and our moves it never numbered go late. Our player keeps what the
//   save has. A save keeps our moves still unanswered and those its world lacks in its co-save record (SaveRecord).
//
// StorageView::Client holds the rules; this is its engine. The engine's own inventory changes made here are kept out of
// WorldSync's watch (Applying). Main thread only, but for Applying and the menu entry points, which post to it.
namespace Storage
{
	// ---- the session (Session.cpp) ------------------------------------------------------------------------

	void OnWelcome(bool a_rule, bool a_hosting, std::uint64_t a_session);
	void OnDisconnected();
	void OnOwners(const Protocol::StorageOwners& a_owners);
	void OnContents(const Protocol::StorageListing& a_contents);
	void OnRefused(const Protocol::StorageRefusal& a_refusal);
	// The world state the server just sent answers our requests up to a_caughtUp.asked: our moves at a friend's storage are
	// settled once it answers our latest (StorageView::Side::CaughtUp).
	void OnCaughtUp(const Protocol::StorageCaughtUp& a_caughtUp);

	// A packet we were to send that the session's numbering couldn't carry (FormTranslate dropped it): a move of ours at a
	// friend's storage among them is undone, as it never went.
	void Unsent(std::span<const std::uint8_t> a_packet);

	// Before a save or a load, between WorldSync::FlushLoot (the player's last moves) and sending what WorldSync queued:
	// no save keeps the session's things in a friend's copy. Then a load's world is a save's (OnGameLoaded).
	void BeforeSaveOrLoad();
	void OnGameLoaded();

	// The co-save's record (main.cpp): our moves at a friend's storage still unanswered as the save was made, and those
	// numbered that its world lacked (the true copy undid them).
	[[nodiscard]] std::vector<std::uint8_t> SaveRecord();
	void                                    LoadRecord(std::span<const std::uint8_t> a_bytes);
	void                                    RevertRecord();

	// Every frame while welcomed: unanswered moves time out, and the host's game lists what it has loaded.
	void Frame();

	// ---- the container menu (any thread: posted to the main thread) ------------------------------------------

	void OnMenuShown(RE::ObjectRefHandle a_container);
	void OnMenuClosed();

	// ---- WorldSync's loot ------------------------------------------------------------------------------------

	[[nodiscard]] StorageView::Keeper           KeeperOf(std::uint32_t a_container);
	[[nodiscard]] std::optional<std::uint32_t> ViewFrom(std::uint32_t a_container);
	[[nodiscard]] std::uint32_t                 KeptSince(std::uint32_t a_container);
	[[nodiscard]] bool                          Aliased(std::uint32_t a_container, std::uint32_t a_item);
	[[nodiscard]] bool                          Reporting(const Protocol::ContainerChange& a_change);
	void                                        OwnStorage(const Protocol::IndexedContainerChange& a_change, bool a_lacking);
	void                                        Short(std::uint32_t a_index, const Protocol::ContainerChange& a_change, std::int32_t a_missing);
	void                                        UnlistAll();  // our world asked the server for the session's changes again (a gap): we settle again

	// Whether this thread is making one of Storage's own inventory changes (WorldSync's ContainerWatcher leaves them out).
	[[nodiscard]] bool Applying();

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// The dev channel's `storage` (docs/DEV-CHANNEL.md): status; owners; `view <ref>` / `close` (what the menu's show and
	// close do, without the menu); `take <ref> <item> <count>` / `put <ref> <item> <count>` (a transfer between our player
	// and the container, as the menu makes one); `count <ref> <item>`.
	[[nodiscard]] std::string Command(std::string_view a_args);
}
