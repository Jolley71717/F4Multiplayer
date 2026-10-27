#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Giving weapons and armor through a container (the host's bContainerGifts). GiftBook.h holds the rules (what a put, a
// take, an offer, a grant and a reload do); this is its engine: what a container and our player hold, a gift's copy put
// in or taken out of the container whose menu is open, and our player's take held back until the server grants it.
//
// The engine's own inventory changes made here are kept out of WorldSync's watch (Applying), and judged by OwnGear (a
// gift's copy only ever in a container, our player's take only ever theirs). Our player's moves come from WorldSync's
// event watch (OnMoved) and wait in an inbox for the main thread, which reads it before anything else here looks at the
// container. Main thread only, but for the entry points that say otherwise.
namespace Gifts
{
	// ---- the session (Session.cpp) ------------------------------------------------------------------------

	// We were welcomed as a_playerId; a_rule: the host's bContainerGifts.
	void OnWelcome(std::uint32_t a_playerId, bool a_rule);

	// The server's messages about gifts (a drop with a holder; a loose drop's are Drops'). OnGone: false when the drop is no
	// gift we know of (Drops' to handle).
	void OnPlaced(const Protocol::DroppedItem& a_drop);
	bool OnGone(const Protocol::DropGone& a_gone);
	void OnList(const Protocol::DropList& a_list);

	// Every frame while welcomed and in the game.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Before a save or a load (no copy is saved), and after a load.
	void BeforeSaveOrLoad();
	void OnGameLoaded();

	// The session ended: our copies leave the container, and all but our record (Drops') is forgotten. Nothing is given back.
	void Reset();

	// ---- the container menu and WorldSync (any thread: posted or kept for the main thread) ---------------------

	void OnMenuShown(RE::ObjectRefHandle a_container);
	void OnMenuClosed();

	// Our player moved a_count of a_base between their inventory and a_holder (+: put in, -: taken out).
	void OnMoved(std::uint32_t a_holder, std::uint32_t a_base, std::int32_t a_count);

	// Whether this thread is making one of Gifts' own inventory changes (WorldSync's ContainerWatcher leaves them out).
	[[nodiscard]] bool Applying();

	// Main thread: a weapon or armor our player puts in a_holder is given (the host's rule on, and a gift's place): WorldSync
	// says no "stays in your game only" hint for it.
	[[nodiscard]] bool GivesIn(std::uint32_t a_holder);

	// For `net status`.
	[[nodiscard]] std::string Describe();

	// The dev channel's `gifts` (docs/DEV-CHANNEL.md): status; list; `view <ref>` / `close` (what the container menu's show
	// and close do, without the menu); `count <ref> <item>`; `take <ref> <item> [count]` / `put <ref> <item> [count]` (a
	// transfer between our player and the container, as the menu makes one).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
