#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// Our copy of an NPC another game runs has its health there: the owner's game deals what the NPC takes, our player's
// hits are reported (WorldSync's health reports, then the server's belief in our kill), and another player's hits come
// as their health reports. Nothing else in our world is to hurt the copy. In the live test of the explosion ghosts a
// host's raider kept its 30 in the host's game, while the friend's copy went 30, 26, 21, 19, 12, 11 between the
// explosions it was a ghost for, and then lay dead at 0: something in the friend's world wore it down where the owner's
// game did not (what, CopyWatch's log of hits on copies names), and its death there was never believed.
//
// So a copy that loses health with no hit of our player's on it (from OURS_GRACE before the drop on) is given it back
// (CopyWatch, each frame). A drop is judged only once it has stood for JUDGE_AFTER, so that our player's hit, whose event
// can come a frame after its damage, is never taken back; what wears a copy down a little at a time (a fire, a bleed)
// is given back before it adds up. A lethal blow in one go is not given back (the explosions of friends' throws are
// passed by: BlastShield). Health a report sets, or a copy gains, is where it stands from then on. Pure, so it can be
// tested without the game.
namespace CopyHealth
{
	using Clock = std::chrono::steady_clock;

	constexpr Clock::duration OURS_GRACE = std::chrono::milliseconds{ 1000 };
	constexpr Clock::duration JUDGE_AFTER = std::chrono::milliseconds{ 100 };
	constexpr float           SLACK = 0.05f;      // smaller drops are rounding
	constexpr std::size_t     MAX_COPIES = 1024;  // copies watched at once (a guard)

	class Book
	{
	public:
		// Our player hit the copy now.
		void OurHit(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.ourHit = a_now;
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .health = std::nullopt, .ourHit = a_now };
			}
		}

		// The copy's health is a_health now by a report, or as first seen: where it stands.
		void Set(std::uint32_t a_id, float a_health)
		{
			if (auto it = entries.find(a_id); it != entries.end()) {
				it->second.health = a_health;
				it->second.dropSince.reset();
			} else if (entries.size() < MAX_COPIES) {
				entries[a_id] = Entry{ .health = a_health };
			}
		}

		// The copy reads a_health now: how much to give back (0: none, or not yet). After giving it back, Set what it reads.
		float Check(std::uint32_t a_id, float a_health, Clock::time_point a_now)
		{
			auto it = entries.find(a_id);
			if (it == entries.end() || !it->second.health) {
				Set(a_id, a_health);
				return 0.0f;
			}
			auto& entry = it->second;
			if (a_health >= *entry.health - SLACK) {
				entry.health = a_health;
				entry.dropSince.reset();
				return 0.0f;
			}
			if (!entry.dropSince) {
				entry.dropSince = a_now;
			}
			// Our player's hit around the drop: the copy is ours to hurt.
			if (entry.ourHit && *entry.ourHit + OURS_GRACE >= *entry.dropSince) {
				entry.health = a_health;
				entry.dropSince.reset();
				return 0.0f;
			}
			if (a_now - *entry.dropSince < JUDGE_AFTER) {
				return 0.0f;
			}
			entry.dropSince.reset();
			return *entry.health - a_health;
		}

		// The health the copy stands at (none before it is first seen).
		[[nodiscard]] std::optional<float> Health(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() ? it->second.health : std::nullopt;
		}

		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		// Forgets every copy a_keep says no to (no longer a copy of ours, or gone).
		template <class Keep>
		void Prune(Keep a_keep)
		{
			std::erase_if(entries, [&](const auto& a_entry) { return !a_keep(a_entry.first); });
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		struct Entry
		{
			std::optional<float>             health;
			std::optional<Clock::time_point> ourHit;
			std::optional<Clock::time_point> dropSince;
		};

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
