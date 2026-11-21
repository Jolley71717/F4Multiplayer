#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// What a death effect made of a body, made the same in every game (the game side: src/game/BodyStates.h). Kept free of
// the game so it can be tested.
//
// A weapon's critical kill can change the body: a laser's leaves an ash pile, a plasma's a goo pile, a cryo's a frozen
// body. The engine does this with a magic effect on the weapon's critical spell, whose script runs in the game whose hit
// killed, and only there: it sets the actor's critical stage (ACTOR_CRITICAL_STAGE: a start stage while the effect plays,
// the end stage after), attaches a pile (an activator that opens the dead actor's inventory) and makes the body unseen.
// The other games learn of the death and kill their own body of that actor with the console, which runs no such effect:
// one player loots a pile, the other a whole body lying where it fell (a player's report, 0.20.4).
//
// So each game tells what its dead bodies show (Protocol::BodyState: the stage and the pile's base), once the effect has
// ended, and makes of its own body what another game told and its own doesn't show: the start stage, the pile, the body
// unseen, the end stage, as the game's scripts do. Never the other way: a body is never put back, and a game never kills
// by it (what is told waits until our body of that actor lies dead here). Covered, by the stage: disintegrated (lasers,
// also the pale blue ones and Liberty Prime's), turned to goo (plasma, a mirelurk queen's acid) and frozen (cryo), with
// any pile base, a mod's too (one this game lacks is told as none, and the game's own for the stage is used). Not covered:
// lost limbs, and the burnt look of a fire's critical kill (an armor the script puts on the body).
//
// Two games that each made something else of the same body (both killed it at once, each with its own effect) keep their
// own: the first a game knows stands.
namespace BodyEffects
{
	using Clock = std::chrono::steady_clock;

	// The engine's critical stages (RE::ACTOR_CRITICAL_STAGE): the odd ones are an effect still playing.
	constexpr std::uint8_t GOO_START = 1;
	constexpr std::uint8_t GOO_END = 2;
	constexpr std::uint8_t DISINTEGRATE_START = 3;
	constexpr std::uint8_t DISINTEGRATE_END = 4;
	constexpr std::uint8_t FREEZE_START = 5;
	constexpr std::uint8_t FREEZE_END = 6;

	// The game's own piles (Fallout4.esm: AshPile01, GooPile01), for a pile base this game doesn't have.
	constexpr std::uint32_t ASH_PILE = 0x0009142E;
	constexpr std::uint32_t GOO_PILE = 0x00139F8D;

	constexpr auto          RETRY_AFTER = std::chrono::seconds(2);
	constexpr std::uint32_t TRIES = 3;
	constexpr std::size_t   MAX = 256;

	struct State
	{
		std::uint8_t  stage = 0;  // the body's critical stage, 0 for none
		std::uint32_t pile = 0;   // the base of the pile attached to it, 0 for none

		constexpr bool Any() const { return stage != 0 || pile != 0; }
		constexpr bool operator==(const State&) const = default;
	};

	// An effect that has ended: what a game tells, and what another makes of its body.
	constexpr bool Ended(std::uint8_t a_stage)
	{
		return a_stage == GOO_END || a_stage == DISINTEGRATE_END || a_stage == FREEZE_END;
	}

	// An effect still playing (its script has more to do): waited out.
	constexpr bool Running(std::uint8_t a_stage)
	{
		return a_stage == GOO_START || a_stage == DISINTEGRATE_START || a_stage == FREEZE_START;
	}

	// The stage an effect starts at, for the one it ends at (0 for any other).
	constexpr std::uint8_t StartOf(std::uint8_t a_end)
	{
		return Ended(a_end) ? static_cast<std::uint8_t>(a_end - 1) : std::uint8_t{ 0 };
	}

	// The body is unseen at this stage, and a pile stands in for it (what it carried is looted from the pile).
	constexpr bool LeavesPile(std::uint8_t a_stage)
	{
		return a_stage == GOO_END || a_stage == DISINTEGRATE_END;
	}

