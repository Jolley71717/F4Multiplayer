#pragma once

#include "Protocol.h"
#include "game/CharacterBinding.h"
#include "game/LootSync.h"

#include <cstdint>

// The order Session handles a load, a hello, a welcome and a change of server in, between which character plays
// (CharacterBinding::Tracker) and what our world holds of the session (LootSync::Loot). Kept free of the game so the
// tests run the same steps against a real server; Session and WorldSync call these and nothing else for it.
namespace SessionSteps
{
	// Connected: the Hello's world part. The loaded character joins (see Tracker::OnHello) with what its world holds.
	[[nodiscard]] inline Protocol::WorldRequest OnHello(CharacterBinding::Tracker& a_binding, LootSync::Loot& a_loot, std::uint64_t a_character,
		LootSync::Clock::time_point a_now = LootSync::Clock::now())
	{
		a_binding.OnHello(a_character);
		return a_loot.Hello(a_now);
	}

	// The server's Welcome. kLeave: it isn't taken (Tracker::OnWelcome), and nothing of it may be applied.
	[[nodiscard]] inline CharacterBinding::Step OnWelcome(CharacterBinding::Tracker& a_binding, LootSync::Loot& a_loot, bool a_hosting, const Protocol::Welcome& a_welcome)
	{
		const auto step = a_binding.OnWelcome(a_hosting, a_welcome.sessionId);
		if (step == CharacterBinding::Step::kNone) {
			a_loot.OnWelcome(a_welcome.sessionId, a_welcome.playerId);
		}
		return step;
	}

	// The connection went (it may come back: the loot's record stays, and the character that joined).
	inline void OnDisconnected(LootSync::Loot& a_loot)
	{
		a_loot.OnDisconnected();
	}

	// Another server was chosen, or none.
	inline void OnServerChanged(CharacterBinding::Tracker& a_binding, LootSync::Loot& a_loot)
	{
		a_loot.OnDisconnected();
		a_binding.OnServerChanged();
	}

	// A save was loaded, or (a_newGame) a new game started, after the save's co-save record came in. First whether the
	// loaded world is the session's at all: a_act does the step (stops hosting, or leaves, which disconnects). Only then
	// is the world bound to the session it is still in, and its changes since asked for (a_send): a world that left must
	// not be recorded as holding the session's changes, nor ask for them.
	template <class Act, class Send>
	void OnGameLoaded(CharacterBinding::Tracker& a_binding, LootSync::Loot& a_loot, const CharacterBinding::Link& a_link, bool a_newGame,
		std::uint64_t a_character, Act&& a_act, Send&& a_send)
	{
		if (const auto step = a_binding.OnLoad(a_link, a_newGame, a_character); step != CharacterBinding::Step::kNone) {
			a_act(step);
			OnServerChanged(a_binding, a_loot);
		}
		if (const auto request = a_loot.AfterLoad(LootSync::Clock::now())) {
			a_send(*request);
		}
	}
}
