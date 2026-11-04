#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The session's NPC level and legendary chance (protocol 57, docs/SHARED-ROLLS.md): the one level every game rolls
// leveled NPCs at, so a level-16 and a level-21 player meeting a raider build the same one, and the one multiplier of the
// legendary chance (each game's own comes from its difficulty).
//
// The host's rules say whose:
// - sNpcLevel (SessionRules::npcLevel): the mean of the players' here now (the default), the highest anyone has had,
//   the host's, or none (each game its own player's, as before 0.10.2);
// - sLegendaryDifficulty (SessionRules::legendary): the mean of the multipliers of the games here now (the default), the
//   host game's, or none (each game its own difficulty's).
//
// The server keeps both and sends them (kSessionWorld) whenever either changes. Both are kept in the session file, so
// a session carries on where it was until its players' statuses say otherwise. An NPC rolled already keeps its roll
// (the session's, for the session's life) whatever the level does after.
namespace SessionLevel
{
	struct Player
	{
		std::uint16_t level = 0;          // from their status; 0 = not heard yet
		bool          host = false;
		std::uint16_t legendaryMult = 0;  // their game's, thousandths; 0 = not heard yet
	};

	namespace detail
	{
		[[nodiscard]] inline std::string Lower(std::string_view a_text)
		{
			std::string lower;
			for (const char c : a_text) {
				lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			return lower;
		}

		// The mean of the values a_of gives that are not 0, rounded to the nearest (a half up); nothing if there are none.
		template <class Of>
		[[nodiscard]] std::optional<std::uint32_t> Mean(std::span<const Player> a_players, Of a_of)
		{
			std::uint64_t sum = 0;
			std::uint64_t count = 0;
			for (const auto& player : a_players) {
				if (const std::uint32_t value = a_of(player); value != 0) {
					sum += value;
					++count;
				}
			}
			if (count == 0) {
				return std::nullopt;
			}
			return static_cast<std::uint32_t>((sum * 2 + count) / (count * 2));
		}
	}

	// sNpcLevel / --npc-level: "average", "highest", "host" or "own", any case.
	[[nodiscard]] inline std::optional<Protocol::NpcLevelRule> Parse(std::string_view a_text)
	{
		const auto lower = detail::Lower(a_text);
		if (lower == "average") {
			return Protocol::NpcLevelRule::kAverage;
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

	// sLegendaryDifficulty / --legendary-difficulty: "average", "host" or "own", any case.
	[[nodiscard]] inline std::optional<Protocol::LegendaryRule> ParseLegendary(std::string_view a_text)
	{
		const auto lower = detail::Lower(a_text);
		if (lower == "average") {
			return Protocol::LegendaryRule::kAverage;
		}
		if (lower == "host") {
			return Protocol::LegendaryRule::kHost;
		}
		if (lower == "own") {
			return Protocol::LegendaryRule::kOwn;
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
		case Protocol::NpcLevelRule::kAverage:
			return "average";
		}
		return "?";
	}

	[[nodiscard]] inline std::string_view Name(Protocol::LegendaryRule a_rule)
	{
		switch (a_rule) {
		case Protocol::LegendaryRule::kOwn:
			return "own";
		case Protocol::LegendaryRule::kAverage:
			return "average";
		case Protocol::LegendaryRule::kHost:
			return "host";
		}
		return "?";
	}

	// The level after a_players' statuses (the players here now), from a_current.
	// - Average: the mean of theirs, rounded to the nearest (10 and 11 make 11); it follows them as they come, go and
	//   level up. With nobody heard from, it stays.
	// - Highest: the highest anyone has had in the session; it never goes down.
	// - Host: the host's, while the host is there (kept while they are away).
	// - Own: 0, nobody's.
	[[nodiscard]] inline std::uint16_t Next(Protocol::NpcLevelRule a_rule, std::uint16_t a_current, std::span<const Player> a_players)
	{
		switch (a_rule) {
		case Protocol::NpcLevelRule::kOwn:
			return 0;
		case Protocol::NpcLevelRule::kAverage:
			{
				const auto mean = detail::Mean(a_players, [](const Player& a_player) { return a_player.level; });
				return mean ? static_cast<std::uint16_t>((std::min<std::uint32_t>)(*mean, Protocol::MAX_IDENTITY_LEVEL)) : a_current;
			}
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

	// The legendary chance multiplier (thousandths) after a_players' statuses, from a_current. Average: the mean of their
	// games' multipliers, rounded to the nearest thousandth (the multipliers themselves, not the difficulties: the steps
	// are not evenly spaced, and Survival is two past Very Hard); with nobody heard from, it stays. Host: the host game's
	// (kept while they are away). Own: 0, each game its own.
	[[nodiscard]] inline std::uint16_t NextLegendary(Protocol::LegendaryRule a_rule, std::uint16_t a_current, std::span<const Player> a_players)
	{
		switch (a_rule) {
		case Protocol::LegendaryRule::kOwn:
			return 0;
		case Protocol::LegendaryRule::kAverage:
			{
				const auto mean = detail::Mean(a_players, [](const Player& a_player) { return a_player.legendaryMult; });
				return mean ? static_cast<std::uint16_t>((std::min<std::uint32_t>)(*mean, Protocol::MAX_LEGENDARY_MULT)) : a_current;
			}
		case Protocol::LegendaryRule::kHost:
			for (const auto& player : a_players) {
				if (player.host && player.legendaryMult != 0) {
					return (std::min)(player.legendaryMult, Protocol::MAX_LEGENDARY_MULT);
				}
			}
			return a_current;
		}
		return a_current;
	}
}
