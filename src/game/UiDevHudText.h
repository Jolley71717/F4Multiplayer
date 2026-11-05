#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

// What the developer HUD says, from numbers gathered in the game (UiDevHud.cpp). Kept free of the game so it can be tested.
// It goes on screen and so into screenshots and videos: no addresses, names or Steam ids here.
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

	struct Facts
	{
		Link          link = Link::kDormant;
		bool          steam = false;
		std::string   linkDetail;       // where to, and how it's going (masked: this goes on screen)
		struct Friend
		{
			std::uint32_t        id = 0;
			std::optional<float> metres;  // none: not in our cell or worldspace
			bool                 unreachable = false;  // somewhere our game doesn't have
		};
		std::vector<Friend> friends;
		std::uint32_t roundTripMs = 0;  // 0: not measured yet
		float         fps = 0.0f;
		float         frameMeanMs = 0.0f;
		float         frameWorstMs = 0.0f;
		std::uint32_t hitches = 0;
		float         oursP50Ms = 0.0f;
		float         oursP99Ms = 0.0f;
		std::string   location;
		std::uint32_t cell = 0;
		float         x = 0.0f;
		float         y = 0.0f;
		std::uint32_t update = 0;  // counts up with each text, so a frozen HUD can be told from a quiet one
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

	inline std::string Format(const Facts& a_facts)
	{
		const auto ping = a_facts.roundTripMs ? std::format("{} ms", a_facts.roundTripMs) : std::string("-");
		auto       out = std::format("{}{}{}  |  ping {}  |  players {}\n", LinkName(a_facts.link), a_facts.steam ? " steam" : "", a_facts.linkDetail.empty() ? "" : "  " + a_facts.linkDetail, ping, a_facts.friends.size() + 1);
		for (const auto& entry : a_facts.friends) {
			out += entry.unreachable ? std::format("player {}  unreachable\n", entry.id) :
			       entry.metres      ? std::format("player {}  {:.0f} m\n", entry.id, *entry.metres) :
			                           std::format("player {}  elsewhere\n", entry.id);
		}
		return out +
		       std::format("{:.0f} fps  |  frame {:.1f} / {:.0f} ms  |  hitches {}\n", a_facts.fps, a_facts.frameMeanMs, a_facts.frameWorstMs, a_facts.hitches) +
		       std::format("mod {:.2f} / {:.2f} ms  |  update {}\n", a_facts.oursP50Ms, a_facts.oursP99Ms, a_facts.update) +
		       std::format("{} {:08X}  ({:.0f}, {:.0f})", a_facts.location.empty() ? "-" : a_facts.location, a_facts.cell, a_facts.x, a_facts.y);
	}
}
