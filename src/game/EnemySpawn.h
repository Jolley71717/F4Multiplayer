#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The `spawnenemies <baseHex> <n> <radius>` dev command: a few NPCs of one base set around the player, for a test
// script that needs a fight on cue (an ambush the session shares, the follower's targets). The game places them as
// it places a random encounter's raiders, made at runtime, so a session shares them like one (Spawns.h). Kept free
// of the game so it can be tested.
namespace EnemySpawn
{
	// The most one command places: more would only crowd the ring and the session's spawn budget.
	constexpr int MAX_COUNT = 16;

	// The ring's radius limits. Closer than the minimum, 16 of them stand inside each other and the player; past the
	// maximum, outdoors they could land outside the loaded cells.
	constexpr float MIN_RADIUS = 256.0f;
	constexpr float MAX_RADIUS = 4000.0f;

	// How far above the player's feet they are set: the ground rises or falls over the radius, and one set a little
	// above it drops onto it, where one set below it is inside the hill.
	constexpr float LIFT = 64.0f;

	struct Order
	{
		std::uint32_t base = 0;
		int           asked = 0;   // as typed
		int           count = 0;   // what is placed: asked, capped at MAX_COUNT
		float         radius = 0;
	};

	// Where one is set, and the way it faces (the game's heading: 0 is north, +y, and turning is clockwise).
	struct Spot
	{
		float x = 0, y = 0, z = 0;
		float heading = 0;
	};

	inline std::string Usage()
	{
		return std::format("error: usage: spawnenemies <baseHex> <n 1-{}> <radius {:.0f}-{:.0f}>", MAX_COUNT, MIN_RADIUS, MAX_RADIUS);
	}

	// The command's arguments, or nothing when one is missing, unreadable or out of range. A count over MAX_COUNT is
	// capped, not refused, so a script asking for a horde still gets a fight.
	inline std::optional<Order> Parse(std::string_view a_args)
	{
		std::vector<std::string_view> words;
		while (!a_args.empty()) {
			const auto start = a_args.find_first_not_of(' ');
			if (start == std::string_view::npos) {
				break;
			}
			a_args.remove_prefix(start);
			const auto end = a_args.find(' ');
			words.push_back(a_args.substr(0, end));
			a_args.remove_prefix(end == std::string_view::npos ? a_args.size() : end);
		}
		if (words.size() != 3) {
			return std::nullopt;
		}
		const auto whole = [](std::string_view a_text, auto& a_value, int a_base) {
			const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value, a_base);
			return !a_text.empty() && ec == std::errc{} && ptr == a_text.data() + a_text.size();
		};
		auto baseText = words[0];
		if (baseText.starts_with("0x") || baseText.starts_with("0X")) {
			baseText.remove_prefix(2);
		}
		Order order;
		if (!whole(baseText, order.base, 16) || order.base == 0 || !whole(words[1], order.asked, 10) || order.asked < 1) {
			return std::nullopt;
		}
		const auto [ptr, ec] = std::from_chars(words[2].data(), words[2].data() + words[2].size(), order.radius);
		if (ec != std::errc{} || ptr != words[2].data() + words[2].size() || !(order.radius >= MIN_RADIUS && order.radius <= MAX_RADIUS)) {
			return std::nullopt;
		}
		order.count = order.asked < MAX_COUNT ? order.asked : MAX_COUNT;
		return order;
	}

	// a_count spots evenly round a ring of a_radius about the player, the first straight ahead (a_heading), each set
	// a little above the player's feet and facing the player.
	inline std::vector<Spot> Ring(float a_x, float a_y, float a_z, float a_heading, int a_count, float a_radius)
	{
		constexpr float TURN = 2.0f * std::numbers::pi_v<float>;
		std::vector<Spot> out;
		for (int i = 0; i < a_count; ++i) {
			const float angle = a_heading + TURN * static_cast<float>(i) / static_cast<float>(a_count);
			Spot spot;
			spot.x = a_x + std::sin(angle) * a_radius;
			spot.y = a_y + std::cos(angle) * a_radius;
			spot.z = a_z + LIFT;
			spot.heading = std::fmod(angle + std::numbers::pi_v<float>, TURN);
			if (spot.heading < 0.0f) {
				spot.heading += TURN;
			}
			if (spot.heading >= TURN) {
				spot.heading = 0.0f;  // a hair under 0 rounds up to a whole turn when one is added
			}
			out.push_back(spot);
		}
		return out;
	}

	// The answer: how many were placed of how many asked, the base, and the new references' form IDs, which a script
	// hands to `refinfo`, `combat` or `remove`. The name is the base's (an NPC's, never a player's), its apostrophes
	// made backquotes as `enemies` does, so it stays one quoted word.
	inline std::string Answer(const Order& a_order, std::string_view a_name, std::span<const std::uint32_t> a_placed)
	{
		std::string name{ a_name };
		for (auto& c : name) {
			if (c == '\'') {
				c = '`';
			}
		}
		std::string ids;
		for (const auto id : a_placed) {
			ids += std::format("{}{:08X}", ids.empty() ? "" : ",", id);
		}
		return std::format("placed={} asked={} base={:08X} '{}' radius={:.0f} ids={}", a_placed.size(), a_order.asked, a_order.base, name, a_order.radius,
			ids.empty() ? std::string{ "none" } : ids);
	}
}
