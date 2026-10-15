#pragma once

#include "Protocol.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The `enemies` dev command: the actors near the player that threaten someone in the session, for a test
// script that plays a co-op follower. It shoots only at what the player can see (los), and an enemy another
// game runs has its AI off here, so what it fights is asked of the owner's game (owner): there the one it
// fights is given as the session player it is (targetPlayer), the same number in every game, where the form
// ID of a friend's stand-in differs from game to game. An actor made at runtime (made=true: a random
// encounter, a spawned enemy) has another form ID in every game that has one at all: `enemies like` finds
// this game's actor of the same base standing at the same spot. Kept free of the game so it can be tested.
namespace EnemyList
{
	// The most actors one answer lists, nearest first.
	constexpr std::size_t MAX_LISTED = 30;

	// How far from the spot `enemies like` looks for this game's actor when not told.
	constexpr float LIKE_WITHIN = 512.0f;

	// What the game says of one actor near the player.
	struct Seen
	{
		std::uint32_t id = 0;
		std::uint32_t base = 0;
		std::string   name;
		float         x = 0, y = 0, z = 0;
		float         distance = 0;          // from the player
		bool          dead = false;
		bool          standIn = false;       // a friend's stand-in
		bool          companion = false;     // anyone's companion (Dogmeat and the rest)
		bool          hostile = false;       // hostile to the player
		bool          inCombat = false;
		std::uint32_t target = 0;            // its combat target here, 0 for none
		std::uint32_t targetPlayer = 0;      // the session player that target is (ours, or a friend's stand-in), else 0
		bool          lineOfSight = false;   // the player can see it
		std::uint32_t owner = 0;             // the player whose game runs it (0: nobody, an actor only this game has)
		bool          runsHere = true;       // its AI runs in this game, so its combat target here is the real one
	};

	// Whether this game runs an NPC's AI: nobody's (the game's own, as when playing alone) or ours.
	constexpr bool RunsHere(std::uint32_t a_owner, std::uint32_t a_localPlayer)
	{
		return a_owner == 0 || a_owner == a_localPlayer;
	}

	// Made at runtime (0xFF......): this form ID means another actor, or nothing, in every other game.
	constexpr bool MadeHere(std::uint32_t a_id)
	{
		return a_id != 0 && a_id != Protocol::PLAYER_REF_ID && !Protocol::IsShareableRef(a_id);
	}

	// Whether it is an enemy worth listing: alive, nobody's stand-in or companion, and hostile to the player or,
	// with its AI here, fighting a player (ours, also outside a session). A copy of an NPC another game runs fights nothing here.
	inline bool Listed(const Seen& a_seen)
	{
		if (a_seen.dead || a_seen.standIn || a_seen.companion) {
			return false;
		}
		return a_seen.hostile || (a_seen.runsHere && a_seen.inCombat && (a_seen.targetPlayer != 0 || a_seen.target == Protocol::PLAYER_REF_ID));
	}

	// One actor as the dev channel prints it. The name's apostrophes become backquotes, so it stays one quoted word
	// for a script. Without its AI here, its combat target is left out (00000000): the owner's game has the real one.
	inline std::string Describe(const Seen& a_seen)
	{
		std::string name = a_seen.name;
		std::ranges::replace(name, '\'', '`');
		const bool targetKnown = a_seen.runsHere && a_seen.inCombat;
		return std::format("{:08X} '{}' base={:08X} hostile={} combat={} target={:08X} targetPlayer={} los={} dist={:.0f} x={:.0f} y={:.0f} z={:.0f} owner={} here={} made={}",
			a_seen.id, name, a_seen.base, a_seen.hostile, targetKnown, targetKnown ? a_seen.target : 0, targetKnown ? a_seen.targetPlayer : 0, a_seen.lineOfSight,
			a_seen.distance, a_seen.x, a_seen.y, a_seen.z, a_seen.owner, a_seen.runsHere, MadeHere(a_seen.id));
	}

