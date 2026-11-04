#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// The health of the NPCs hurt in the session, as the games that run them last reported it (kReportHealth), for a game
// that joins, rejoins or loads a save. Each health report used to be passed on and forgotten, and a game that runs an
// NPC reports it only when it changes (src/game/HealthShare.h), so a game that came in later had its copies of the
// hurt ones at its own save's health, or a fresh cell's, until the NPC's health next moved: rejoin after a crash
// mid-fight and the raiders stood at full health in that game, and a copy its save had lower than the NPC could be
// killed by a hit the NPC would have taken.
//
// So the server keeps the last word on each NPC hurt here, as it keeps what each one wears (npcEquipment), and sends
// it with the world state (on joining and after a load) to every game but the one that runs the NPC; that game's
// copy waits for its NPC to load (WorldSync's pendingHealth), as a live report does. Only NPCs hurt in the session: a
// report of full health from a game that comes to run an NPC (HealthShare's first readings, every NPC it loads) is
// kept only for one already kept, which healed (its health in a joiner's older save may be lower). An NPC leaves it
// when it dies (a death is shared as one), when nobody runs it any more (released, or its game left: whoever runs it
// next reports its own reading, and a cell reset may have healed it meanwhile), and with the session. At most MAX_KEPT
// (under the client's MAX_PENDING_HEALTH, past which a game drops every report still waiting); past it the oldest
// healed one goes first, then the oldest. Pure, so it can be tested without the server.
namespace NpcHealths
{
	constexpr std::size_t MAX_KEPT = 256;

	class Book
	{
	public:
		// The game that runs a_ref said its health is a_fraction of its maximum (and the server passed that on).
		void Heard(std::uint32_t a_ref, float a_fraction)
		{
			if (!std::isfinite(a_fraction)) {
				return;
			}
			const float fraction = std::clamp(a_fraction, 0.0f, 1.0f);
			if (const auto it = entries.find(a_ref); it != entries.end()) {
				it->second = Entry{ fraction, ++order };
				return;
			}
			if (fraction >= 1.0f) {
				return;  // never hurt here: the joiner's own reading is as good
			}
			if (entries.size() >= MAX_KEPT) {
				Evict();
			}
			entries.emplace(a_ref, Entry{ fraction, ++order });
		}

		void Forget(std::uint32_t a_ref) { entries.erase(a_ref); }
		void Clear() { entries.clear(); }

		// a_visit(ref, fraction) for each kept.
		template <class Visit>
		void ForEach(Visit a_visit) const
		{
			for (const auto& [ref, entry] : entries) {
				a_visit(ref, entry.fraction);
			}
		}

		[[nodiscard]] std::size_t Size() const { return entries.size(); }
		[[nodiscard]] bool        Contains(std::uint32_t a_ref) const { return entries.contains(a_ref); }

	private:
		struct Entry
		{
			float         fraction = 1.0f;
			std::uint64_t at = 0;  // the order it was last heard in
		};

		// Makes room for one: the oldest healed back to full, else the oldest.
		void Evict()
		{
			auto victim = entries.end();
			for (auto it = entries.begin(); it != entries.end(); ++it) {
				const bool healed = it->second.fraction >= 1.0f;
				if (victim == entries.end()) {
					victim = it;
					continue;
				}
				const bool victimHealed = victim->second.fraction >= 1.0f;
				if (healed != victimHealed ? healed : it->second.at < victim->second.at) {
					victim = it;
				}
			}
			if (victim != entries.end()) {
				entries.erase(victim);
			}
		}

		std::unordered_map<std::uint32_t, Entry> entries;
		std::uint64_t                            order = 0;
	};
}