	// The game's own pile for a stage, 0 for a stage that leaves none.
	constexpr std::uint32_t DefaultPile(std::uint8_t a_stage)
	{
		return a_stage == DISINTEGRATE_END ? ASH_PILE : a_stage == GOO_END ? GOO_PILE : 0u;
	}

	// What was heard, as a game can make it: a body that is unseen always has a pile (the teller's base was one this game
	// lacks, or it couldn't read its own), or what the body carried could not be looted here.
	constexpr State Whole(State a_state)
	{
		if (a_state.pile == 0) {
			a_state.pile = DefaultPile(a_state.stage);
		}
		return a_state;
	}

	// What is known of a body once a_later is added to a_first: the first stage stands, and a pile is only added to a body
	// of the same stage (or to one with none).
	constexpr State Merged(const State& a_first, const State& a_later)
	{
		if (!a_first.Any()) {
			return a_later;
		}
		State out = a_first;
		if (out.pile == 0 && (a_later.stage == out.stage || out.stage == 0)) {
			out.pile = a_later.pile;
		}
		if (out.stage == 0) {
			out.stage = a_later.stage;
		}
		return out;
	}

	// Whether a_new says something a_known doesn't hold yet.
	constexpr bool Adds(const State& a_new, const State& a_known)
	{
		return Merged(a_known, a_new) != a_known;
	}

	// What our body, showing a_have, still needs to show a_want: the stage only for a body with none (an ended effect; a
	// body is never taken from one stage to another), the pile only for a body with none that is, or is about to be, at
	// the pile's stage.
	constexpr State Needed(const State& a_want, const State& a_have)
	{
		State need;
		if (a_have.stage == 0 && Ended(a_want.stage)) {
			need.stage = a_want.stage;
		}
		const bool stageFits = a_have.stage == a_want.stage || need.stage != 0;
		if (a_have.pile == 0 && a_want.pile != 0 && stageFits) {
			need.pile = a_want.pile;
		}
		return need;
	}

	enum class Verdict
	{
		kNone,    // nothing is wanted of it, or it shows it all
		kWait,    // not now: its own effect still plays, or a try was made less than RETRY_AFTER ago
		kApply,   // make this of it now (a try is counted)
		kDone,    // it shows what was wanted, after our tries (said once)
		kGiveUp,  // TRIES tries left it short (said once)
	};

	struct Step
	{
		Verdict       verdict = Verdict::kNone;
		State         what;         // kApply: what to make of it (a stage to end at, a pile to attach); kGiveUp: what it lacks
		std::uint32_t attempt = 0;  // kApply, kDone, kGiveUp: the tries so far, this one included
	};

	// The dead bodies we know of, by the id the wire uses for the actor (a plugin's reference, or a spawn id): what the
	// games already know of each (told by us, or heard), and what was heard that ours is to show. At most MAX, the one
	// least lately told or heard of forgotten first.
	class Book
	{
	public:
		// Our body shows a_local now (its effect ended): what to tell the others, all of it, or nothing when they know.
		std::optional<State> Tell(std::uint32_t a_id, const State& a_local, Clock::time_point a_now)
		{
			if (a_id == 0 || !a_local.Any() || Running(a_local.stage)) {
				return std::nullopt;
			}
			const auto it = entries.find(a_id);
			if (!Adds(a_local, it != entries.end() ? it->second.known : State{})) {
				return std::nullopt;
			}
			auto& entry = it != entries.end() ? it->second : Make(a_id);
			entry.known = Merged(entry.known, a_local);
			entry.touched = a_now;
			return a_local;
		}

		// Another game told what its body shows. True if ours is now wanted to show more than was wanted before.
		bool Heard(std::uint32_t a_id, const State& a_state, Clock::time_point a_now)
		{
			const auto state = Whole(a_state);
			if (a_id == 0 || !state.Any() || Running(state.stage)) {
				return false;
			}
			const auto it = entries.find(a_id);
			auto&      entry = it != entries.end() ? it->second : Make(a_id);
			entry.touched = a_now;
			entry.known = Merged(entry.known, state);
			const auto want = Merged(entry.want, state);
			if (want == entry.want) {
				return false;
			}
			entry.want = want;
			entry.tries = 0;
			entry.gaveUp = false;
			return true;
		}

