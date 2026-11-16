#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

// The kill feed's limits ("<player> killed <victim>"): which deaths get a line. Kept free of the game so it can be
// tested; Party::OnActorDied names the killer and the victim and shows the line.
namespace KillFeed
{
	using namespace std::chrono_literals;
	using Clock = std::chrono::steady_clock;

	// A victim's kill is shown once: a second report of the same death within this long adds no line. The game that runs
	// an NPC shows the line for a friend's blow itself, as it reports the death (WorldSync's ShowFriendsKill), and that
	// friend's own report of the kill may cross it.
	inline constexpr auto SHOWN_ONCE = 10s;
	// At most PER_WINDOW lines per WINDOW, so a big fight doesn't bury everything else.
	inline constexpr auto WINDOW = 5s;
	inline constexpr int  PER_WINDOW = 3;

	class Book
	{
	public:
		// A kill of a_victim (the id our game knows it by) that has a killer and a victim to name: true to show its line.
		// One the window has no room for counts as shown all the same: its second report gets no line either.
		[[nodiscard]] bool Show(std::uint32_t a_victim, Clock::time_point a_now)
		{
			std::erase_if(shown, [&](const auto& a_entry) { return a_now - a_entry.second > SHOWN_ONCE; });
			if (!shown.try_emplace(a_victim, a_now).second) {
				return false;  // shown already
			}
			if (a_now >= windowEnds) {
				windowEnds = a_now + WINDOW;
				inWindow = 0;
			}
			return ++inWindow <= PER_WINDOW;
		}

	private:
		std::unordered_map<std::uint32_t, Clock::time_point> shown;  // by victim, when
		Clock::time_point                                    windowEnds{};
		int                                                  inWindow = 0;
	};
}
