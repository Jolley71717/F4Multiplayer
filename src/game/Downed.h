#pragma once

#include "Protocol.h"

// Downed and revive: in a session with friends, a lethal hit knocks the player down instead of
// killing them (the game's "essential" state, held down until helped). A friend who stays next
// to them for 2 s helps them up; after 45 s, when they give up, or when the last friend leaves,
// they die as usual. Main thread only.
namespace Downed
{
	// Call every frame. a_withFriends: in a session with at least one other player.
	void Frame(bool a_withFriends);

	[[nodiscard]] bool IsDown();

	// Die now instead of waiting for help.
	void GiveUp();

	// A friend helped us up (playerId = the friend).
	void ApplyRevive(const Protocol::Revive& a_revive);

	// A friend's downed state, from their status.
	void SetFriendDown(std::uint32_t a_id, bool a_down);
	void OnPlayerLeft(std::uint32_t a_id);

	// The essential flag we set must never be written into a save.
	void OnBeforeSave();

	// A save was loaded: whatever state we were in belongs to the old game.
	void OnGameLoaded();

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Forgets friends and pending messages (the next Frame without friends clears the rest).
	void Reset();

	[[nodiscard]] std::string Describe();
}
