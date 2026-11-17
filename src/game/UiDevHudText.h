#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the developer HUD says, from numbers gathered in the game (UiDevHud.cpp). Kept free of the game so it can be tested.
// It goes on screen and so into screenshots and videos: no addresses, names or Steam ids here. In English whatever the
// game's language, in a monospace font: the columns line up, and the words are the logs' (player numbers, the protocol's
// menu kinds).
//
//   JOINED 127.0.0.1:7779 (ping 9ms) – F4MP v0.20.0 p84
//   Sanctuary Hills 0000DD60 (-80352, 89600)
//   3 (me)               0.0s  @talking
//   1 (host)      12m    0.1s
//   5             8m     0.1s  @pipboy
//   6             away   2.4s  @crafting
//   [60 fps – 16.7/33ms – 3 hitches (12s ago) – f4mp 0.12/0.40ms]
//
// Each player's row starts with their compass marker, drawn by the movie in the gutter to its left.
namespace UiDevHudText
{
	enum class Link
	{
		kDormant,
		kHostWaiting,
		kJoining,
		kHosting,
		kJoined,
	};

	// Between the parts of a line: an en dash, as a hyphen would read as a minus sign or a missing value.
	inline constexpr std::string_view SEPARATOR = " \xE2\x80\x93 ";  // an en dash

	// An age turns amber past this, and red past STALE (seconds).
	inline constexpr float LATE = 1.0f;
	inline constexpr float STALE = 5.0f;

	struct Facts
	{
		Link          link = Link::kDormant;
		bool          steam = false;
		bool          dedicated = false;  // in a session whose server has no host player
		std::string   linkDetail;         // where to, and how it's going (masked: this goes on screen)
		std::uint32_t roundTripMs = 0;    // 0: not measured yet
		std::string   version;            // "0.20.0"
		std::uint32_t protocol = 0;

		std::string   location;
		std::uint32_t cell = 0;
		float         x = 0.0f;
		float         y = 0.0f;

		struct Player
		{
			std::uint32_t        id = 0;
			bool                 me = false;
			bool                 host = false;
			std::optional<float> metres;               // none: not in our cell or worldspace (and never for us)
			bool                 unreachable = false;  // somewhere our game doesn't have
			std::optional<float> age;                  // seconds since their last state came (ours: since the server was last heard)
			std::string_view     menu;                 // the protocol's name of the menu they are in; empty: none
		};
		std::vector<Player> players;  // ourselves first

		float                fps = 0.0f;
		float                frameMeanMs = 0.0f;
		float                frameWorstMs = 0.0f;
		std::uint32_t        hitches = 0;  // since the last load
		std::optional<float> lastHitch;    // seconds since the latest of them
		float                oursP50Ms = 0.0f;
		float                oursP99Ms = 0.0f;
	};

	// A player's row, for the movie: which line it is, and where its age is, to color it.
	struct Row
	{
		std::uint32_t id = 0;
		std::size_t   line = 0;
		std::size_t   ageFrom = 0;  // in characters of the whole text, as Flash counts them
		std::size_t   ageTo = 0;    // past the age; ageFrom when it has none
		int           ageLevel = 0;  // 0 fresh, 1 late, 2 stale
	};

	struct Shown
	{
		std::string      text;
		std::vector<Row> rows;
	};

	inline const char* LinkName(Link a_link)
	{
		switch (a_link) {
		case Link::kHostWaiting:
			return "HOST WAITING";
		case Link::kJoining:
			return "JOINING";
		case Link::kHosting:
			return "HOSTING";
		case Link::kJoined:
			return "JOINED";
		default:
			return "DORMANT";
		}
	}

	// "0.20.0" from the plugin's "0.20.0.0" (as the menu's About shows it).
	inline std::string ShortVersion(std::string a_version)
	{
		while (a_version.ends_with(".0") && std::count(a_version.begin(), a_version.end(), '.') > 2) {
			a_version.resize(a_version.size() - 2);
		}
		return a_version;
	}

	inline int AgeLevel(float a_seconds)
	{
		return a_seconds > STALE ? 2 : a_seconds > LATE ? 1 : 0;
	}

	// Characters as Flash counts them (UTF-16 units; every character here is in the basic plane).
	inline std::size_t Characters(std::string_view a_text)
	{
		std::size_t count = 0;
		for (const auto c : a_text) {
			count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
		}
		return count;
	}

	// Always in seconds: "m" is the distance's metres.
	inline std::string Seconds(float a_seconds)
	{
		return a_seconds < 10.0f ? std::format("{:.1f}s", a_seconds) : std::format("{:.0f}s", a_seconds);
	}

	// How long ago the latest hitch was: minutes past one.
	inline std::string Ago(float a_seconds)
	{
		return a_seconds < 60.0f ? Seconds(a_seconds) : std::format("{:.0f} min", a_seconds / 60.0f);
	}

	inline Shown Format(const Facts& a_facts)
	{
		Shown out;
		auto& text = out.text;
		const auto ping = a_facts.roundTripMs ? std::format("{}ms", a_facts.roundTripMs) : std::string("-");
		text += std::format("{}{}{}{} (ping {}){}F4MP v{} p{}\n", LinkName(a_facts.link), a_facts.steam ? " steam" : "",
			a_facts.linkDetail.empty() ? "" : " " + a_facts.linkDetail, a_facts.dedicated ? " dedicated" : "", ping, SEPARATOR,
			a_facts.version.empty() ? "?" : a_facts.version, a_facts.protocol);
		text += std::format("{} {:08X} ({:.0f}, {:.0f})\n", a_facts.location.empty() ? "-" : a_facts.location, a_facts.cell, a_facts.x, a_facts.y);

		std::size_t line = 2;
		for (const auto& player : a_facts.players) {
			const auto who = std::format("{}{}", player.id, player.me ? " (me)" : player.host ? " (host)" : "");
			const auto where = player.me          ? std::string{} :
			                   player.unreachable ? std::string("??") :
			                   player.metres      ? std::format("{:.0f}m", *player.metres) :
			                                        std::string("away");
			auto row = std::format("{:<14}{:<7}", who, where);
			Row shown{ .id = player.id, .line = line++ };
			shown.ageFrom = Characters(text) + Characters(row);
			if (player.age) {
				const auto age = Seconds(*player.age);
				row += age;
				shown.ageLevel = AgeLevel(*player.age);
				row.append(age.size() < 6 ? 6 - age.size() : 1, ' ');
			}
			shown.ageTo = shown.ageFrom + (player.age ? Seconds(*player.age).size() : 0);
			if (!player.menu.empty()) {
				row.append(player.age ? 0 : 6, ' ');
				row += std::format("@{}", player.menu);
			}
			while (!row.empty() && row.back() == ' ') {
				row.pop_back();
			}
			text += row + "\n";
			out.rows.push_back(shown);
		}

		const auto hitches = a_facts.hitches == 1 ? std::string("1 hitch") : std::format("{} hitches", a_facts.hitches);
		const auto ago = a_facts.hitches && a_facts.lastHitch ? std::format(" ({} ago)", Ago(*a_facts.lastHitch)) : std::string{};
		text += std::format("[{:.0f} fps{}{:.1f}/{:.0f}ms{}{}{}{}f4mp {:.2f}/{:.2f}ms]", a_facts.fps, SEPARATOR, a_facts.frameMeanMs, a_facts.frameWorstMs, SEPARATOR,
			hitches, ago, SEPARATOR, a_facts.oursP50Ms, a_facts.oursP99Ms);
		return out;
	}
}
