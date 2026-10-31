#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The session's NPC level (protocol 57, docs/SHARED-ROLLS.md): the one level every game rolls leveled NPCs at, so a
// level-16 and a level-21 player meeting a raider build the same one. The host's rule (sNpcLevel, SessionRules::npcLevel)
// says which: the highest of the players' (the default), the host's, or none (each game its own player's, as before 0.10.2).
// The server keeps it and sends it (kSessionWorld) whenever it changes; it is kept in the session file, so a session
// carries on at the level it reached.
namespace SessionLevel
{
	struct Player
	{
		std::uint16_t level = 0;  // from their status; 0 = not heard yet
		bool          host = false;
	};

	// sNpcLevel / --npc-level: "highest", "host" or "own", any case.
	[[nodiscard]] inline std::optional<Protocol::NpcLevelRule> Parse(std::string_view a_text)
	{
		std::string lower;
		for (const char c : a_text) {
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		if (lower == "highest") {
			return Protocol::NpcLevelRule::kHighest;
		}
		if (lower == "host") {
			return Protocol::NpcLevelRule::kHost;
		}
		if (lower == "own") {
			return Protocol::NpcLevelRule::kOwn;
		}
		return std::nullopt;
	}

	[[nodiscard]] inline std::string_view Name(Protocol::NpcLevelRule a_rule)
	{
		switch (a_rule) {
		case Protocol::NpcLevelRule::kOwn:
			return "own";
		case Protocol::NpcLevelRule::kHighest:
			return "highest";
		case Protocol::NpcLevelRule::kHost:
			return "host";
		}
		return "?";
	}

	// The level after a_players' statuses, from a_current. Highest: the highest anyone has had in the session (it never
	// goes down: an NPC rolled at it stays that). Host: the host's, while the host is there (kept while they are away).
	// Own: 0, nobody's.
	[[nodiscard]] inline std::uint16_t Next(Protocol::NpcLevelRule a_rule, std::uint16_t a_current, std::span<const Player> a_players)
	{
		switch (a_rule) {
		case Protocol::NpcLevelRule::kOwn:
			return 0;
		case Protocol::NpcLevelRule::kHighest:
			{
				auto level = a_current;
				for (const auto& player : a_players) {
					level = (std::max)(level, player.level);
				}
				return (std::min)(level, Protocol::MAX_IDENTITY_LEVEL);
			}
		case Protocol::NpcLevelRule::kHost:
			for (const auto& player : a_players) {
				if (player.host && player.level != 0) {
					return (std::min)(player.level, Protocol::MAX_IDENTITY_LEVEL);
				}
			}
			return a_current;
		}
		return a_current;
	}
}