		// Our body of it lies dead here, loaded, and shows a_local: what to do about what was heard.
		Step Apply(std::uint32_t a_id, const State& a_local, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end() || !it->second.want.Any()) {
				return {};
			}
			auto& entry = it->second;
			auto  need = Needed(entry.want, a_local);
			if (Running(a_local.stage)) {
				// Its own effect plays (its script ends it), unless it is our own try that left it at the start: ended now.
				if (entry.tries == 0 || a_local.stage != StartOf(entry.want.stage)) {
					return { Verdict::kWait };
				}
				need.stage = entry.want.stage;
				if (a_local.pile == 0) {
					need.pile = entry.want.pile;
				}
			}
			if (!need.Any()) {
				if (entry.tries == 0 || entry.gaveUp) {
					return {};
				}
				const auto tries = entry.tries;
				entry.tries = 0;
				return { Verdict::kDone, {}, tries };
			}
			if (entry.gaveUp) {
				return {};
			}
			if (entry.tries > 0 && a_now - entry.tried < RETRY_AFTER) {
				return { Verdict::kWait };
			}
			if (entry.tries >= TRIES) {
				entry.gaveUp = true;
				return { Verdict::kGiveUp, need, entry.tries };
			}
			++entry.tries;
			entry.tried = a_now;
			return { Verdict::kApply, need, entry.tries };
		}

		// What was heard for it that ours is to show (nothing: none).
		State Wanted(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() ? it->second.want : State{};
		}

		// What the games know of it (told by us, or heard).
		State Known(std::uint32_t a_id) const
		{
			const auto it = entries.find(a_id);
			return it != entries.end() ? it->second.known : State{};
		}

		// Someone joined, who knows none of it: every body is told again as we see it. What was heard still stands.
		void ForgetTold()
		{
			for (auto& [id, entry] : entries) {
				entry.known = {};
			}
		}

		// A game was loaded here: our bodies are as the save had them, so all that was heard is wanted of them anew (the
		// ids, for the caller to look at again), and what we see is told again.
		std::vector<std::uint32_t> WantedAgain()
		{
			std::vector<std::uint32_t> ids;
			for (auto& [id, entry] : entries) {
				entry.known = entry.want;
				entry.seen = {};
				entry.tries = 0;
				entry.gaveUp = false;
				if (entry.want.Any()) {
					ids.push_back(id);
				}
			}
			return ids;
		}

		void Clear() { entries.clear(); }

		std::size_t Size() const { return entries.size(); }

		// The bodies something is still wanted of that we haven't given up on (ours may not be loaded, or not dead yet).
		std::size_t Waiting() const
		{
			std::size_t count = 0;
			for (const auto& [id, entry] : entries) {
				count += entry.want.Any() && !entry.gaveUp && entry.tries == 0 && Adds(entry.want, entry.seen) ? 1u : 0u;
			}
			return count;
		}

		// Our body of it was seen showing a_local (for Waiting: what was wanted and is shown waits no more).
		void Seen(std::uint32_t a_id, const State& a_local)
		{
			if (const auto it = entries.find(a_id); it != entries.end()) {
				it->second.seen = a_local;
			}
		}

	private:
		struct Entry
		{
			State             known;  // what the games know: told by us, or heard
			State             want;   // what was heard: ours is to show it
			State             seen;   // what ours last showed
			std::uint32_t     tries = 0;
			bool              gaveUp = false;
			Clock::time_point tried{};
			Clock::time_point touched{};
		};

		Entry& Make(std::uint32_t a_id)
		{
			if (entries.size() >= MAX) {
				auto oldest = entries.begin();
				for (auto it = entries.begin(); it != entries.end(); ++it) {
					if (it->second.touched < oldest->second.touched) {
						oldest = it;
					}
				}
				entries.erase(oldest);
			}
			return entries[a_id];
		}

		std::unordered_map<std::uint32_t, Entry> entries;
	};
}
