#pragma once

#include "game/RemoteEdits.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// NPCs our player placed with the console (`player.placeatme <base> <n>`, `placeleveledactoratme`): which of our game's
// runtime NPCs they are, so Spawns shares them calm too.
//
// A runtime NPC (0xFF......) is each game's own, shared as a spawn by the game that made it (Spawns.h). Until now only
// one fighting or hostile to its player was offered, or one a quest of a shared kind made (a random encounter's actor,
// a shared radiant's, the host's settler): each game's scripts make their own calm NPCs, and offering every one would
// double them. An NPC the console placed is made by one game alone, by its player's hand, so a calm one stood in that
// game only (a Nexus report on 0.20.1: "spawning NPCs with the console isn't synced at all"; a hostile one was shared
// already, from its first scan).
//
// The console's command names no reference the plugin can read back without the engine's argument parser, so the NPC is
// found by where and when it appears: the command's hook (ConsolePlace) says "a place command ran on a reference
// standing there, now", and a runtime NPC the scan sees for the first time within APPEARS_WITHIN of that, within
// PLACED_WITHIN of the spot, is the console's. First seen means: not seen by any scan in the last SEEN_KEPT under that id
// with that base (an id is the engine's to give again once its reference is gone, so the base is kept with it). An NPC of
// a script's that happens to load beside the player in those seconds is taken for the console's too: it is shared calm
// then, which is the worst of it.
//
// Limits: the server's on any spawn (Protocol::MAX_SPAWNS_PER_PLAYER a game, its session's, its tokens) hold as they
// are; of those, at most MAX_CALM calm console NPCs at once (Spawns counts them), so a fight's spawns keep half a game's
// share. Nothing new travels: a console NPC is a plain spawn (Protocol::SpawnKind::kPlain).
//
// Pure, so it is tested.
namespace ConsoleSpawns
{
	using Clock = std::chrono::steady_clock;
	using RemoteEdits::Spot;

	// The NPC stands (its 3D in) within this of the command: its model loads in the background.
	inline constexpr auto APPEARS_WITHIN = std::chrono::seconds(10);
	// ...and within this of where the command's reference stood: placed at it or a little off (the command's distance
	// argument), and a calm NPC walks a few hundred units in that time.
	inline constexpr float PLACED_WITHIN = 1024.0f;
	// Commands remembered at once (a batch file's worth); past them the oldest goes.
	inline constexpr std::size_t MAX_COMMANDS = 16;
	// A runtime NPC not seen by a scan for this long is new again when it next is.
	inline constexpr auto SEEN_KEPT = std::chrono::minutes(10);
	inline constexpr std::size_t MAX_SEEN = 2048;
	// The console's NPCs remembered (till a load or the session's end); past them the one seen longest ago goes.
	inline constexpr std::size_t MAX_MADE = 256;
	// Calm console NPCs one game shares at once: half its spawns, as for an encounter's (Encounters::MAX_CALM).
	inline constexpr std::size_t MAX_CALM = 16;

	class Book
	{
	public:
		// The console ran a place command on a reference standing at a_at.
		void Ran(const Spot& a_at, Clock::time_point a_now)
		{
			Expire(a_now);
			if (commands.size() >= MAX_COMMANDS) {
				commands.erase(commands.begin());
			}
			commands.push_back({ a_at, a_now });
			++ran;
		}

		// The scan sees one of our game's runtime NPCs standing (every scan, each one): whether the console placed it.
		bool Seen(std::uint32_t a_id, std::uint32_t a_base, const Spot& a_at, Clock::time_point a_now)
		{
			if (const auto it = made.find(a_id); it != made.end()) {
				if (it->second.base == a_base) {
					it->second.seen = a_now;
					return true;
				}
				made.erase(it);  // the id names another NPC now
			}
			if (const auto it = seen.find(a_id); it != seen.end() && it->second.base == a_base && a_now - it->second.seen < SEEN_KEPT) {
				it->second.seen = a_now;
				return false;
			}
			const bool placed = std::ranges::any_of(commands, [&](const Command& a_command) {
				return a_now - a_command.at < APPEARS_WITHIN && RemoteEdits::Within(RemoteEdits::Distance(a_command.spot, a_at), PLACED_WITHIN);
			});
			if (!placed) {
				if (seen.size() >= MAX_SEEN && !seen.contains(a_id)) {
					Expire(a_now);
					if (seen.size() >= MAX_SEEN) {
						seen.erase(Oldest(seen));
					}
				}
				seen[a_id] = { a_base, a_now };
				return false;
			}
			seen.erase(a_id);
			if (made.size() >= MAX_MADE) {
				made.erase(Oldest(made));
			}
			made[a_id] = { a_base, a_now };
			++found;
			return true;
		}

		// Whether the id is one the console placed, as last seen.
		[[nodiscard]] bool Made(std::uint32_t a_id) const { return made.contains(a_id); }

		// Commands too old to place anything, and NPCs not seen for SEEN_KEPT, go.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(commands, [&](const Command& a_command) { return a_now - a_command.at >= APPEARS_WITHIN; });
			std::erase_if(seen, [&](const auto& a_entry) { return a_now - a_entry.second.seen >= SEEN_KEPT; });
		}

		// A load or the session's end: the ids name other references from here on.
		void Clear()
		{
			commands.clear();
			seen.clear();
			made.clear();
		}

		[[nodiscard]] std::size_t   Size() const { return made.size(); }
		[[nodiscard]] std::size_t   Waiting() const { return commands.size(); }
		[[nodiscard]] std::uint32_t Ran() const { return ran; }
		[[nodiscard]] std::uint32_t Found() const { return found; }

	private:
		struct Command
		{
			Spot              spot;
			Clock::time_point at{};
		};
		struct Npc
		{
			std::uint32_t     base = 0;
			Clock::time_point seen{};
		};
		using Npcs = std::unordered_map<std::uint32_t, Npc>;

		static Npcs::iterator Oldest(Npcs& a_npcs)
		{
			return std::ranges::min_element(a_npcs, {}, [](const auto& a_entry) { return a_entry.second.seen; });
		}

		std::vector<Command> commands;
		Npcs                 seen;  // runtime NPCs the scan saw that the console did not place
		Npcs                 made;  // the console's
		std::uint32_t        ran = 0, found = 0;
	};
}
