#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Power armor lent between players (the rules: FrameLoan.h). A friend presses E on our copy of a frame their game made
// and parked: our game asks them for it, their game sends its pieces and core, ours puts copies of them into our copy,
// and our player gets in through the engine's own activation. While they wear it our status names it by its spawn id,
// which every game hides its own reference of (RemotePlayers' frame hide), and our copy is held as it is (the spawn
// book's lent fact). As they get out the copies are gathered and removed, and their game is told where the frame
// stands and what wear came back: it moves its own there and wears its pieces down to that. A save made in the frame
// keeps our copy with our player in it, and notes it (the LOAN record): loaded again, our player is sent out of it and
// it goes, with the copies. Main thread only, but for the record (SaveRecord, LoadRecord, RevertRecord), which the save
// and the load call on their own thread.
namespace FrameLoans
{
	// The spawn id of the frame our copy a_local is of, while it is lent to us (our wear names it by that); 0 otherwise,
	// and 0 once its owner is gone (their spawn went with them).
	[[nodiscard]] std::uint32_t WornSpawnOf(std::uint32_t a_local);

	// The friend our own frame a_local is lent to (or offered to, the server's word on it not in yet); 0 if none.
	[[nodiscard]] std::uint32_t LentOutTo(std::uint32_t a_local);
	[[nodiscard]] bool          LentOut(std::uint32_t a_local);

	// Our copy of the spawn a_id is lent to us: held as it is for the loan (SpawnBook::FrameFacts::lent).
	[[nodiscard]] bool LentToUs(std::uint32_t a_id);

	// a_local is our copy of the frame lent to us, while the loan lasts (not while it is only asked for): the frame our
	// player gets out of last is it for the settle after they are out, and it is not their parked frame to tell.
	[[nodiscard]] bool BorrowedCopy(std::uint32_t a_local);

	// Our player pressed E on a_frame, our copy of a_owner's frame shared as a_spawnId (RemotePlayers' frame gate): false
	// lets them in (lent to us, the kit in it); true keeps them out, and the HUD says why or that we asked.
	bool KeepOut(RE::TESObjectREFR* a_frame, std::uint32_t a_spawnId, std::uint32_t a_owner);

	// An item of a_base named a_name is one of the copies of a friend's lent pieces our player holds now (Drops never gives
	// one away: it goes back at the return).
	[[nodiscard]] bool IsLentItem(std::uint32_t a_base, std::string_view a_name);

	// A save or a load is coming (Spawns::DiscardCopies): the copy our player wears on loan is kept as it is.
	[[nodiscard]] bool KeepsThroughSave(const RE::TESObjectREFR* a_copy);

	// Just before a save: what its LOAN record holds (SaveRecord). A clean-up after a load not done yet is noted again.
	void BeforeSave();

	// A save is coming, before the session's copies go (Session::OnBeforeSaveOrLoad): a frame lent to us that our player
	// has not got into yet is given back, unworn, so that none of it is in the save.
	void EndUnwornBeforeSave();

	// A friend's status: whether they lend their frames.
	void OnStatus(std::uint32_t a_playerId, bool a_lends);
	void OnPlayerLeft(std::uint32_t a_playerId);

	void OnLoanAsked(const Protocol::FrameBorrow& a_ask);
	void OnFrameLent(const Protocol::FrameLending& a_lending);
	void OnFrameReturned(const Protocol::FrameReturn& a_return);

	// Every frame, in a session or not (a loan worn as the session ended, and the clean-up after a load, go on).
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// The co-save's LOAN record (src/main.cpp). Empty: none to write.
	std::vector<std::uint8_t> SaveRecord();
	void                      LoadRecord(const std::vector<std::uint8_t>& a_bytes);
	void                      RevertRecord();

	// A load is about to begin (kPreLoadGame, after Session::OnBeforeSaveOrLoad): the loans of our frames, the gives back
	// waiting and the moves waiting are let go now, while the engine's data they hold on to (the extra data of the lent
	// pieces) is still whole. Nothing changes in what the game does: they end with the load anyway.
	void BeforeLoad();

	// A save was loaded (or a new game begun): the loans of the old world are over, and a loaded LOAN record is cleaned up.
	void OnGameLoaded();

	// The session ended. A frame our player wears on loan is theirs to finish wearing: it goes as they get out.
	void Reset();

	std::string Describe();
}
