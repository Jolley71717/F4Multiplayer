#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// A friend's stage that turns a faction on the player (StageRules::HOSTILE: a kick-out, an ending, a story stage that
// sets a place's people on the player) waits in this game until none of that faction is near, and is then set; it is
// never skipped (the maintainer's rule, 2026-11-11, in place of "set only beside the friend, else skipped"). What
// "near" is, and the wait's bookkeeping, kept free of the game so it can be tested: QuestSync reads the actors, judges
// the stage (StageRules::Judge, Verdict::kHostileWaits) and writes the log.
namespace HostileHold
{
	using Clock = std::chrono::steady_clock;

	// A member of the faction is near when it counts (Counts) and is within RANGE game units of the player (two cells,
	// about 115 m), read among the actors the game simulates fully (its high process: the loaded cells around the
	// player outdoors, or the interior they stand in). One that near comes at the player the moment the faction turns;
	// one farther, or in a cell that isn't loaded, doesn't know. A fight with anyone else is not "near": QuestSync's
	// capped combat hold (MAX_COMBAT_HOLD) holds the stage through it, as it holds any caught-up stage, and a member of
	// the faction who is fighting the player is near by distance anyway.
	inline constexpr float RANGE = 8192.0f;

	// Whether a member of the faction counts at all: a dead one or a disabled one doesn't, nor a friend's stand-in (the
	// caller leaves those out), nor one following a player: the player's own companion (the game's CurrentCompanionFaction
	// mark, NpcSync::IsOurCompanion: Danse in the Brotherhood's faction, Deacon in the Railroad's, X6-88 in the Institute's;
	// always beside the player, so the kick-out would wait for as long as they follow, and the kick-out's own script, run here
	// for the three kick-outs that move the player (StageRules::KICKOUTS), dismisses them) or a friend's (the server's word on
	// whose the NPC is, Companions::FollowsFriend: a friend's Danse beside us is theirs, not a member about to turn).
	inline bool Counts(bool a_alive, bool a_disabled, bool a_followsPlayer)
	{
		return a_alive && !a_disabled && !a_followsPlayer;
	}

	// A hostility call (QuestSync::TurnOnPlayer) the VM wouldn't queue (a load on, no handle yet) is tried again at the next
	// poll this many times, the stage's quest waiting meanwhile, before it is given up on. Never for a faction not in this
	// game or a call that needs the stage's actor: those are skipped at once.
	inline constexpr unsigned CALL_TRIES = 10;

	inline bool Near(float a_dx, float a_dy, float a_dz)
	{
		return a_dx * a_dx + a_dy * a_dy + a_dz * a_dz <= RANGE * RANGE;
	}

	// How long a held stage has waited, in play (time paused or loading doesn't count: no actor moves then), and when to
	// say so: a line in the log as the hold begins, one every LOG_EVERY of play after (a player standing among the faction
	// for an hour sees in the log why the quest doesn't move), and the whole wait when the stage lands; and one HUD line
	// once a hold has lasted NOTICE_AFTER of play, so a player who stays on the Prydwen knows why their quest doesn't
	// move. A hold has no end of its own: the maintainer's rule is that the stage waits, however long, and is never
	// skipped.
	inline constexpr auto LOG_EVERY = std::chrono::minutes(5);
	inline constexpr auto NOTICE_AFTER = std::chrono::minutes(3);

	class Waits
	{
	public:
		// A stage judged held now.
		struct Judged
		{
			bool            begun = false;      // the first time: the hold begins
			bool            logDue = false;     // a line is due (begun, or LOG_EVERY of play since the last)
			bool            noticeDue = false;  // the HUD line is due (NOTICE_AFTER of play, once)
			Clock::duration played{};           // how long it has waited so far
		};

		// Each poll: the play since the last one is added to every held stage, unless the game was paused meanwhile.
		void Tick(Clock::time_point a_now, bool a_paused)
		{
			const auto gap = last ? a_now - *last : Clock::duration::zero();
			last = a_now;
			if (a_paused) {
				return;
			}
			for (auto& [key, held] : stages) {
				held.played += gap;
			}
		}

		// a_faction: the faction near now (the one with the most near, of a stage that turns several), for the landing line.
		Judged Hold(std::uint64_t a_key, std::uint32_t a_faction = 0)
		{
			const auto [it, inserted] = stages.try_emplace(a_key);
			auto&      held = it->second;
			held.faction = a_faction;
			const bool due = inserted || held.played - held.logged >= LOG_EVERY;
			if (due) {
				held.logged = held.played;
			}
			const bool notice = !held.noticed && held.played >= NOTICE_AFTER;
			if (notice) {
				held.noticed = true;
			}
			return { inserted, due, notice, held.played };
		}

		// The stage was set, skipped or dropped: how long it waited and the faction it last waited for, if it was held at all.
		struct Ended
		{
			Clock::duration played{};
			std::uint32_t   faction = 0;
		};
		std::optional<Ended> End(std::uint64_t a_key)
		{
			const auto it = stages.find(a_key);
			if (it == stages.end()) {
				return std::nullopt;
			}
			const Ended ended{ it->second.played, it->second.faction };
			stages.erase(it);
			return ended;
		}

		// Drops the held stages a_stillWaiting(key) says no (their quest went away: dropped, done by the player, finished).
		template <class StillWaiting>
		void Keep(StillWaiting a_stillWaiting)
		{
			std::erase_if(stages, [&](const auto& a_entry) { return !a_stillWaiting(a_entry.first); });
		}

		[[nodiscard]] bool        Holds(std::uint64_t a_key) const { return stages.contains(a_key); }
		[[nodiscard]] std::size_t Size() const { return stages.size(); }

		void Clear()
		{
			stages.clear();
			last.reset();
		}

	private:
		struct Held
		{
			Clock::duration played{};
			Clock::duration logged{};  // played as of the last line
			std::uint32_t   faction = 0;  // the faction last near
			bool            noticed = false;
		};

		std::unordered_map<std::uint64_t, Held> stages;
		std::optional<Clock::time_point>        last;
	};
}