	// The `enemies` answer: the listed ones, nearest first, at most a_max, "; " between them; "none" when there are none.
	inline std::string List(std::vector<Seen> a_seen, std::size_t a_max = MAX_LISTED)
	{
		std::erase_if(a_seen, [](const Seen& a_entry) { return !Listed(a_entry); });
		std::ranges::stable_sort(a_seen, {}, &Seen::distance);
		std::string out;
		for (std::size_t i = 0; i < a_seen.size() && i < a_max; ++i) {
			out += (i ? "; " : "") + Describe(a_seen[i]);
		}
		return out.empty() ? std::string{ "none" } : out;
	}

	// This game's actor for one another game listed at (a_x, a_y) with base a_base (0: any base, as a leveled
	// enemy can roll another one in each game): the nearest alive one within a_within, not a stand-in or a
	// companion. Height is left out: two games put the same actor on the ground a little differently.
	inline std::optional<Seen> Like(const std::vector<Seen>& a_here, std::uint32_t a_base, float a_x, float a_y, float a_within = LIKE_WITHIN)
	{
		const Seen* best = nullptr;
		float       bestDistance = 0.0f;
		for (const auto& seen : a_here) {
			if (seen.dead || seen.standIn || seen.companion || (a_base != 0 && seen.base != a_base)) {
				continue;
			}
			const float distance = std::hypot(seen.x - a_x, seen.y - a_y);
			if (!(distance <= a_within) || (best && distance >= bestDistance)) {
				continue;
			}
			best = &seen;
			bestDistance = distance;
		}
		return best ? std::optional<Seen>{ *best } : std::nullopt;
	}

	// Reads an `enemies` answer back, as a script does: id, name, base, place and flags of each actor. (The distance
	// is the reader's distance from the player of the game that answered.) Pieces it can't read are skipped.
	inline std::vector<Seen> Parse(std::string_view a_answer)
	{
		const auto field = [](std::string_view a_text, std::string_view a_key) -> std::string_view {
			const auto key = std::string{ " " } + std::string{ a_key } + "=";
			const auto at = a_text.find(key);
			if (at == std::string_view::npos) {
				return {};
			}
			auto value = a_text.substr(at + key.size());
			return value.substr(0, value.find(' '));
		};
		const auto hex = [](std::string_view a_text) -> std::optional<std::uint32_t> {
			std::uint32_t value = 0;
			const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			return ec == std::errc{} && ptr == a_text.data() + a_text.size() && !a_text.empty() ? std::optional{ value } : std::nullopt;
		};
		const auto number = [](std::string_view a_text) {
			float value = 0.0f;
			std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			return value;
		};
		const auto whole = [](std::string_view a_text) {
			std::uint32_t value = 0;
			std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			return value;
		};
		std::vector<Seen> out;
		while (!a_answer.empty()) {
			const auto end = a_answer.find("; ");
			const auto piece = a_answer.substr(0, end);
			a_answer.remove_prefix(end == std::string_view::npos ? a_answer.size() : end + 2);
			const auto open = piece.find(" '");
			const auto close = open == std::string_view::npos ? open : piece.find('\'', open + 2);
			const auto id = open == std::string_view::npos ? std::nullopt : hex(piece.substr(0, open));
			if (!id || close == std::string_view::npos) {
				continue;
			}
			const auto rest = piece.substr(close + 1);
			Seen seen;
			seen.id = *id;
			seen.name = std::string{ piece.substr(open + 2, close - open - 2) };
			seen.base = hex(field(rest, "base")).value_or(0);
			seen.hostile = field(rest, "hostile") == "true";
			seen.inCombat = field(rest, "combat") == "true";
			seen.target = hex(field(rest, "target")).value_or(0);
			seen.targetPlayer = whole(field(rest, "targetPlayer"));
			seen.lineOfSight = field(rest, "los") == "true";
			seen.distance = number(field(rest, "dist"));
			seen.x = number(field(rest, "x"));
			seen.y = number(field(rest, "y"));
			seen.z = number(field(rest, "z"));
			seen.owner = whole(field(rest, "owner"));
			seen.runsHere = field(rest, "here") == "true";
			out.push_back(std::move(seen));
		}
		return out;
	}
}
